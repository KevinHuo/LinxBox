package service

import (
	"encoding/json"
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

type fakeTaskDispatcher struct {
	payload             contracts.TaskDispatchPayload
	sends               int
	prepareErr, sendErr error
	beforeSend          func()
}

func (d *fakeTaskDispatcher) PrepareDispatch(_ string, payload contracts.TaskDispatchPayload) (TaskDelivery, error) {
	d.payload = payload
	return TaskDelivery{MessageID: "dispatch", Sequence: 7, Generation: 1, Send: func() error {
		d.sends++
		if d.beforeSend != nil {
			d.beforeSend()
		}
		return d.sendErr
	}}, d.prepareErr
}

func newExecutionEnv(t *testing.T) (*requestEnv, *fakeTaskDispatcher) {
	t.Helper()
	e := newRequestEnv(t)
	d := &fakeTaskDispatcher{}
	e.s.dispatcher = d
	e.prepare(t, "req")
	if _, err := e.propose(t, "req", "  按原文继续🙂\n"); err != nil {
		t.Fatal(err)
	}
	return e, d
}

func confirmExecution(t *testing.T, e *requestEnv) *storage.ExecutionRow {
	t.Helper()
	r, err := e.s.Confirm(t.Context(), requestDevice, "req")
	if err != nil || r.Status != "sending" {
		t.Fatalf("确认失败: %+v %v", r, err)
	}
	x, err := e.s.executions.ByRequest(t.Context(), e.db.DB(), "req")
	if err != nil {
		t.Fatal(err)
	}
	return x
}

func executionEvent(x *storage.ExecutionRow, typ string, sequence int64) contracts.DesktopMessage {
	state := map[string]string{"task.started": "running", "task.progress": "running", "task.completed": "succeeded", "task.failed": "failed", "task.interrupted": "interrupted", "task.cancelled": "cancelled", "task.approval_required": "approval_required"}[typ]
	thread, turn := "thread", "turn"
	p := contracts.TaskEventPayload{TaskID: x.TaskID, EventID: contracts.NewMessageID("evt_"), EventSequence: sequence,
		OccurredAt: time.Now().UTC().Format(time.RFC3339), State: state, Stage: "finished", Summary: "本轮状态", Result: json.RawMessage("null"), ThreadID: &thread, TurnID: &turn}
	if typ == "task.failed" {
		p.Error = &contracts.PublicError{Code: "INTERNAL", Message: "不应透传的 provider 详情 /private/path", RequestID: "req"}
	}
	if typ == "task.completed" {
		p.Result = json.RawMessage(`{"contract_version":"task-result-v1","status":"succeeded","summary":"本轮完成","changes":[],"verification":[],"risks":[],"needs_user_action":null,"voice_summary":"本轮完成"}`)
	}
	raw, _ := json.Marshal(p)
	return contracts.DesktopMessage{Type: typ, Payload: raw}
}

func ackEvent(x *storage.ExecutionRow, disposition string) contracts.DesktopMessage {
	raw, _ := json.Marshal(contracts.TaskAckPayload{TaskID: x.TaskID, MessageID: x.MessageID, Sequence: x.Sequence, Disposition: disposition})
	return contracts.DesktopMessage{Type: "task.ack", Payload: raw}
}

func mutateEvent(msg contracts.DesktopMessage, mutate func(*contracts.TaskEventPayload)) contracts.DesktopMessage {
	var p contracts.TaskEventPayload
	_ = json.Unmarshal(msg.Payload, &p)
	mutate(&p)
	msg.Payload, _ = json.Marshal(p)
	return msg
}

func requestStatus(t *testing.T, e *requestEnv, want string) {
	t.Helper()
	r, err := e.s.Get(t.Context(), requestDevice, "req")
	if err != nil || r.Status != want {
		t.Fatalf("期望 %s，实际 %+v: %v", want, r, err)
	}
}

func TestConfirmPersistBeforeSendAndConcurrentDuplicates(t *testing.T) {
	e, d := newExecutionEnv(t)
	d.beforeSend = func() {
		x, err := e.s.executions.ByRequest(t.Context(), e.db.DB(), "req")
		if err != nil || x.TaskID != d.payload.TaskID {
			t.Errorf("发送前身份未落盘: %+v %v", x, err)
		}
		r, err := e.s.requests.Get(t.Context(), e.db.DB(), "req")
		if err != nil || r.Status != "sending" {
			t.Errorf("发送前状态未落盘: %+v %v", r, err)
		}
	}
	var wg sync.WaitGroup
	for range 12 {
		wg.Go(func() {
			if _, err := e.s.Confirm(t.Context(), requestDevice, "req"); err != nil {
				t.Error(err)
			}
		})
	}
	wg.Wait()
	if d.sends != 1 || d.payload.Continuation.ThreadID != "thread" || d.payload.Continuation.AgentSessionID != "session" || d.payload.Prompt != "  按原文继续🙂\n" {
		t.Fatalf("重复投递或目标/文字改变: %+v", d)
	}
	_, err := e.s.Cancel(t.Context(), requestDevice, "req")
	requestCode(t, err, domain.CodeConflict)
}

func TestConfirmRejectsInvalidPreparationAndKeepsDraft(t *testing.T) {
	for _, tc := range []string{"early", "missing", "foreign", "removed", "rerouted", "busy", "unknown", "offline", "unconfigured", "expired", "cancelled", "database"} {
		t.Run(tc, func(t *testing.T) {
			e, d := newExecutionEnv(t)
			device, id, want := requestDevice, "req", domain.CodeConflict
			switch tc {
			case "early":
				_, _ = e.db.DB().Exec(`UPDATE requests SET status='awaiting_text',text=NULL`)
			case "missing":
				id = "missing"
				want = domain.CodeNotFound
			case "foreign":
				device = "AA:BB:CC:DD:EE:02"
				want = domain.CodePermissionDenied
			case "removed":
				_, _ = e.db.DB().Exec(`DELETE FROM session_catalog`)
				want = domain.CodePermissionDenied
			case "rerouted":
				_, _ = e.db.DB().Exec(`UPDATE session_catalog SET thread_id='other'`)
			case "busy":
				_, _ = e.db.DB().Exec(`UPDATE session_catalog SET status='running'`)
			case "unknown":
				_, _ = e.db.DB().Exec(`UPDATE session_catalog SET status='unknown'`)
			case "offline":
				d.prepareErr = domain.ErrDependencyUnavailable("离线")
				want = domain.CodeDependencyUnavailable
			case "unconfigured":
				e.s.dispatcher = nil
				want = domain.CodeDependencyUnavailable
			case "expired":
				e.now = e.now.Add(301 * time.Second)
			case "cancelled":
				_, _ = e.s.Cancel(t.Context(), requestDevice, "req")
			case "database":
				_, _ = e.db.DB().Exec(`DROP TABLE request_executions`)
				want = domain.CodeInternal
			}
			_, err := e.s.Confirm(t.Context(), device, id)
			requestCode(t, err, want)
			if d.sends != 0 {
				t.Fatal("拒绝后仍然执行")
			}
		})
	}
}

func TestExecutionFactsAndAckOrdering(t *testing.T) {
	e, _ := newExecutionEnv(t)
	x := confirmExecution(t, e)
	ctx := t.Context()
	for _, ack := range []string{"accepted", "duplicate"} {
		if err := e.s.ApplyTask(ctx, "desktop-1", 1, ackEvent(x, ack)); err != nil {
			t.Fatal(err)
		}
	}
	requestStatus(t, e, "sending")
	started := executionEvent(x, "task.started", 1)
	if err := e.s.ApplyTask(ctx, "desktop-1", 1, started); err != nil {
		t.Fatal(err)
	}
	requestStatus(t, e, "running")
	if err := e.s.ApplyTask(ctx, "desktop-1", 1, ackEvent(x, "accepted")); err != nil {
		t.Fatal(err)
	}
	requestStatus(t, e, "running")
	if err := e.s.ApplyTask(ctx, "desktop-1", 1, executionEvent(x, "task.progress", 2)); err != nil {
		t.Fatal(err)
	}
	completed := executionEvent(x, "task.completed", 3)
	if err := e.s.ApplyTask(ctx, "desktop-1", 1, completed); err != nil {
		t.Fatal(err)
	}
	requestStatus(t, e, "completed")
	before := len(e.n.events)
	if err := e.s.ApplyTask(ctx, "desktop-1", 1, completed); err != nil {
		t.Fatal(err)
	}
	if err := e.s.ApplyTask(ctx, "desktop-1", 1, started); err != nil {
		t.Fatal(err)
	}
	if len(e.n.events) != before {
		t.Fatal("重复事实重复推送")
	}
	requestCode(t, e.s.ApplyTask(ctx, "desktop-1", 1, executionEvent(x, "task.failed", 4)), domain.CodeConflict)
	x, err := e.s.executions.ByTask(ctx, e.db.DB(), x.TaskID)
	if err != nil || x.Result == nil || !x.Terminal {
		t.Fatalf("终态结果未保存: %+v %v", x, err)
	}
}

func TestExecutionTerminalMappings(t *testing.T) {
	for _, tc := range []struct{ typ, want string }{
		{"task.failed", "failed"}, {"task.interrupted", "unknown"}, {"task.cancelled", "cancelled"}, {"task.approval_required", "unknown"},
	} {
		t.Run(tc.typ, func(t *testing.T) {
			e, _ := newExecutionEnv(t)
			x := confirmExecution(t, e)
			if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, executionEvent(x, tc.typ, 1)); err != nil {
				t.Fatal(err)
			}
			requestStatus(t, e, tc.want)
			r, _ := e.s.Get(t.Context(), requestDevice, "req")
			if r.Error != nil && r.Error.Message == "不应透传的 provider 详情 /private/path" {
				t.Fatal("原始错误泄露")
			}
		})
	}
	for _, disposition := range []string{"rejected", "expired"} {
		t.Run(disposition, func(t *testing.T) {
			e, _ := newExecutionEnv(t)
			x := confirmExecution(t, e)
			if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, ackEvent(x, disposition)); err != nil {
				t.Fatal(err)
			}
			requestStatus(t, e, "failed")
			requestCode(t, e.s.ApplyTask(t.Context(), "desktop-1", 1, ackEvent(x, "accepted")), domain.CodeConflict)
		})
	}
}

