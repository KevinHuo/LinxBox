package service

import (
	"errors"
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

func TestPrepareConflictIdentifiesBlockingRequests(t *testing.T) {
	for _, mode := range []string{"device", "session", "device_and_session", "two_blockers", "catalog_only"} {
		t.Run(mode, func(t *testing.T) {
			e := newRequestEnv(t)
			if mode != "catalog_only" {
				e.prepare(t, "old")
			}
			device, session := requestDevice, "session"
			switch mode {
			case "device":
				session = "other"
			case "session":
				device = "AA:BB:CC:DD:EE:02"
			case "two_blockers":
				if _, err := e.s.Prepare(t.Context(), "AA:BB:CC:DD:EE:02", "second", "other"); err != nil {
					t.Fatal(err)
				}
				session = "other"
			case "catalog_only":
				if _, err := e.db.DB().Exec(`UPDATE session_catalog SET status='running' WHERE agent_session_id='session'`); err != nil {
					t.Fatal(err)
				}
			}
			_, err := e.s.Prepare(t.Context(), device, "new", session)
			requestCode(t, err, domain.CodeConflict)
			var conflict *RequestConflict
			if !errors.As(err, &conflict) {
				t.Fatal("冲突未保留内部排查信息", err)
			}
			if domain.MessageOf(err) != "设备或目标会话存在未结束请求" {
				t.Fatal("内部诊断不得改变公共错误字段", err)
			}
			want := 1
			if mode == "two_blockers" {
				want = 2
			}
			if mode == "catalog_only" {
				want = 0
			}
			if len(conflict.Blockers) != want {
				t.Fatalf("占用者数量错误: %+v", conflict)
			}
			if mode == "catalog_only" {
				if conflict.TargetStatus != "running" {
					t.Fatal(conflict)
				}
			} else {
				first := conflict.Blockers[0]
				scope := mode
				if mode == "two_blockers" {
					scope = "device"
					if conflict.Blockers[1].RequestID != "second" || conflict.Blockers[1].Scope != "session" {
						t.Fatal(conflict.Blockers)
					}
				}
				if first.RequestID != "old" || first.Status != "awaiting_text" || first.Scope != scope {
					t.Fatal(conflict.Blockers)
				}
			}
			_, err = e.s.Get(t.Context(), device, "new")
			requestCode(t, err, domain.CodeNotFound)
		})
	}
}
