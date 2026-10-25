-- 按 #14 保存录音目标与不可修改的最终 ASR；尚不调度任务。
CREATE TABLE requests (
    request_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    desktop_client_id TEXT NOT NULL,
    agent_session_id TEXT NOT NULL,
    thread_id TEXT NOT NULL,
    project_id TEXT NOT NULL,
    target_title TEXT NOT NULL,
    text TEXT,
    status TEXT NOT NULL,
    error_code TEXT,
    error_message TEXT,
    expires_at INTEGER NOT NULL,
    expired INTEGER NOT NULL DEFAULT 0,
    linx_agent_id TEXT NOT NULL DEFAULT '',
    linx_session_id TEXT NOT NULL DEFAULT ''
);

CREATE UNIQUE INDEX requests_active_device ON requests(device_id)
    WHERE status IN ('awaiting_text','awaiting_confirmation','sending','running','unknown');
CREATE UNIQUE INDEX requests_active_session ON requests(desktop_client_id, agent_session_id)
    WHERE status IN ('awaiting_text','awaiting_confirmation','sending','running','unknown');
CREATE INDEX requests_expiry ON requests(expires_at)
    WHERE status IN ('awaiting_text','awaiting_confirmation');