func TestExecutionRejectsUnrelatedAndInconsistentFacts(t *testing.T) {
	for _, tc := range []string{"desktop", "task", "thread", "turn", "state", "result", "partial", "sequence", "id", "ack", "progress", "unknown_type"} {
		t.Run(tc, func(t *testing.T) {
			e, _ := newExecutionEnv(t)
			x := confirmExecution(t, e)
			started := executionEvent(x, "task.started", 1)
			if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, started); err != nil {
				t.Fatal(err)
			}
			msg := executionEvent(x, "task.progress", 2)
			desktop, want := domain.DesktopClientID("desktop-1"), domain.CodeConflict
			switch tc {
			case "desktop":
				desktop = "other"
				want = domain.CodePermissionDenied
			case "task":
				msg = mutateEvent(msg, func(p *contracts.TaskEventPayload) { p.TaskID = "missing" })
				want = domain.CodeNotFound
			case "thread":
				msg = mutateEvent(msg, func(p *contracts.TaskEventPayload) { v := "other"; p.ThreadID = &v })
			case "turn":
				msg = mutateEvent(msg, func(p *contracts.TaskEventPayload) { v := "other"; p.TurnID = &v })
			case "state":
				msg = mutateEvent(msg, func(p *contracts.TaskEventPayload) { p.State = "succeeded" })
			case "result":
				msg = mutateEvent(msg, func(p *contracts.TaskEventPayload) { p.Result = json.RawMessage(`{"status":"succeeded"}`) })
			case "partial":
				msg = mutateEvent(msg, func(p *contracts.TaskEventPayload) { p.ThreadID = nil })
			case "sequence":
				msg = mutateEvent(msg, func(p *contracts.TaskEventPayload) { p.EventSequence = 1 })
			case "id":
				msg = mutateEvent(started, func(p *contracts.TaskEventPayload) { p.Summary = "更换事实" })
			case "ack":
				msg = ackEvent(x, "rejected")
			case "progress":
				msg = mutateEvent(msg, func(p *contracts.TaskEventPayload) { p.TurnID = nil })
			case "unknown_type":
				msg.Type = "task.unrecognized"
			}
			requestCode(t, e.s.ApplyTask(t.Context(), desktop, 1, msg), want)
			requestStatus(t, e, "running")
		})
	}
}

