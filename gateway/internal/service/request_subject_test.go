package service

import (
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

func TestMCPSubjectProposals(t *testing.T) {
	for _, implicit := range []bool{false, true} {
		e := newRequestEnv(t)
		e.s.cfg.MCP.AllowImplicitRequestID = implicit
		e.prepare(t, "req-1")
		if _, err := e.s.Prepare(t.Context(), "AA:BB:CC:DD:EE:02", "req-2", "other"); err != nil {
			t.Fatal(err)
		}
		for i, device := range []domain.DeviceID{requestDevice, "AA:BB:CC:DD:EE:02"} {
			want := []string{"req-1", "req-2"}[i]
			var id string
			if implicit {
				r, err := e.s.ProposeImplicitForSubject(t.Context(), device, "alice", "agent", "voice", "原文")
				if err != nil {
					t.Fatal(err)
				}
				id = r.RequestID
			} else {
				r, err := e.s.ProposeForSubject(t.Context(), device, "alice", want, "agent", "voice", "原文")
				if err != nil {
					t.Fatal(err)
				}
				id = r.RequestID
			}
			if id != want {
				t.Fatalf("多设备请求串线：%s != %s", id, want)
			}
		}
	}
}

func TestMCPSubjectDenialsDoNotSaveText(t *testing.T) {
	for _, scenario := range []string{"跨账号", "未登记", "撤销凭据", "账号变化", "无绑定", "串请求", "登记查询失败"} {
		t.Run(scenario, func(t *testing.T) {
			for _, implicit := range []bool{false, true} {
				e := newRequestEnv(t)
				e.s.cfg.MCP.AllowImplicitRequestID = true
				e.prepare(t, "req")
				device, subject, want := requestDevice, domain.Subject("alice"), domain.CodePermissionDenied
				switch scenario {
				case "跨账号":
					subject = "bob"
				case "未登记":
					device = "AA:BB:CC:DD:EE:09"
				case "撤销凭据":
					e.s.cfg.Credentials.Devices[0].Token = "new-token"
				case "账号变化":
					e.s.cfg.Credentials.Devices[0].Subject = "bob"
				case "无绑定":
					if _, err := e.db.DB().Exec(`DELETE FROM bindings`); err != nil {
						t.Fatal(err)
					}
				case "串请求":
					device = "AA:BB:CC:DD:EE:02"
					if implicit {
						want = domain.CodeNotFound
					}
				case "登记查询失败":
					if _, err := e.db.DB().Exec(`DROP TABLE device_registrations`); err != nil {
						t.Fatal(err)
					}
					want = domain.CodeInternal
				}
				var err error
				if implicit {
					_, err = e.s.ProposeImplicitForSubject(t.Context(), device, subject, "agent", "voice", "不能保存")
				} else {
					_, err = e.s.ProposeForSubject(t.Context(), device, subject, "req", "agent", "voice", "不能保存")
				}
				requestCode(t, err, want)
				row, err := e.s.requests.Get(t.Context(), e.db.DB(), "req")
				if err != nil || row.Text != nil || len(e.n.events) != 0 {
					t.Fatalf("拒绝时不应保存文字或通知：%+v %v", row, err)
				}
			}
		})
	}
}

func TestMCPSubjectRequiresScopeAndImplicitSwitch(t *testing.T) {
	e := newRequestEnv(t)
	_, err := e.s.ProposeForSubject(t.Context(), requestDevice, "", "req", "agent", "voice", "原文")
	requestCode(t, err, domain.CodePermissionDenied)
	_, err = e.s.ProposeImplicitForSubject(t.Context(), requestDevice, "alice", "agent", "voice", "原文")
	requestCode(t, err, domain.CodePermissionDenied)
	e.s.cfg.MCP.AllowImplicitRequestID = true
	_, err = e.s.ProposeImplicitForSubject(t.Context(), requestDevice, "", "agent", "voice", "原文")
	requestCode(t, err, domain.CodePermissionDenied)
}
