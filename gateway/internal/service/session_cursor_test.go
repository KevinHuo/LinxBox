package service

import (
	"fmt"
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

func TestCursorBoundToDeviceAndSnapshot(t *testing.T) {
	e := newServiceEnv(t)
	for _, id := range []string{"AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02"} {
		if _, err := e.binding.Bind(e.ctx, "desktop-1", "alice", id); err != nil {
			t.Fatal(err)
		}
	}
	items := []contracts.SessionRecord{record("1"), record("2"), record("3"), record("4"), record("5"), record("6")}
	if _, err := e.sessions.Sync(e.ctx, "desktop-1", items); err != nil {
		t.Fatal(err)
	}
	page, err := e.sessions.List(e.ctx, "AA:BB:CC:DD:EE:01", "")
	if err != nil {
		t.Fatal(err)
	}
	if page.NextCursor == nil {
		t.Fatal("缺少下一页")
	}
	if _, err := e.sessions.List(e.ctx, "AA:BB:CC:DD:EE:02", *page.NextCursor); domain.CodeOf(err) != domain.CodeInvalidArgument {
		t.Fatal("跨设备 cursor 应拒绝")
	}
	page2, err := e.sessions.List(e.ctx, "AA:BB:CC:DD:EE:01", *page.NextCursor)
	if err != nil || len(page2.Items) != 1 {
		t.Fatal("有效分页失败")
	}
	c := e.sessions.cursors[*page.NextCursor]
	c.Expires = time.Now().Add(-time.Second)
	e.sessions.cursors[*page.NextCursor] = c
	if _, err := e.sessions.List(e.ctx, "AA:BB:CC:DD:EE:01", *page.NextCursor); domain.CodeOf(err) != domain.CodeConflict {
		t.Fatal("过期 cursor 应拒绝")
	}
	if _, err := e.sessions.Sync(e.ctx, "desktop-1", items); err != nil {
		t.Fatal(err)
	}
	c.Expires = time.Now().Add(time.Minute)
	e.sessions.cursors[*page.NextCursor] = c
	if _, err := e.sessions.List(e.ctx, "AA:BB:CC:DD:EE:01", *page.NextCursor); domain.CodeOf(err) != domain.CodeConflict {
		t.Fatal("目录版本变化应返回 CONFLICT")
	}
	if _, err := e.sessions.List(e.ctx, "AA:BB:CC:DD:EE:01", "expired-or-restarted"); domain.CodeOf(err) != domain.CodeConflict {
		t.Fatal("重启后无法识别的 cursor 应返回 CONFLICT")
	}
	if _, err := e.sessions.DesktopForSession(e.ctx, "AA:BB:CC:DD:EE:01", "missing"); err == nil {
		t.Fatal("缺失会话不可路由")
	}
	if _, err := e.sessions.DesktopForSession(e.ctx, "AA:BB:CC:DD:EE:FF", "1"); err == nil {
		t.Fatal("未绑定设备不可路由")
	}
	if id, err := e.sessions.DesktopForSession(e.ctx, "AA:BB:CC:DD:EE:01", "1"); err != nil || id != "desktop-1" {
		t.Fatal("查询路由错误")
	}
	if ids, err := e.sessions.DevicesForSession(e.ctx, "desktop-1", "1"); err != nil || len(ids) != 2 {
		t.Fatal("通知路由错误")
	}
	if _, err := e.sessions.DevicesForSession(e.ctx, "desktop-2", "1"); err == nil {
		t.Fatal("不能通知其他 Desktop 会话")
	}
}

func TestCursorCapacity(t *testing.T) {
	e := newServiceEnv(t)
	for i := 0; i < 1024; i++ {
		e.sessions.cursors[fmt.Sprint(i)] = catalogCursor{Expires: time.Now().Add(time.Minute)}
	}
	if _, err := e.sessions.newCursor(catalogCursor{}); err == nil {
		t.Fatal("分页缓存必须有界")
	}
	e.sessions.cursors["0"] = catalogCursor{Expires: time.Now().Add(-time.Second)}
	if _, err := e.sessions.newCursor(catalogCursor{}); err != nil {
		t.Fatal(err)
	}
}
