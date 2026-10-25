package storage

import (
	"context"
	"database/sql"
	"errors"
	"time"
)

// Repositories 集中管理业务数据读写；初期集中，按实际体量拆分。
type Repositories struct {
	db       *sql.DB
	Bindings BindingRepo
	Sessions SessionRepo
	Devices  DeviceRepo
}

// NewRepositories 基于现有连接构造仓储。
func NewRepositories(db *sql.DB) *Repositories {
	return &Repositories{
		db:       db,
		Bindings: BindingRepo{},
		Sessions: SessionRepo{},
	}
}

// WithTx 在事务中执行 fn，失败回滚。
func (r *Repositories) WithTx(ctx context.Context, fn func(tx *sql.Tx) error) error {
	tx, err := r.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	if err := fn(tx); err != nil {
		tx.Rollback()
		return err
	}
	return tx.Commit()
}

// DB 返回底层连接，供非事务读取使用。
func (r *Repositories) DB() *sql.DB { return r.db }

// Queryer 使仓储方法同时兼容 *sql.DB 与 *sql.Tx。
type Queryer interface {
	ExecContext(ctx context.Context, query string, args ...any) (sql.Result, error)
	QueryRowContext(ctx context.Context, query string, args ...any) *sql.Row
	QueryContext(ctx context.Context, query string, args ...any) (*sql.Rows, error)
}

// BindingRow 是一条绑定记录。
type BindingRow struct {
	DeviceID        string
	DesktopClientID string
	Subject         string
	CreatedAt       time.Time
}

// BindingRepo 读写 bindings 表。
type BindingRepo struct{}

// Insert 写入绑定记录；主键冲突由调用方按业务处理。
func (BindingRepo) Insert(ctx context.Context, q Queryer, row BindingRow) error {
	_, err := q.ExecContext(ctx, `INSERT INTO bindings(device_id, desktop_client_id, subject, created_at) VALUES(?, ?, ?, ?)`,
		row.DeviceID, row.DesktopClientID, row.Subject, row.CreatedAt.UTC().Format(time.RFC3339))
	return err
}

// GetByDevice 按设备读取绑定；无记录返回 sql.ErrNoRows。
func (BindingRepo) GetByDevice(ctx context.Context, q Queryer, deviceID string) (*BindingRow, error) {
	row := q.QueryRowContext(ctx, `SELECT device_id, desktop_client_id, subject, created_at FROM bindings WHERE device_id = ?`, deviceID)
	return scanBinding(row)
}

// ListDevicesByDesktop 返回绑定到指定 Desktop 的全部设备标识。
func (BindingRepo) ListDevicesByDesktop(ctx context.Context, q Queryer, desktopClientID string) ([]string, error) {
	rows, err := q.QueryContext(ctx, `SELECT device_id FROM bindings WHERE desktop_client_id = ?`, desktopClientID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		out = append(out, id)
	}
	return out, rows.Err()
}

func scanBinding(row *sql.Row) (*BindingRow, error) {
	var b BindingRow
	var created string
	if err := row.Scan(&b.DeviceID, &b.DesktopClientID, &b.Subject, &created); err != nil {
		return nil, err
	}
	createdAt, err := time.Parse(time.RFC3339, created)
	if err != nil {
		return nil, err
	}
	b.CreatedAt = createdAt
	return &b, nil
}

// SessionRow 是一条会话目录记录。
type SessionRow struct {
	DesktopClientID string
	AgentSessionID  string
	ThreadID        string
	ProjectID       string
	ProjectName     string
	Agent           string
	Title           string
	Status          string
	Summary         string
	UpdatedAt       time.Time
}

// SessionRepo 读写 session_catalog 与 catalog_meta。
type SessionRepo struct{}

// ReplaceAll 在同一事务内按上传顺序重写 Desktop 目录，并更新目录版本。
func (SessionRepo) ReplaceAll(ctx context.Context, tx *sql.Tx, desktopClientID string, rows []SessionRow, version int64, now time.Time) error {
	if _, err := tx.ExecContext(ctx, `DELETE FROM session_catalog WHERE desktop_client_id = ?`, desktopClientID); err != nil {
		return err
	}
	for position, r := range rows {
		if _, err := tx.ExecContext(ctx, `INSERT INTO session_catalog(
			desktop_client_id, agent_session_id, thread_id, project_id, project_name, agent, title, status, summary, updated_at, sort_order
		) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.DesktopClientID, r.AgentSessionID, r.ThreadID, r.ProjectID, r.ProjectName, r.Agent, r.Title, r.Status, r.Summary,
			now.UTC().Format(time.RFC3339), position); err != nil {
			return err
		}
	}
	_, err := tx.ExecContext(ctx, `INSERT INTO catalog_meta(desktop_client_id, version, updated_at) VALUES(?, ?, ?)
		ON CONFLICT(desktop_client_id) DO UPDATE SET version = excluded.version, updated_at = excluded.updated_at`,
		desktopClientID, version, now.UTC().Format(time.RFC3339))
	return err
}

// GetVersion 读取目录版本；从未同步时返回 0。
func (SessionRepo) GetVersion(ctx context.Context, q Queryer, desktopClientID string) (int64, error) {
	var v int64
	err := q.QueryRowContext(ctx, `SELECT version FROM catalog_meta WHERE desktop_client_id = ?`, desktopClientID).Scan(&v)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, nil
	}
	return v, err
}

// List 按上传顺序分页；旧目录尚未重新同步时以 ID 稳定排序。
// limit+1 用于判断是否还有下一页。
func (SessionRepo) List(ctx context.Context, q Queryer, desktopClientID string, offset, limit int) ([]SessionRow, error) {
	rows, err := q.QueryContext(ctx, `SELECT desktop_client_id, agent_session_id, thread_id, project_id, project_name, agent, title, status, summary, updated_at
		FROM session_catalog WHERE desktop_client_id = ? ORDER BY sort_order, agent_session_id LIMIT ? OFFSET ?`,
		desktopClientID, limit, offset)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []SessionRow
	for rows.Next() {
		var r SessionRow
		var updated string
		if err := rows.Scan(&r.DesktopClientID, &r.AgentSessionID, &r.ThreadID, &r.ProjectID,
			&r.ProjectName, &r.Agent, &r.Title, &r.Status, &r.Summary, &updated); err != nil {
			return nil, err
		}
		ts, err := time.Parse(time.RFC3339, updated)
		if err != nil {
			return nil, err
		}
		r.UpdatedAt = ts
		out = append(out, r)
	}
	return out, rows.Err()
}

// Get 读取单条会话记录；无记录返回 sql.ErrNoRows。
func (SessionRepo) Get(ctx context.Context, q Queryer, desktopClientID, agentSessionID string) (*SessionRow, error) {
	row := q.QueryRowContext(ctx, `SELECT desktop_client_id, agent_session_id, thread_id, project_id, project_name, agent, title, status, summary, updated_at
		FROM session_catalog WHERE desktop_client_id = ? AND agent_session_id = ?`, desktopClientID, agentSessionID)
	var r SessionRow
	var updated string
	if err := row.Scan(&r.DesktopClientID, &r.AgentSessionID, &r.ThreadID, &r.ProjectID,
		&r.ProjectName, &r.Agent, &r.Title, &r.Status, &r.Summary, &updated); err != nil {
		return nil, err
	}
	return &r, nil
}
