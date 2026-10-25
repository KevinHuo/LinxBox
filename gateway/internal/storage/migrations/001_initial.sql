-- 001_initial: 首版绑定与会话目录。
-- 请求、任务、投递与执行事件表由后续阶段迁移补充。

CREATE TABLE IF NOT EXISTS bindings (
    device_id          TEXT PRIMARY KEY,
    desktop_client_id  TEXT NOT NULL,
    subject            TEXT NOT NULL,
    created_at         TEXT NOT NULL
);

-- 一个设备只能有一个有效绑定；同 Desktop 重复绑定幂等。
CREATE UNIQUE INDEX IF NOT EXISTS bindings_device_uq ON bindings(device_id);
CREATE UNIQUE INDEX IF NOT EXISTS bindings_desktop_device_uq ON bindings(desktop_client_id, device_id);

CREATE TABLE IF NOT EXISTS session_catalog (
    desktop_client_id  TEXT NOT NULL,
    agent_session_id   TEXT NOT NULL,
    thread_id          TEXT NOT NULL,
    project_id         TEXT NOT NULL,
    project_name       TEXT NOT NULL,
    agent              TEXT NOT NULL,
    title              TEXT NOT NULL,
    status             TEXT NOT NULL,
    summary            TEXT NOT NULL,
    updated_at         TEXT NOT NULL,
    PRIMARY KEY (desktop_client_id, agent_session_id)
);

CREATE INDEX IF NOT EXISTS session_catalog_desktop_idx ON session_catalog(desktop_client_id);

-- 目录版本：同步后 +1，旧 cursor 作废，固件从首批重读。
CREATE TABLE IF NOT EXISTS catalog_meta (
    desktop_client_id  TEXT PRIMARY KEY,
    version            INTEGER NOT NULL,
    updated_at         TEXT NOT NULL
);
