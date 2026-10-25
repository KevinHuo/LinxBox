package service

import (
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

func TestAccountChangeFiltersCatalogAndMessageNotifications(t *testing.T) {
	e := newServiceEnv(t)
	for _, device := range []string{"AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02"} {
		if _, err := e.binding.Bind(e.ctx, "desktop-1", "alice", device); err != nil {
			t.Fatal(err)
		}
	}
	e.cfg.Credentials.Devices[0].Subject = "bob"
	if _, err := e.sessions.Sync(e.ctx, "desktop-1", []contracts.SessionRecord{record("session")}); err != nil {
		t.Fatal(err)
	}
	if e.notify.updateCount() != 1 || e.notify.updates[0] != "AA:BB:CC:DD:EE:02" {
		t.Fatalf("目录更新只能通知仍归属原账号的设备: %v", e.notify.updates)
	}
	devices, err := e.sessions.DevicesForSession(e.ctx, "desktop-1", "session")
	if err != nil || len(devices) != 1 || devices[0] != "AA:BB:CC:DD:EE:02" {
		t.Fatalf("新消息通知未过滤失效账号绑定: %v: %v", devices, err)
	}
	// 删除 Desktop 凭据后，任何设备都不能继续读取该绑定或接收其通知。
	e.cfg.Credentials.Desktops = nil
	if _, err := e.sessions.Get(e.ctx, "AA:BB:CC:DD:EE:02", "session"); domain.CodeOf(err) != domain.CodePermissionDenied {
		t.Fatalf("已移除的 Desktop 凭据不应授权旧目录: %v", err)
	}
	if devices, err := e.sessions.DevicesForSession(e.ctx, "desktop-1", "session"); err != nil || len(devices) != 0 {
		t.Fatalf("移除凭据后不能再发送通知: %v: %v", devices, err)
	}
}

func TestAccountChangeRevokesRequestsAndExpirationNotices(t *testing.T) {
	for _, change := range []string{"device", "desktop"} {
		t.Run(change, func(t *testing.T) {
			e := newRequestEnv(t)
			e.prepare(t, "private")
			if _, err := e.propose(t, "private", "原账号私有正文"); err != nil {
				t.Fatal(err)
			}
			count := len(e.n.events)
			if change == "device" {
				e.s.cfg.Credentials.Devices[0].Subject = "bob"
			} else {
				e.s.cfg.Credentials.Desktops[0].Subject = "bob"
			}
			_, err := e.s.Prepare(t.Context(), requestDevice, "private", "session")
			requestCode(t, err, domain.CodePermissionDenied)
			_, err = e.s.Get(t.Context(), requestDevice, "private")
			requestCode(t, err, domain.CodePermissionDenied)
			_, err = e.s.Cancel(t.Context(), requestDevice, "private")
			requestCode(t, err, domain.CodePermissionDenied)
			_, err = e.propose(t, "private", "原账号私有正文")
			requestCode(t, err, domain.CodePermissionDenied)
			e.now = e.now.Add(301 * time.Second)
			if err := e.s.Expire(t.Context()); err != nil {
				t.Fatal(err)
			}
			if len(e.n.events) != count {
				t.Fatal("请求过期不能向新账号推送原账号快照")
			}
			row, err := e.s.requests.Get(t.Context(), e.db.DB(), "private")
			if err != nil || row.Status != "failed" || row.Text == nil || *row.Text != "原账号私有正文" {
				t.Fatalf("拒绝新账号访问仍须保留原记录及真实过期状态: %+v: %v", row, err)
			}
		})
	}
}
