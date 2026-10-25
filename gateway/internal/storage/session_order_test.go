package storage

import (
	"database/sql"
	"path/filepath"
	"slices"
	"testing"
	"time"
)

func orderedSessionRows(desktop string, ids ...string) []SessionRow {
	rows := make([]SessionRow, 0, len(ids))
	for _, id := range ids {
		rows = append(rows, SessionRow{DesktopClientID: desktop, AgentSessionID: id,
			ThreadID: "thread-" + id, ProjectID: "p", ProjectName: "项目", Agent: "codex", Title: id, Status: "idle"})
	}
	return rows
}

func assertSessionOrder(t *testing.T, repos *Repositories, desktop string, offset, limit int, want []string) {
	t.Helper()
	rows, err := repos.Sessions.List(t.Context(), repos.DB(), desktop, offset, limit)
	if err != nil {
		t.Fatal(err)
	}
	got := make([]string, 0, len(rows))
	for _, row := range rows {
		got = append(got, row.AgentSessionID)
	}
	if !slices.Equal(got, want) {
		t.Fatalf("目录顺序错误：got=%v want=%v", got, want)
	}
}

func TestSessionOrderPersistsAndRollsBack(t *testing.T) {
	path := filepath.Join(t.TempDir(), "order.db")
	db, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	repos := NewRepositories(db.DB())
	replace := func(desktop string, ids []string, version int64) error {
		return repos.WithTx(t.Context(), func(tx *sql.Tx) error {
			return repos.Sessions.ReplaceAll(t.Context(), tx, desktop, orderedSessionRows(desktop, ids...), version, time.Now())
		})
	}
	want := []string{"z", "a", "m", "b", "y", "c"}
	if err := replace("d1", want, 1); err != nil {
		t.Fatal(err)
	}
	if err := replace("d2", []string{"y", "a"}, 1); err != nil {
		t.Fatal(err)
	}
	assertSessionOrder(t, repos, "d1", 0, 5, want[:5])
	assertSessionOrder(t, repos, "d1", 5, 5, want[5:])
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}
	db, err = Open(path)
	if err != nil {
		t.Fatal(err)
	}
	repos = NewRepositories(db.DB())
	assertSessionOrder(t, repos, "d1", 0, 10, want)
	slices.Reverse(want)
	if err := replace("d1", want, 2); err != nil {
		t.Fatal(err)
	}
	assertSessionOrder(t, repos, "d1", 0, 10, want)
	// 第二条重复 ID 写入失败，删除、首条插入及版本变化必须整体回滚。
	if err := replace("d1", []string{"new", "new"}, 3); err == nil {
		t.Fatal("重复 ID 应导致事务失败")
	}
	assertSessionOrder(t, repos, "d1", 0, 10, want)
	version, err := repos.Sessions.GetVersion(t.Context(), repos.DB(), "d1")
	if err != nil || version != 2 {
		t.Fatalf("失败不应推进版本：%d %v", version, err)
	}
	if err := replace("d1", nil, 3); err != nil {
		t.Fatal(err)
	}
	assertSessionOrder(t, repos, "d1", 0, 5, nil)
	assertSessionOrder(t, repos, "d2", 0, 5, []string{"y", "a"})
}

func TestSessionOrderMigratesLegacyCatalog(t *testing.T) {
	path := filepath.Join(t.TempDir(), "legacy.db")
	legacy, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	defer legacy.Close()
	if _, err := legacy.Exec(`CREATE TABLE schema_migrations(version TEXT PRIMARY KEY, applied_at TEXT NOT NULL)`); err != nil {
		t.Fatal(err)
	}
	// 创建真实 001～004 数据库，不通过新仓储写入，验证已有部署升级。
	for _, name := range []string{"001_initial", "002_requests", "003_device_registrations", "004_request_executions"} {
		body, err := embeddedMigrations.ReadFile("migrations/" + name + ".sql")
		if err != nil {
			t.Fatal(err)
		}
		if _, err := legacy.Exec(string(body)); err != nil {
			t.Fatal(err)
		}
		if _, err := legacy.Exec(`INSERT INTO schema_migrations VALUES(?, '2026-10-26T00:00:00Z')`, name); err != nil {
			t.Fatal(err)
		}
	}
	for _, statement := range []string{
		`INSERT INTO bindings VALUES('AA:BB:CC:DD:EE:01', 'd1', 'alice', '2026-10-26T00:00:00Z')`,
		`INSERT INTO catalog_meta VALUES('d1', 7, '2026-10-26T00:00:00Z')`,
		`INSERT INTO requests(request_id, device_id, desktop_client_id, agent_session_id, thread_id, project_id, target_title, status, expires_at) VALUES('r1', 'AA:BB:CC:DD:EE:01', 'd1', 'z', 't', 'p', '标题', 'completed', 0)`,
	} {
		if _, err := legacy.Exec(statement); err != nil {
			t.Fatal(err)
		}
	}
	for _, id := range []string{"z", "a", "m"} {
		if _, err := legacy.Exec(`INSERT INTO session_catalog VALUES('d1', ?, 't', 'p', '项目', 'codex', '标题', 'idle', '', '2026-10-26T00:00:00Z')`, id); err != nil {
			t.Fatal(err)
		}
	}
	if err := legacy.Close(); err != nil {
		t.Fatal(err)
	}
	// 再次打开也必须幂等；首次新同步后顺序及目录版本保持。
	for attempt := 0; attempt < 2; attempt++ {
		db, err := Open(path)
		if err != nil {
			t.Fatal(err)
		}
		defer db.Close()
		repos := NewRepositories(db.DB())
		want, wantVersion := []string{"a", "m", "z"}, int64(7)
		if attempt == 1 {
			want, wantVersion = []string{"z", "a", "m"}, 8
		}
		assertSessionOrder(t, repos, "d1", 0, 5, want)
		version, err := repos.Sessions.GetVersion(t.Context(), repos.DB(), "d1")
		if err != nil || version != wantVersion {
			t.Fatalf("目录版本错误：%d %v", version, err)
		}
		for table, condition := range map[string]string{
			"bindings":          "desktop_client_id='d1' AND subject='alice'",
			"requests":          "request_id='r1' AND status='completed'",
			"schema_migrations": "version='005_session_order'",
		} {
			var count int
			if err := db.DB().QueryRow("SELECT COUNT(*) FROM " + table + " WHERE " + condition).Scan(&count); err != nil || count != 1 {
				t.Fatalf("迁移未保留 %s：%d %v", table, count, err)
			}
		}
		if attempt == 0 {
			if err := repos.WithTx(t.Context(), func(tx *sql.Tx) error {
				return repos.Sessions.ReplaceAll(t.Context(), tx, "d1", orderedSessionRows("d1", "z", "a", "m"), 8, time.Now())
			}); err != nil {
				t.Fatal(err)
			}
		}
		if err := db.Close(); err != nil {
			t.Fatal(err)
		}
	}
}
