package service

import (
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

func TestRequestImplicitSelectionAndIdempotency(t *testing.T) {
	e := newRequestEnv(t)
	call := func(text string) (contracts.Request, error) {
		return e.s.ProposeImplicit(t.Context(), requestDevice, "agent", "voice", text)
	}
	_, err := call("文字")
	requestCode(t, err, domain.CodePermissionDenied)
	e.s.cfg.MCP.AllowImplicitRequestID = true
	_, err = call("文字")
	requestCode(t, err, domain.CodeNotFound)
	e.prepare(t, "req-1")
	_, err = e.s.ProposeImplicit(t.Context(), "AA:BB:CC:DD:EE:02", "agent", "voice", "文字")
	requestCode(t, err, domain.CodeNotFound)
	_, err = e.s.ProposeImplicit(t.Context(), requestDevice, "agent", "", "文字")
	requestCode(t, err, domain.CodeInvalidArgument)
	_, err = call(" \n")
	requestCode(t, err, domain.CodeInvalidArgument)
	text := "  原文🙂\n"
	for range 2 {
		r, err := call(text)
		if err != nil || r.RequestID != "req-1" || r.Status != "awaiting_confirmation" || r.Text == nil || *r.Text != text {
			t.Fatal(r, err)
		}
	}
	if len(e.n.events) != 1 {
		t.Fatal("重复通知", e.n.events)
	}
	_, err = call("改写")
	requestCode(t, err, domain.CodeConflict)
	_, err = e.s.ProposeImplicit(t.Context(), requestDevice, "agent", "other-voice", text)
	requestCode(t, err, domain.CodeConflict)
	_, err = e.s.ProposeImplicit(t.Context(), requestDevice, "other-agent", "voice", text)
	requestCode(t, err, domain.CodeConflict)
	restarted := NewRequestService(storage.NewRepositories(e.db.DB()), e.s.cfg, e.n)
	if r, err := restarted.ProposeImplicit(t.Context(), requestDevice, "agent", "voice", text); err != nil || r.RequestID != "req-1" {
		t.Fatal(r, err)
	}
	if _, err := e.s.Cancel(t.Context(), requestDevice, "req-1"); err != nil {
		t.Fatal(err)
	}
	_, err = call(text)
	requestCode(t, err, domain.CodeNotFound)
	e.prepare(t, "req-2")
	_, err = e.propose(t, "req-1", text)
	requestCode(t, err, domain.CodeConflict)
	if r, err := e.s.Get(t.Context(), requestDevice, "req-2"); err != nil || r.Text != nil {
		t.Fatal("显式旧ID不得回退", r, err)
	}
	if r, err := call("第二轮"); err != nil || r.RequestID != "req-2" {
		t.Fatal(r, err)
	}
}

func TestRequestImplicitExpiryOwnershipAndTarget(t *testing.T) {
	for _, scenario := range []string{"过期", "账号变化", "目标变化", "目录移除"} {
		t.Run(scenario, func(t *testing.T) {
			e := newRequestEnv(t)
			e.s.cfg.MCP.AllowImplicitRequestID = true
			e.prepare(t, "req")
			want := domain.CodePermissionDenied
			switch scenario {
			case "过期":
				e.now = e.now.Add(121 * time.Second)
				want = domain.CodeNotFound
			case "账号变化":
				e.s.cfg.Credentials.Devices[0].Subject = "other-account"
			case "目标变化":
				if _, err := e.db.DB().Exec(`UPDATE session_catalog SET thread_id='changed'`); err != nil {
					t.Fatal(err)
				}
				want = domain.CodeConflict
			case "目录移除":
				if _, err := e.db.DB().Exec(`DELETE FROM session_catalog`); err != nil {
					t.Fatal(err)
				}
			}
			_, err := e.s.ProposeImplicit(t.Context(), requestDevice, "agent", "voice", "文字")
			requestCode(t, err, want)
			row, err := e.s.requests.Get(t.Context(), e.db.DB(), "req")
			if err != nil || row.Text != nil {
				t.Fatal("失败不能保存文字", row, err)
			}
		})
	}
}

func TestRequestImplicitRefusesAmbiguousDevice(t *testing.T) {
	e := newRequestEnv(t)
	e.s.cfg.MCP.AllowImplicitRequestID = true
	e.prepare(t, "req-1")
	// 模拟损坏/旧版数据库；生产唯一索引保持不变，测试验证不会任取一条。
	if _, err := e.db.DB().Exec(`DROP INDEX requests_active_device`); err != nil {
		t.Fatal(err)
	}
	err := e.s.requests.Insert(t.Context(), e.db.DB(), &storage.RequestRow{
		Request:  contracts.Request{RequestID: "req-2", AgentSessionID: "other", TargetTitle: "另一目标", Status: "awaiting_text"},
		DeviceID: string(requestDevice), DesktopID: "desktop-1", ThreadID: "other-thread", ProjectID: "project", ExpiresAt: e.now.Add(time.Minute).UnixMilli(),
	})
	if err != nil {
		t.Fatal(err)
	}
	_, err = e.s.ProposeImplicit(t.Context(), requestDevice, "agent", "voice", "文字")
	requestCode(t, err, domain.CodeConflict)
	for _, id := range []string{"req-1", "req-2"} {
		row, err := e.s.requests.Get(t.Context(), e.db.DB(), id)
		if err != nil || row.Text != nil {
			t.Fatal(row, err)
		}
	}
}
