package storage

import (
	"database/sql"
	"errors"
	"io/fs"
	"path/filepath"
	"testing"
	"testing/fstest"
)

func TestDeviceRegistrationOwnership(t *testing.T) {
	db := openTestDB(t)
	repo := DeviceRepo{}
	ctx := t.Context()
	if _, err := repo.Get(ctx, db.DB(), "mac"); !errors.Is(err, sql.ErrNoRows) {
		t.Fatal("未知设备应返回无记录", err)
	}
	for _, input := range []struct{ subject, key, wantSubject, wantKey string }{
		{"alice", "first", "alice", "first"},
		{"alice", "rotated", "alice", "rotated"},
		{"bob", "stolen", "alice", "rotated"},
	} {
		row, err := repo.Register(ctx, db.DB(), "mac", input.subject, input.key)
		if err != nil || row.Subject != input.wantSubject || row.CredentialKey != input.wantKey {
			t.Fatal("同账号可更换凭据，其他账号不得覆盖归属", row, err)
		}
	}
	if err := repo.AdoptLegacyCredential(ctx, db.DB(), "mac", "alice", "legacy"); err != nil {
		t.Fatal(err)
	}
	row, err := repo.Get(ctx, db.DB(), "mac")
	if err != nil || row.CredentialKey != "rotated" {
		t.Fatal("兼容迁移不得覆盖已登记凭据", row, err)
	}
	_ = db.Close()
	if _, err := repo.Register(ctx, db.DB(), "new", "alice", "key"); err == nil {
		t.Fatal("数据库关闭后登记必须失败")
	}
	if _, err := repo.Get(ctx, db.DB(), "mac"); err == nil {
		t.Fatal("数据库关闭后读取必须失败")
	}
	if err := repo.AdoptLegacyCredential(ctx, db.DB(), "mac", "alice", "key"); err == nil {
		t.Fatal("数据库关闭后迁移必须失败")
	}
}

func TestLegacyBindingsMigrateToRegistrations(t *testing.T) {
	legacy := fstest.MapFS{}
	for _, name := range []string{"001_initial.sql", "002_requests.sql"} {
		body, err := fs.ReadFile(embeddedMigrations, "migrations/"+name)
		if err != nil {
			t.Fatal(err)
		}
		legacy["migrations/"+name] = &fstest.MapFile{Data: body}
	}
	swapMigrations(t, legacy)
	path := filepath.Join(t.TempDir(), "upgrade.db")
	old, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := old.DB().Exec(`INSERT INTO bindings VALUES('mac','desktop','alice','2026-10-25T00:00:00Z')`); err != nil {
		t.Fatal(err)
	}
	_ = old.Close()
	migrationsFS = embeddedMigrations
	upgraded, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer upgraded.Close()
	repo := DeviceRepo{}
	row, err := repo.Get(t.Context(), upgraded.DB(), "mac")
	if err != nil || row.Subject != "alice" || row.CredentialKey != "" {
		t.Fatal("升级必须保留旧绑定归属", row, err)
	}
	if err := repo.AdoptLegacyCredential(t.Context(), upgraded.DB(), "mac", "bob", "wrong"); err != nil {
		t.Fatal(err)
	}
	row, _ = repo.Get(t.Context(), upgraded.DB(), "mac")
	if row.CredentialKey != "" {
		t.Fatal("旧配置账号变化不得重新认领设备")
	}
	if err := repo.AdoptLegacyCredential(t.Context(), upgraded.DB(), "mac", "alice", "key"); err != nil {
		t.Fatal(err)
	}
	row, err = repo.Get(t.Context(), upgraded.DB(), "mac")
	if err != nil || row.CredentialKey != "key" {
		t.Fatal("升级应补齐原账号凭据关联", row, err)
	}
}
