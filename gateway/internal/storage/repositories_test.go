package storage

import (
	"database/sql"
	"errors"
	"path/filepath"
	"testing"
	"time"
)

func openTestDB(t *testing.T) *DB {
	t.Helper()
	db, err := Open(filepath.Join(t.TempDir(), "gw.db"))
	if err != nil {
		t.Fatalf("Open 失败: %v", err)
	}
	t.Cleanup(func() { _ = db.Close() })
	return db
}

func TestOpenMigrate(t *testing.T) {
	// 迁移幂等：同一路径重复打开不报错。
	path := filepath.Join(t.TempDir(), "again.db")
	second, err := Open(path)
	if err != nil {
		t.Fatalf("再次迁移失败: %v", err)
	}
	defer second.Close()
	// 迁移记录存在。
	var n int
	if err := second.db.QueryRow(`SELECT COUNT(1) FROM schema_migrations WHERE version = '001_initial'`).Scan(&n); err != nil {
		t.Fatal(err)
	}
	if n != 1 {
		t.Fatalf("迁移记录数错误: %d", n)
	}
	// 核心表存在。
	for _, table := range []string{"bindings", "session_catalog", "catalog_meta"} {
		if err := second.db.QueryRow(`SELECT COUNT(1) FROM sqlite_master WHERE type='table' AND name=?`, table).Scan(&n); err != nil {
			t.Fatal(err)
		}
		if n != 1 {
			t.Fatalf("缺少表 %s", table)
		}
	}
}

func TestOpenBadPath(t *testing.T) {
	if _, err := Open(filepath.Join(t.TempDir(), "no-such-dir", "gw.db")); err == nil {
		t.Fatal("不可写路径应报错")
	}
}

func TestWithTxRollback(t *testing.T) {
	db := openTestDB(t)
	err := db.WithTx(t.Context(), func(tx *sql.Tx) error {
		if _, err := tx.Exec(`INSERT INTO bindings(device_id, desktop_client_id, subject, created_at) VALUES('AA:BB:CC:DD:EE:01','d1','alice','2026-10-25T00:00:00Z')`); err != nil {
			return err
		}
		return errors.New("触发回滚")
	})
	if err == nil {
		t.Fatal("应返回错误")
	}
	var n int
	if err := db.db.QueryRow(`SELECT COUNT(1) FROM bindings`).Scan(&n); err != nil {
		t.Fatal(err)
	}
	if n != 0 {
		t.Fatal("回滚后不应有数据")
	}
}

func TestBindingRepo(t *testing.T) {
	db := openTestDB(t)
	repos := NewRepositories(db.DB())
	ctx := t.Context()
	now := time.Now().UTC()

	if err := repos.WithTx(ctx, func(tx *sql.Tx) error {
		return repos.Bindings.Insert(ctx, tx, BindingRow{
			DeviceID: "AA:BB:CC:DD:EE:01", DesktopClientID: "d1", Subject: "alice", CreatedAt: now,
		})
	}); err != nil {
		t.Fatal(err)
	}
	row, err := repos.Bindings.GetByDevice(ctx, repos.DB(), "AA:BB:CC:DD:EE:01")
	if err != nil {
		t.Fatal(err)
	}
	if row.DesktopClientID != "d1" || row.Subject != "alice" {
		t.Fatalf("读取结果错误: %+v", row)
	}
	if !row.CreatedAt.Equal(now.Truncate(time.Second)) && row.CreatedAt.UTC().Format(time.RFC3339) != now.UTC().Format(time.RFC3339) {
		t.Fatalf("时间读取错误: %v", row.CreatedAt)
	}
	if _, err := repos.Bindings.GetByDevice(ctx, repos.DB(), "AA:BB:CC:DD:EE:02"); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("无记录应返回 ErrNoRows，得到 %v", err)
	}
	// 唯一约束：同设备绑定另一 Desktop 应失败。
	err = repos.WithTx(ctx, func(tx *sql.Tx) error {
		return repos.Bindings.Insert(ctx, tx, BindingRow{
			DeviceID: "AA:BB:CC:DD:EE:01", DesktopClientID: "d2", Subject: "bob", CreatedAt: now,
		})
	})
	if err == nil {
		t.Fatal("唯一约束应生效")
	}
	// 列表设备。
	if err := repos.WithTx(ctx, func(tx *sql.Tx) error {
		return repos.Bindings.Insert(ctx, tx, BindingRow{
			DeviceID: "AA:BB:CC:DD:EE:02", DesktopClientID: "d1", Subject: "alice", CreatedAt: now,
		})
	}); err != nil {
		t.Fatal(err)
	}
	devices, err := repos.Bindings.ListDevicesByDesktop(ctx, repos.DB(), "d1")
	if err != nil {
		t.Fatal(err)
	}
	if len(devices) != 2 {
		t.Fatalf("设备列表长度错误: %d", len(devices))
	}
}