func TestExecutionUnknownRecoveryNeverRedispatches(t *testing.T) {
	e, d := newExecutionEnv(t)
	x := confirmExecution(t, e)
	if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, executionEvent(x, "task.started", 1)); err != nil {
		t.Fatal(err)
	}
	if err := e.s.DesktopDisconnected(t.Context(), "desktop-1", 99); err != nil {
		t.Fatal(err)
	}
	requestStatus(t, e, "running")
	if err := e.s.DesktopDisconnected(t.Context(), "desktop-1", 1); err != nil {
		t.Fatal(err)
	}
	requestStatus(t, e, "unknown")
	if _, err := e.s.Confirm(t.Context(), requestDevice, "req"); err != nil {
		t.Fatal(err)
	}
	if d.sends != 1 {
		t.Fatal("unknown 被重投")
	}
	if err := e.s.ApplyTask(t.Context(), "desktop-1", 2, executionEvent(x, "task.started", 2)); err != nil {
		t.Fatal(err)
	}
	if err := e.s.DesktopDisconnected(t.Context(), "desktop-1", 1); err != nil {
		t.Fatal(err)
	}
	requestStatus(t, e, "running")
	// 真实关闭并重新打开数据库，不能用同一实例的缓存代替持久化证明。
	var databaseIndex int
	var databaseName, databasePath string
	if err := e.db.DB().QueryRow(`PRAGMA database_list`).Scan(&databaseIndex, &databaseName, &databasePath); err != nil {
		t.Fatal(err)
	}
	if err := e.db.Close(); err != nil {
		t.Fatal(err)
	}
	db, err := storage.Open(databasePath)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	e.db = db
	s := NewRequestService(storage.NewRepositories(db.DB()), e.s.cfg, e.n)
	s.dispatcher = d
	if err := s.Recover(t.Context()); err != nil {
		t.Fatal(err)
	}
	e.s = s
	requestStatus(t, e, "unknown")
	if _, err := s.Confirm(t.Context(), requestDevice, "req"); err != nil {
		t.Fatal(err)
	}
	if d.sends != 1 {
		t.Fatal("重启后重新执行")
	}
	if err := s.ApplyTask(t.Context(), "desktop-1", 3, executionEvent(x, "task.completed", 3)); err != nil {
		t.Fatal(err)
	}
	requestStatus(t, e, "completed")
}

