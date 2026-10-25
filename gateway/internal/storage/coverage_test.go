package storage

import (
	"context"
	"database/sql"
	"errors"
	"io/fs"
	"path/filepath"
	"testing"
	"testing/fstest"
	"time"
)

// swapMigrations 替换迁移源并恢复。
func swapMigrations(t *testing.T, f fs.FS) {
	t.Helper()
	old := migrationsFS
	migrationsFS = f
	t.Cleanup(func() { migrationsFS = old })
}

func TestOpenMigrateReadFileError(t *testing.T) {
	m := fstest.MapFS{"migrations/001_initial.sql": &fstest.MapFile{Data: []byte("SELECT 1;")}}
	swapMigrations(t, readFailFS{inner: m})
	if _, err := Open(filepath.Join(t.TempDir(), "gw.db")); err == nil {
		t.Fatal("迁移文件读取失败应报错")
	}
}

func TestOpenMigrateExecError(t *testing.T) {
	m := fstest.MapFS{"migrations/bad.sql": &fstest.MapFile{Data: []byte("NOT VALID SQL;")}}
	swapMigrations(t, m)
	if _, err := Open(filepath.Join(t.TempDir(), "gw.db")); err == nil {
		t.Fatal("非法迁移 SQL 应报错")
	}
}

func TestWithTxBeginError(t *testing.T) {
	db := openTestDB(t)
	// 占满连接池，BeginTx 将因上下文超时失败。
	var txs []*sql.Tx
	for i := 0; i < 4; i++ {
		tx, err := db.db.Begin()
		if err != nil {
			t.Fatal(err)
		}
		txs = append(txs, tx)
	}
	defer func() {
		for _, tx := range txs {
			_ = tx.Rollback()
		}
	}()
	ctx, cancel := context.WithTimeout(t.Context(), 100*time.Millisecond)
	defer cancel()
	if err := db.WithTx(ctx, func(tx *sql.Tx) error { return nil }); err == nil {
		t.Fatal("连接耗尽时 BeginTx 应报错")
	}
	// Repositories.WithTx 同样失败。
	repos := NewRepositories(db.DB())
	if err := repos.WithTx(ctx, func(tx *sql.Tx) error { return nil }); err == nil {
		t.Fatal("Repositories.WithTx 连接耗尽应报错")
	}
}

func TestWithTxSuccess(t *testing.T) {
	db := openTestDB(t)
	// 提交路径。
	if err := db.WithTx(t.Context(), func(tx *sql.Tx) error {
		_, err := tx.Exec(`INSERT INTO bindings(device_id, desktop_client_id, subject, created_at) VALUES('d','k','s','2026-10-25T00:00:00Z')`)
		return err
	}); err != nil {
		t.Fatal(err)
	}
}

// errQueryer 让仓储查询立即失败。
type errQueryer struct{}

func (errQueryer) ExecContext(ctx context.Context, query string, args ...any) (sql.Result, error) {
	return nil, errors.New("执行失败")
}
func (errQueryer) QueryRowContext(ctx context.Context, query string, args ...any) *sql.Row {
	return nil
}
func (errQueryer) QueryContext(ctx context.Context, query string, args ...any) (*sql.Rows, error) {
	return nil, errors.New("查询失败")
}

func TestQueryErrors(t *testing.T) {
	var q errQueryer
	if _, err := (BindingRepo{}).ListDevicesByDesktop(t.Context(), q, "d"); err == nil {
		t.Fatal("查询失败应报错")
	}
	if _, err := (SessionRepo{}).List(t.Context(), q, "d", 0, 5); err == nil {
		t.Fatal("查询失败应报错")
	}
}

func TestScanBindingBadDate(t *testing.T) {
	db := openTestDB(t)
	repos := NewRepositories(db.DB())
	if _, err := repos.DB().Exec(`INSERT INTO bindings(device_id, desktop_client_id, subject, created_at) VALUES('d','k','s','not-a-time')`); err != nil {
		t.Fatal(err)
	}
	if _, err := repos.Bindings.GetByDevice(t.Context(), repos.DB(), "d"); err == nil {
		t.Fatal("非法时间应解析失败")
	}
}

