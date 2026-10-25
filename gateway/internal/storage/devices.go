package storage

import (
	"context"
	"time"
)

// DeviceRepo 保存 hello 登记的 MAC 与账号；既有归属不能被后来的连接覆盖。
type DeviceRepo struct{}

type DeviceRow struct {
	DeviceID      string
	Subject       string
	CredentialKey string
}

// Register 在调用方事务内首次写入；重复登记保留原 subject，由服务层核对。
func (DeviceRepo) Register(ctx context.Context, q Queryer, deviceID, subject, key string) (*DeviceRow, error) {
	if _, err := q.ExecContext(ctx, `INSERT INTO device_registrations(device_id, subject, credential_key, created_at)
		VALUES(?, ?, ?, ?) ON CONFLICT(device_id) DO UPDATE SET credential_key = excluded.credential_key
		WHERE device_registrations.subject = excluded.subject`, deviceID, subject, key, time.Now().UTC().Format(time.RFC3339)); err != nil {
		return nil, err
	}
	return (DeviceRepo{}).Get(ctx, q, deviceID)
}

func (DeviceRepo) Get(ctx context.Context, q Queryer, deviceID string) (*DeviceRow, error) {
	var row DeviceRow
	err := q.QueryRowContext(ctx, `SELECT device_id, subject, credential_key FROM device_registrations WHERE device_id = ?`, deviceID).
		Scan(&row.DeviceID, &row.Subject, &row.CredentialKey)
	return &row, err
}

// AdoptLegacyCredential 为旧绑定补齐凭据关联，不改变设备或绑定的既有账号。
func (DeviceRepo) AdoptLegacyCredential(ctx context.Context, q Queryer, deviceID, subject, key string) error {
	_, err := q.ExecContext(ctx, `UPDATE device_registrations SET credential_key = ?
		WHERE device_id = ? AND subject = ? AND credential_key = ''`, key, deviceID, subject)
	return err
}
