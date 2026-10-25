package storage

import (
	"context"
	"database/sql"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
)

// RequestRow 保存设备不可见的固定目标及灵矽来源，目录替换不删除请求。
type RequestRow struct {
	contracts.Request
	DeviceID, DesktopID, ThreadID, ProjectID string
	LinxAgentID, LinxSessionID               string
	ExpiresAt                                int64
	Expired                                  bool
}

// RequestRepo 读写请求；状态变化由服务层在事务内校验。
type RequestRepo struct{}

const requestColumns = `request_id, device_id, desktop_client_id, agent_session_id,
thread_id, project_id, target_title, text, status, error_code, error_message,
expires_at, expired, linx_agent_id, linx_session_id`

// Get 读取不可复用的 request_id。
func (RequestRepo) Get(ctx context.Context, q Queryer, id string) (*RequestRow, error) {
	var r RequestRow
	var code, message sql.NullString
	err := q.QueryRowContext(ctx, `SELECT `+requestColumns+` FROM requests WHERE request_id=?`, id).Scan(
		&r.RequestID, &r.DeviceID, &r.DesktopID, &r.AgentSessionID, &r.ThreadID, &r.ProjectID,
		&r.TargetTitle, &r.Text, &r.Status, &code, &message, &r.ExpiresAt, &r.Expired, &r.LinxAgentID, &r.LinxSessionID)
	if err != nil {
		return nil, err
	}
	if code.Valid {
		r.Error = &contracts.Error{Code: code.String, Message: message.String}
	}
	return &r, nil
}

// Insert 固定录音目标；唯一索引同时约束设备和会话的未结束请求。
func (RequestRepo) Insert(ctx context.Context, q Queryer, r *RequestRow) error {
	_, err := q.ExecContext(ctx, `INSERT INTO requests (`+requestColumns+`) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		r.RequestID, r.DeviceID, r.DesktopID, r.AgentSessionID, r.ThreadID, r.ProjectID, r.TargetTitle,
		r.Text, r.Status, nil, nil, r.ExpiresAt, r.Expired, r.LinxAgentID, r.LinxSessionID)
	return err
}

// Save 只写可变状态，不允许更换固定目标。
func (RequestRepo) Save(ctx context.Context, q Queryer, r *RequestRow) error {
	var code, message any
	if r.Error != nil {
		code, message = r.Error.Code, r.Error.Message
	}
	_, err := q.ExecContext(ctx, `UPDATE requests SET text=?,status=?,error_code=?,error_message=?,
expires_at=?,expired=?,linx_agent_id=?,linx_session_id=? WHERE request_id=?`,
		r.Text, r.Status, code, message, r.ExpiresAt, r.Expired, r.LinxAgentID, r.LinxSessionID, r.RequestID)
	return err
}

// RequestBlocker 只含内部诊断标识，不包含文字、标题或本机路径。
type RequestBlocker struct {
	RequestID string `json:"request_id"`
	Status    string `json:"status"`
	Scope     string `json:"scope"`
}

// Blockers 定位设备/会话占用者；两项唯一约束保证最多两条结果。
func (RequestRepo) Blockers(ctx context.Context, q Queryer, device, desktop, session string) ([]RequestBlocker, error) {
	rows, err := q.QueryContext(ctx, `SELECT request_id,status,
CASE WHEN device_id=? AND desktop_client_id=? AND agent_session_id=? THEN 'device_and_session'
WHEN device_id=? THEN 'device' ELSE 'session' END
FROM requests WHERE status IN ('awaiting_text','awaiting_confirmation','sending','running','unknown')
AND (device_id=? OR (desktop_client_id=? AND agent_session_id=?)) ORDER BY request_id LIMIT 2`,
		device, desktop, session, device, device, desktop, session)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var blockers []RequestBlocker
	for rows.Next() {
		var blocker RequestBlocker
		if err := rows.Scan(&blocker.RequestID, &blocker.Status, &blocker.Scope); err != nil {
			return nil, err
		}
		blockers = append(blockers, blocker)
	}
	return blockers, rows.Err()
}

// ExpiredIDs 用固定截止时间读取待处理请求，供启动及周期清理。
func (RequestRepo) ExpiredIDs(ctx context.Context, q Queryer, now int64) ([]string, error) {
	rows, err := q.QueryContext(ctx, `SELECT request_id FROM requests WHERE
status IN ('awaiting_text','awaiting_confirmation') AND expires_at<=?`, now)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}
