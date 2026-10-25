-- 确认与投递身份在发送前落盘；重启不重新投递结果不确定的任务。
CREATE TABLE request_executions (
    request_id TEXT PRIMARY KEY REFERENCES requests(request_id),
    task_id TEXT NOT NULL UNIQUE,
    dispatch_message_id TEXT NOT NULL UNIQUE,
    dispatch_sequence INTEGER NOT NULL,
    desktop_generation INTEGER NOT NULL,
    confirmed_at INTEGER NOT NULL,
    last_event_sequence INTEGER NOT NULL DEFAULT -1,
    turn_id TEXT NOT NULL DEFAULT '',
    terminal INTEGER NOT NULL DEFAULT 0,
    ack_disposition TEXT NOT NULL DEFAULT '',
    result_json TEXT
);

CREATE TABLE execution_events (
    event_id TEXT PRIMARY KEY,
    task_id TEXT NOT NULL REFERENCES request_executions(task_id),
    event_sequence INTEGER NOT NULL,
    payload_hash TEXT NOT NULL,
    UNIQUE(task_id, event_sequence)
);