func TestExecutionAckCorrelationAndContradiction(t *testing.T) {
	e, _ := newExecutionEnv(t)
	x := confirmExecution(t, e)
	wrong := ackEvent(x, "accepted")
	var ack contracts.TaskAckPayload
	_ = json.Unmarshal(wrong.Payload, &ack)
	ack.Sequence++
	wrong.Payload, _ = json.Marshal(ack)
	requestCode(t, e.s.ApplyTask(t.Context(), "desktop-1", 1, wrong), domain.CodeConflict)
	if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, ackEvent(x, "accepted")); err != nil {
		t.Fatal(err)
	}
	requestCode(t, e.s.ApplyTask(t.Context(), "desktop-1", 1, ackEvent(x, "rejected")), domain.CodeConflict)
	requestStatus(t, e, "sending")
}

func TestConfirmRollbackNeverSends(t *testing.T) {
	e, d := newExecutionEnv(t)
	if _, err := e.db.DB().Exec(`CREATE TRIGGER refuse_confirmation BEFORE INSERT ON request_executions BEGIN SELECT RAISE(ABORT,'storage unavailable'); END`); err != nil {
		t.Fatal(err)
	}
	if _, err := e.s.Confirm(t.Context(), requestDevice, "req"); err == nil {
		t.Fatal("写入失败不能确认成功")
	}
	if d.sends != 0 {
		t.Fatal("事务失败仍投递")
	}
	requestStatus(t, e, "awaiting_confirmation")
}

func TestExecutionSendFailureAndMissingResultStayUnknown(t *testing.T) {
	e, d := newExecutionEnv(t)
	d.sendErr = errors.New("socket failed")
	r, err := e.s.Confirm(t.Context(), requestDevice, "req")
	if err != nil || r.Status != "unknown" {
		t.Fatalf("发送失败不能假报未执行: %+v %v", r, err)
	}
	if _, err := e.s.Confirm(t.Context(), requestDevice, "req"); err != nil {
		t.Fatal(err)
	}
	if d.sends != 1 {
		t.Fatal("发送失败后重投")
	}
	e, _ = newExecutionEnv(t)
	x := confirmExecution(t, e)
	msg := mutateEvent(executionEvent(x, "task.completed", 1), func(p *contracts.TaskEventPayload) { p.Result = json.RawMessage("null") })
	if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, msg); err != nil {
		t.Fatal(err)
	}
	requestStatus(t, e, "unknown")
}

func TestExecutionStartupFailureMayHaveThreadWithoutTurn(t *testing.T) {
	e, _ := newExecutionEnv(t)
	x := confirmExecution(t, e)
	msg := mutateEvent(executionEvent(x, "task.failed", 1), func(p *contracts.TaskEventPayload) { p.TurnID = nil })
	if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, msg); err != nil {
		t.Fatal(err)
	}
	if err := e.s.ApplyTask(t.Context(), "desktop-1", 1, ackEvent(x, "rejected")); err != nil {
		t.Fatal(err)
	}
	requestStatus(t, e, "failed")
}
