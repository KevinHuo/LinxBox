package service

import (
	"encoding/json"
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

func TestExecutionIncompleteCompletionCanRecoverAndReleaseDevice(t *testing.T) {
	for _, mode := range []string{"null", "inconsistent", "legacy_after_restart"} {
		t.Run(mode, func(t *testing.T) {
			e, d := newExecutionEnv(t)
			x := confirmExecution(t, e)
			ctx := t.Context()
			if err := e.s.ApplyTask(ctx, "desktop-1", 1, executionEvent(x, "task.started", 1)); err != nil {
				t.Fatal(err)
			}
			missing := mutateEvent(executionEvent(x, "task.completed", 2), func(p *contracts.TaskEventPayload) {
				p.Result = json.RawMessage("null")
				if mode == "inconsistent" {
					p.Result = json.RawMessage(`{"status":"failed"}`)
				}
			})
			if err := e.s.ApplyTask(ctx, "desktop-1", 1, missing); err != nil {
				t.Fatal(err)
			}
			requestStatus(t, e, "unknown")
			if mode == "legacy_after_restart" {
				// 模拟旧版本已持久化的错误屏障，真实关闭并重开数据库。
				if _, err := e.db.DB().Exec(`UPDATE request_executions SET terminal=1 WHERE request_id='req'`); err != nil {
					t.Fatal(err)
				}
				var index int
				var name, path string
				if err := e.db.DB().QueryRow(`PRAGMA database_list`).Scan(&index, &name, &path); err != nil {
					t.Fatal(err)
				}
				if err := e.db.Close(); err != nil {
					t.Fatal(err)
				}
				db, err := storage.Open(path)
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() { db.Close() })
				e.db = db
				e.s = NewRequestService(storage.NewRepositories(db.DB()), e.s.cfg, e.n)
				e.s.dispatcher = d
				if err := e.s.Recover(ctx); err != nil {
					t.Fatal(err)
				}
			} else {
				saved, err := e.s.executions.ByRequest(ctx, e.db.DB(), "req")
				if err != nil || saved.Terminal {
					t.Fatalf("缺少结果不能形成终态屏障: %+v %v", saved, err)
				}
			}
			_, err := e.s.Prepare(ctx, requestDevice, "new", "other")
			requestCode(t, err, domain.CodeConflict)
			_, err = e.s.Get(ctx, requestDevice, "new")
			requestCode(t, err, domain.CodeNotFound)
			// 重复确认及重复原事件均不重新投递、不重复推送。
			count := len(e.n.events)
			if _, err := e.s.Confirm(ctx, requestDevice, "req"); err != nil {
				t.Fatal(err)
			}
			if err := e.s.ApplyTask(ctx, "desktop-1", 2, missing); err != nil {
				t.Fatal(err)
			}
			if len(e.n.events) != count || d.sends != 1 {
				t.Fatal("等待补报时发生额外投递或推送")
			}
			// completed 已声明结束，后续 started/progress 不能把它回退为运行中。
			requestCode(t, e.s.ApplyTask(ctx, "desktop-1", 2, executionEvent(x, "task.started", 3)), domain.CodeConflict)
			requestCode(t, e.s.ApplyTask(ctx, "desktop-1", 2, executionEvent(x, "task.progress", 3)), domain.CodeConflict)
			fixed := executionEvent(x, "task.completed", 3)
			requestCode(t, e.s.ApplyTask(ctx, "other", 2, fixed), domain.CodePermissionDenied)
			requestCode(t, e.s.ApplyTask(ctx, "desktop-1", 2, mutateEvent(fixed, func(p *contracts.TaskEventPayload) { v := "other"; p.ThreadID = &v })), domain.CodeConflict)
			requestCode(t, e.s.ApplyTask(ctx, "desktop-1", 2, mutateEvent(fixed, func(p *contracts.TaskEventPayload) { v := "other"; p.TurnID = &v })), domain.CodeConflict)
			requestCode(t, e.s.ApplyTask(ctx, "desktop-1", 2, mutateEvent(fixed, func(p *contracts.TaskEventPayload) { p.EventSequence = 2 })), domain.CodeConflict)
			var original contracts.TaskEventPayload
			if err := json.Unmarshal(missing.Payload, &original); err != nil {
				t.Fatal(err)
			}
			requestCode(t, e.s.ApplyTask(ctx, "desktop-1", 2, mutateEvent(fixed, func(p *contracts.TaskEventPayload) { p.EventID = original.EventID })), domain.CodeConflict)
			requestStatus(t, e, "unknown")
			if err := e.s.ApplyTask(ctx, "desktop-1", 2, fixed); err != nil {
				t.Fatal(err)
			}
			requestStatus(t, e, "completed")
			count = len(e.n.events)
			if err := e.s.ApplyTask(ctx, "desktop-1", 2, fixed); err != nil {
				t.Fatal(err)
			}
			if len(e.n.events) != count || d.sends != 1 {
				t.Fatal("恢复结果不幂等或重新投递")
			}
			if _, err := e.s.Prepare(ctx, requestDevice, "new", "other"); err != nil {
				t.Fatalf("原任务完成后设备仍被占用: %v", err)
			}
		})
	}
}

func TestExecutionVerifiedTerminalCannotBeOverridden(t *testing.T) {
	for _, typ := range []string{"task.completed", "task.failed", "task.cancelled", "task.interrupted"} {
		t.Run(typ, func(t *testing.T) {
			e, _ := newExecutionEnv(t)
			x := confirmExecution(t, e)
			if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, executionEvent(x, typ, 1)); err != nil {
				t.Fatal(err)
			}
			requestCode(t, e.s.ApplyTask(t.Context(), "desktop-1", 1, executionEvent(x, "task.completed", 2)), domain.CodeConflict)
		})
	}
}

func TestExecutionIncompleteCompletionCanResolveWithFailure(t *testing.T) {
	e, _ := newExecutionEnv(t)
	x := confirmExecution(t, e)
	missing := mutateEvent(executionEvent(x, "task.completed", 1), func(p *contracts.TaskEventPayload) { p.Result = json.RawMessage("null") })
	if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, missing); err != nil {
		t.Fatal(err)
	}
	if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, executionEvent(x, "task.failed", 2)); err != nil {
		t.Fatal(err)
	}
	requestStatus(t, e, "failed")
	if _, err := e.s.Prepare(t.Context(), requestDevice, "new", "other"); err != nil {
		t.Fatal(err)
	}
}
