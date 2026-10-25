-- 旧目录未记录上传位置，重新同步前沿用 agent_session_id 排序。
ALTER TABLE session_catalog ADD COLUMN sort_order INTEGER NOT NULL DEFAULT 0;

CREATE INDEX session_catalog_order_idx
    ON session_catalog(desktop_client_id, sort_order, agent_session_id);
