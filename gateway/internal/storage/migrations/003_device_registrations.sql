-- 设备通过已认证连接的 hello 动态登记；MAC 不再由凭据配置决定。
CREATE TABLE device_registrations (
    device_id  TEXT PRIMARY KEY,
    subject    TEXT NOT NULL,
    credential_key TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);

-- 沿用既有绑定中已经保存的账号归属，重启升级后不丢失原绑定。
INSERT INTO device_registrations(device_id, subject, created_at)
SELECT device_id, subject, created_at FROM bindings;
