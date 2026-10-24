/**
 * 本地 SQLite schema 与有序迁移。
 *
 * 契约来源：SPEC-0003 §7.1（本地表）与 §6.4（单调 schema version 与事务迁移）。
 *
 * 迁移列表是**追加式**的：已经发布的迁移不允许修改，只能在末尾追加新的版本。
 * 数据库版本高于当前构建时，旧版本必须拒绝打开，而不是静默损坏。
 */

/** 当前构建支持的 schema 版本。 */
export const SCHEMA_VERSION = 1;

/** 一次迁移。 */
export interface SchemaMigration {
  readonly version: number;
  readonly name: string;
  readonly sql: string;
}

/**
 * 迁移 1：建立 SPEC-0003 §7.1 的全部本地表。
 *
 * 约束：
 * - `deliveries.message_id` 唯一（message ID 至多执行一次）；
 * - `tasks.task_id` 唯一；
 * - `tasks.terminal_event_id` 是单列，天然保证每个任务至多一个 terminal event；
 * - `task_events` 的 `(task_id, event_sequence)` 唯一，`event_id` 全局唯一（重放复用同一 event ID）。
 */
const INITIAL_SCHEMA_SQL = `
CREATE TABLE IF NOT EXISTS schema_meta (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS projects (
  project_id TEXT PRIMARY KEY,
  display_name TEXT NOT NULL,
  realpath TEXT NOT NULL,
  capabilities TEXT NOT NULL,
  grant_version INTEGER NOT NULL,
  revoked_at TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS deliveries (
  message_id TEXT PRIMARY KEY,
  sequence INTEGER NOT NULL,
  type TEXT NOT NULL,
  payload_hash TEXT NOT NULL,
  received_at TEXT NOT NULL,
  disposition TEXT NOT NULL,
  error_code TEXT,
  error_message TEXT,
  acked_at TEXT
);

CREATE TABLE IF NOT EXISTS tasks (
  task_id TEXT PRIMARY KEY,
  project_id TEXT NOT NULL,
  state TEXT NOT NULL,
  stage TEXT NOT NULL,
  policy_version TEXT NOT NULL,
  grant_version INTEGER NOT NULL,
  event_sequence INTEGER NOT NULL DEFAULT 0,
  terminal_event_id TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS provider_ownership (
  task_id TEXT PRIMARY KEY,
  project_id TEXT NOT NULL,
  thread_id TEXT NOT NULL,
  turn_id TEXT NOT NULL,
  provider_version TEXT NOT NULL,
  created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS task_events (
  event_id TEXT PRIMARY KEY,
  task_id TEXT NOT NULL,
  event_sequence INTEGER NOT NULL,
  occurred_at TEXT NOT NULL,
  state TEXT NOT NULL,
  stage TEXT NOT NULL,
  summary TEXT NOT NULL,
  result_json TEXT,
  error_json TEXT,
  UNIQUE (task_id, event_sequence)
);

CREATE TABLE IF NOT EXISTS local_results (
  task_id TEXT PRIMARY KEY,
  projection_version TEXT NOT NULL,
  local_detail_json TEXT NOT NULL,
  cloud_summary_json TEXT,
  device_content_json TEXT,
  generated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS outgoing_frames (
  frame_id TEXT PRIMARY KEY,
  type TEXT NOT NULL,
  task_id TEXT,
  payload_json TEXT NOT NULL,
  status TEXT NOT NULL,
  attempt_count INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL,
  sent_at TEXT
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_deliveries_sequence ON deliveries (sequence);
CREATE INDEX IF NOT EXISTS idx_tasks_project ON tasks (project_id);
CREATE INDEX IF NOT EXISTS idx_task_events_task ON task_events (task_id, event_sequence);
CREATE INDEX IF NOT EXISTS idx_outgoing_frames_status ON outgoing_frames (status);
`;

/** 有序、追加式的迁移列表。 */
export const SCHEMA_MIGRATIONS: readonly SchemaMigration[] = [
  { version: 1, name: 'initial_local_tables', sql: INITIAL_SCHEMA_SQL },
];

/**
 * 把版本 N 的数据库升级到当前版本所需要的迁移。
 *
 * 迁移按版本升序返回；版本高于当前构建时抛错（不允许降级打开）。
 */
export function migrationsNeeded(currentVersion: number): readonly SchemaMigration[] {
  assertSchemaVersionSupported(currentVersion);
  return SCHEMA_MIGRATIONS.filter((migration) => migration.version > currentVersion);
}

/** 数据库版本是否可以被当前构建打开。 */
export function isSupportedSchemaVersion(version: number): boolean {
  return Number.isInteger(version) && version >= 0 && version <= SCHEMA_VERSION;
}

/**
 * 断言数据库版本可被当前构建打开。
 *
 * 版本高于当前构建时必须拒绝打开：旧版本无法理解新 schema 的语义，静默打开会损坏数据。
 */
export function assertSchemaVersionSupported(version: number): void {
  if (!Number.isInteger(version) || version < 0) {
    throw new Error(`非法的数据库 schema 版本: ${version}`);
  }
  if (version > SCHEMA_VERSION) {
    throw new Error(
      `数据库 schema 版本 ${version} 高于当前构建支持的版本 ${SCHEMA_VERSION}，拒绝打开`,
    );
  }
}