func TestSessionRepo(t *testing.T) {
	db := openTestDB(t)
	repos := NewRepositories(db.DB())
	ctx := t.Context()
	now := time.Now().UTC()

	// 从未同步时版本为 0。
	v, err := repos.Sessions.GetVersion(ctx, repos.DB(), "d1")
	if err != nil {
		t.Fatal(err)
	}
	if v != 0 {
		t.Fatalf("初始版本应为 0: %d", v)
	}

	rows := []SessionRow{
		{DesktopClientID: "d1", AgentSessionID: "s1", ThreadID: "t1", ProjectID: "p1", ProjectName: "proj", Agent: "codex", Title: "会话1", Status: "idle", Summary: ""},
		{DesktopClientID: "d1", AgentSessionID: "s2", ThreadID: "t2", ProjectID: "p1", ProjectName: "proj", Agent: "codex", Title: "会话2", Status: "running", Summary: "进行中"},
	}
	if err := repos.WithTx(ctx, func(tx *sql.Tx) error {
		return repos.Sessions.ReplaceAll(ctx, tx, "d1", rows, 1, now)
	}); err != nil {
		t.Fatal(err)
	}
	v, err = repos.Sessions.GetVersion(ctx, repos.DB(), "d1")
	if err != nil {
		t.Fatal(err)
	}
	if v != 1 {
		t.Fatalf("版本应为 1: %d", v)
	}
	// 替换后旧记录消失。
	replaced := []SessionRow{{DesktopClientID: "d1", AgentSessionID: "s3", ThreadID: "t3", ProjectID: "p1", ProjectName: "proj", Agent: "codex", Title: "会话3", Status: "completed", Summary: "完成"}}
	if err := repos.WithTx(ctx, func(tx *sql.Tx) error {
		return repos.Sessions.ReplaceAll(ctx, tx, "d1", replaced, 2, now)
	}); err != nil {
		t.Fatal(err)
	}
	list, err := repos.Sessions.List(ctx, repos.DB(), "d1", 0, 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(list) != 1 || list[0].AgentSessionID != "s3" {
		t.Fatalf("替换后列表错误: %+v", list)
	}
	// 分页。
	for i := 0; i < 7; i++ {
		replaced = append(replaced, SessionRow{
			DesktopClientID: "d1", AgentSessionID: "s" + string(rune('a'+i)), ThreadID: "t", ProjectID: "p",
			ProjectName: "proj", Agent: "codex", Title: "t", Status: "idle", Summary: "",
		})
	}
	if err := repos.WithTx(ctx, func(tx *sql.Tx) error {
		return repos.Sessions.ReplaceAll(ctx, tx, "d1", replaced, 3, now)
	}); err != nil {
		t.Fatal(err)
	}
	page1, err := repos.Sessions.List(ctx, repos.DB(), "d1", 0, 5)
	if err != nil {
		t.Fatal(err)
	}
	if len(page1) != 5 {
		t.Fatalf("第一页应为 5 项: %d", len(page1))
	}
	page2, err := repos.Sessions.List(ctx, repos.DB(), "d1", 5, 5)
	if err != nil {
		t.Fatal(err)
	}
	if len(page2) != 3 {
		t.Fatalf("第二页应为 3 项: %d", len(page2))
	}
	// Get 单条与缺失。
	got, err := repos.Sessions.Get(ctx, repos.DB(), "d1", "s3")
	if err != nil {
		t.Fatal(err)
	}
	if got.ProjectID != "p1" {
		t.Fatalf("Get 结果错误: %+v", got)
	}
	if _, err := repos.Sessions.Get(ctx, repos.DB(), "d1", "missing"); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("缺失记录应返回 ErrNoRows: %v", err)
	}
	// 不同 Desktop 隔离。
	other, err := repos.Sessions.List(ctx, repos.DB(), "d2", 0, 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(other) != 0 {
		t.Fatal("不同 Desktop 不应看到目录")
	}
}

func TestEmptyAndEdgePaths(t *testing.T) {
	db := openTestDB(t)
	repos := NewRepositories(db.DB())
	ctx := t.Context()
	now := time.Now().UTC()

	// 未绑定 Desktop 的设备列表为空。
	devices, err := repos.Bindings.ListDevicesByDesktop(ctx, repos.DB(), "nobody")
	if err != nil {
		t.Fatal(err)
	}
	if len(devices) != 0 {
		t.Fatalf("空列表错误: %v", devices)
	}
	// ReplaceAll 空目录：清空且版本推进。
	if err := repos.WithTx(ctx, func(tx *sql.Tx) error {
		return repos.Sessions.ReplaceAll(ctx, tx, "d1", []SessionRow{{DesktopClientID: "d1", AgentSessionID: "a", ThreadID: "t", ProjectID: "p", ProjectName: "p", Agent: "codex", Title: "t", Status: "idle", Summary: ""}}, 1, now)
	}); err != nil {
		t.Fatal(err)
	}
	if err := repos.WithTx(ctx, func(tx *sql.Tx) error {
		return repos.Sessions.ReplaceAll(ctx, tx, "d1", []SessionRow{}, 2, now)
	}); err != nil {
		t.Fatal(err)
	}
	list, err := repos.Sessions.List(ctx, repos.DB(), "d1", 0, 5)
	if err != nil {
		t.Fatal(err)
	}
	if len(list) != 0 {
		t.Fatalf("清空后应为空: %d", len(list))
	}
	// 越界 offset 返回空而非报错。
	list, err = repos.Sessions.List(ctx, repos.DB(), "d1", 100, 5)
	if err != nil {
		t.Fatal(err)
	}
	if len(list) != 0 {
		t.Fatalf("越界 offset 应为空: %d", len(list))
	}
	// GetVersion 缺失 Desktop 返回 0。
	v, err := repos.Sessions.GetVersion(ctx, repos.DB(), "nobody")
	if err != nil {
		t.Fatal(err)
	}
	if v != 0 {
		t.Fatalf("缺失 Desktop 版本应为 0: %d", v)
	}
	// 零行 ReplaceAll 对未存在 Desktop 也推进版本。
	if err := repos.WithTx(ctx, func(tx *sql.Tx) error {
		return repos.Sessions.ReplaceAll(ctx, tx, "nobody", []SessionRow{}, 1, now)
	}); err != nil {
		t.Fatal(err)
	}
}
