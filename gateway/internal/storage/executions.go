package storage

import (
	"context"
	"database/sql"
)

// ExecutionRow 是已确认任务的持久身份；不因断线或目录替换而删除。
type ExecutionRow struct {
	RequestID, TaskID, MessageID string
	Sequence                     int64
	Generation                   uint64
	ConfirmedAt, LastEvent       int64
	TurnID, Ack                  string
	Terminal                     bool
	Result                       *string
}

// ExecutionRepo 保存投递身份、顺序与事件指纹，不保存原始进度文字。
type ExecutionRepo struct{}

const executionColumns = `request_id,task_id,dispatch_message_id,dispatch_sequence,
desktop_generation,confirmed_at,last_event_sequence,turn_id,terminal,ack_disposition,result_json`

func scanExecution(row *sql.Row) (*ExecutionRow, error) {
	var e ExecutionRow
	err := row.Scan(&e.RequestID, &e.TaskID, &e.MessageID, &e.Sequence, &e.Generation,
		&e.ConfirmedAt, &e.LastEvent, &e.TurnID, &e.Terminal, &e.Ack, &e.Result)
	return &e, err
}

// ByRequest 查找原确认；重复确认只读此身份，不生成第二次投递。
func (ExecutionRepo) ByRequest(ctx context.Context, q Queryer, id string) (*ExecutionRow, error) {
	return scanExecution(q.QueryRowContext(ctx, `SELECT `+executionColumns+` FROM request_executions WHERE request_id=?`, id))
}

// ByTask 从 Desktop 的 task_id 恢复原请求。
func (ExecutionRepo) ByTask(ctx context.Context, q Queryer, id string) (*ExecutionRow, error) {
	return scanExecution(q.QueryRowContext(ctx, `SELECT `+executionColumns+` FROM request_executions WHERE task_id=?`, id))
}

// Insert 与 Request.sending 在同一事务内提交。
func (ExecutionRepo) Insert(ctx context.Context, q Queryer, e *ExecutionRow) error {
	_, err := q.ExecContext(ctx, `INSERT INTO request_executions (`+executionColumns+`) VALUES (?,?,?,?,?,?,?,?,?,?,?)`,
		e.RequestID, e.TaskID, e.MessageID, e.Sequence, e.Generation, e.ConfirmedAt, e.LastEvent, e.TurnID, e.Terminal, e.Ack, e.Result)
	return err
}

// Save 只修改执行事实，不改动原投递身份。
func (ExecutionRepo) Save(ctx context.Context, q Queryer, e *ExecutionRow) error {
	_, err := q.ExecContext(ctx, `UPDATE request_executions SET desktop_generation=?,last_event_sequence=?,turn_id=?,terminal=?,ack_disposition=?,result_json=? WHERE request_id=?`,
		e.Generation, e.LastEvent, e.TurnID, e.Terminal, e.Ack, e.Result, e.RequestID)
	return err
}

// EventHash 用于识别完全相同的重复事件和 event_id 冲突。
func (ExecutionRepo) EventHash(ctx context.Context, q Queryer, id string) (string, error) {
	var hash string
	err := q.QueryRowContext(ctx, `SELECT payload_hash FROM execution_events WHERE event_id=?`, id).Scan(&hash)
	return hash, err
}

// AddEvent 与状态更新一起提交，约束同一任务的事件序号唯一。
func (ExecutionRepo) AddEvent(ctx context.Context, q Queryer, task, id string, sequence int64, hash string) error {
	_, err := q.ExecContext(ctx, `INSERT INTO execution_events(event_id,task_id,event_sequence,payload_hash) VALUES(?,?,?,?)`, id, task, sequence, hash)
	return err
}

// ActiveIDs 返回尚未确定终态的已投递请求；generation=0 表示进程重启恢复。
func (ExecutionRepo) ActiveIDs(ctx context.Context, q Queryer, desktop string, generation uint64) ([]string, error) {
	rows, err := q.QueryContext(ctx, `SELECT r.request_id FROM requests r JOIN request_executions e ON e.request_id=r.request_id
WHERE r.status IN ('sending','running') AND (?='' OR (r.desktop_client_id=? AND e.desktop_generation=?))`, desktop, desktop, generation)
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