func TestListBadDate(t *testing.T) {
	db := openTestDB(t)
	repos := NewRepositories(db.DB())
	if _, err := repos.DB().Exec(`INSERT INTO session_catalog(
		desktop_client_id, agent_session_id, thread_id, project_id, project_name, agent, title, status, summary, updated_at
	) VALUES('d','a','t','p','p','codex','t','idle','','not-a-time')`); err != nil {
		t.Fatal(err)
	}
	if _, err := repos.Sessions.List(t.Context(), repos.DB(), "d", 0, 5); err == nil {
		t.Fatal("非法时间应解析失败")
	}
}

// 用触发器让 ReplaceAll 的 DELETE / INSERT 失败。
func TestReplaceAllTriggerErrors(t *testing.T) {
	db := openTestDB(t)
	repos := NewRepositories(db.DB())
	// 先写入一行，DELETE 才会触发行级触发器。
	if _, err := repos.DB().Exec(`INSERT INTO session_catalog(
		desktop_client_id, agent_session_id, thread_id, project_id, project_name, agent, title, status, summary, updated_at
	) VALUES('d','a','t','p','p','codex','t','idle','','2026-10-25T00:00:00Z')`); err != nil {
		t.Fatal(err)
	}
	if _, err := repos.DB().Exec(`CREATE TRIGGER deny_delete BEFORE DELETE ON session_catalog BEGIN SELECT RAISE(ABORT, 'no delete'); END`); err != nil {
		t.Fatal(err)
	}
	now := time.Now().UTC()
	if err := repos.WithTx(t.Context(), func(tx *sql.Tx) error {
		return repos.Sessions.ReplaceAll(t.Context(), tx, "d", nil, 1, now)
	}); err == nil {
		t.Fatal("DELETE 被拒绝时应报错")
	}
	// 重新打开独立连接：删除触发器后建 INSERT 触发器。
	db2 := openTestDB(t)
	repos2 := NewRepositories(db2.DB())
	if _, err := repos2.DB().Exec(`CREATE TRIGGER deny_insert BEFORE INSERT ON session_catalog BEGIN SELECT RAISE(ABORT, 'no insert'); END`); err != nil {
		t.Fatal(err)
	}
	row := SessionRow{DesktopClientID: "d", AgentSessionID: "a", ThreadID: "t", ProjectID: "p", ProjectName: "p", Agent: "codex", Title: "t", Status: "idle", Summary: "", UpdatedAt: now}
	if err := repos2.WithTx(t.Context(), func(tx *sql.Tx) error {
		return repos2.Sessions.ReplaceAll(t.Context(), tx, "d", []SessionRow{row}, 1, now)
	}); err == nil {
		t.Fatal("INSERT 被拒绝时应报错")
	}
}

// dirFailFS 目录读取失败。
type dirFailFS struct{}

func (dirFailFS) Open(name string) (fs.File, error) {
	return nil, errors.New("打开失败")
}
func (dirFailFS) ReadDir(name string) ([]fs.DirEntry, error) {
	return nil, errors.New("目录失败")
}

// readFailFS 目录可读但文件读取失败。
type readFailFS struct{ inner fstest.MapFS }

func (readFailFS) Open(name string) (fs.File, error) {
	return nil, errors.New("读取失败")
}
func (f readFailFS) ReadDir(name string) ([]fs.DirEntry, error) {
	return f.inner.ReadDir(name)
}

func TestOpenMigrateSkipApplied(t *testing.T) {
	// 再次打开同一数据库：迁移已应用，exists>0 分支走 continue。
	path := filepath.Join(t.TempDir(), "gw.db")
	if _, err := Open(path); err != nil {
		t.Fatal(err)
	}
	db2, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	db2.Close()
}
