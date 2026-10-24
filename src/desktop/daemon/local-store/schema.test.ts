import { describe, expect, it } from 'vitest';

import {
  SCHEMA_MIGRATIONS,
  SCHEMA_VERSION,
  assertSchemaVersionSupported,
  isSupportedSchemaVersion,
  migrationsNeeded,
} from './schema.js';

const REQUIRED_TABLES = [
  'projects',
  'deliveries',
  'tasks',
  'provider_ownership',
  'task_events',
  'local_results',
  'outgoing_frames',
] as const;

describe('SCHEMA_VERSION', () => {
  it('是单调递增的整数版本', () => {
    expect(Number.isInteger(SCHEMA_VERSION)).toBe(true);
    expect(SCHEMA_VERSION).toBeGreaterThan(0);
  });
});

describe('SCHEMA_MIGRATIONS', () => {
  it('版本严格递增且从 1 开始', () => {
    const versions = SCHEMA_MIGRATIONS.map((migration) => migration.version);
    expect(versions[0]).toBe(1);
    for (let index = 1; index < versions.length; index += 1) {
      const previous = versions[index - 1];
      const current = versions[index];
      expect(previous).toBeDefined();
      expect(current).toBeDefined();
      expect(current as number).toBe((previous as number) + 1);
    }
  });

  it('最后一条迁移就是当前版本', () => {
    const last = SCHEMA_MIGRATIONS[SCHEMA_MIGRATIONS.length - 1];
    expect(last?.version).toBe(SCHEMA_VERSION);
  });

  it('每条迁移都有名字与 SQL', () => {
    for (const migration of SCHEMA_MIGRATIONS) {
      expect(migration.name.length).toBeGreaterThan(0);
      expect(migration.sql.trim().length).toBeGreaterThan(0);
    }
  });

  it('建立 SPEC-0003 §7.1 的全部本地表', () => {
    const sql = SCHEMA_MIGRATIONS[0]?.sql ?? '';
    for (const table of REQUIRED_TABLES) {
      expect(sql).toContain(`CREATE TABLE IF NOT EXISTS ${table}`);
    }
  });

  it('deliveries 的 message_id 唯一', () => {
    const sql = SCHEMA_MIGRATIONS[0]?.sql ?? '';
    expect(sql).toMatch(
      /CREATE TABLE IF NOT EXISTS deliveries \([\s\S]*?message_id TEXT PRIMARY KEY/,
    );
  });

  it('tasks 的 task_id 唯一', () => {
    const sql = SCHEMA_MIGRATIONS[0]?.sql ?? '';
    expect(sql).toMatch(/CREATE TABLE IF NOT EXISTS tasks \([\s\S]*?task_id TEXT PRIMARY KEY/);
  });

  it('每个任务至多一个 terminal event', () => {
    const sql = SCHEMA_MIGRATIONS[0]?.sql ?? '';
    expect(sql).toMatch(/CREATE TABLE IF NOT EXISTS tasks \([\s\S]*?terminal_event_id TEXT/);
  });

  it('task_events 的 (task_id, event_sequence) 唯一', () => {
    const sql = SCHEMA_MIGRATIONS[0]?.sql ?? '';
    expect(sql).toContain('UNIQUE (task_id, event_sequence)');
    expect(sql).toMatch(
      /CREATE TABLE IF NOT EXISTS task_events \([\s\S]*?event_id TEXT PRIMARY KEY/,
    );
  });
});

describe('migrationsNeeded', () => {
  it('空库需要全部迁移', () => {
    expect(migrationsNeeded(0)).toHaveLength(SCHEMA_MIGRATIONS.length);
  });

  it('已是当前版本时不需要迁移', () => {
    expect(migrationsNeeded(SCHEMA_VERSION)).toHaveLength(0);
  });

  it('数据库版本高于当前构建时拒绝', () => {
    expect(() => migrationsNeeded(SCHEMA_VERSION + 1)).toThrow(/拒绝打开/);
  });
});

describe('isSupportedSchemaVersion', () => {
  it('接受 0 到当前版本', () => {
    expect(isSupportedSchemaVersion(0)).toBe(true);
    expect(isSupportedSchemaVersion(SCHEMA_VERSION)).toBe(true);
  });

  it('拒绝更高版本与非法版本', () => {
    expect(isSupportedSchemaVersion(SCHEMA_VERSION + 1)).toBe(false);
    expect(isSupportedSchemaVersion(-1)).toBe(false);
    expect(isSupportedSchemaVersion(1.5)).toBe(false);
  });
});

describe('assertSchemaVersionSupported', () => {
  it('当前版本可以通过', () => {
    expect(() => assertSchemaVersionSupported(SCHEMA_VERSION)).not.toThrow();
  });

  it('更高版本必须拒绝打开', () => {
    expect(() => assertSchemaVersionSupported(SCHEMA_VERSION + 1)).toThrow(/拒绝打开/);
  });

  it('非法版本被拒绝', () => {
    expect(() => assertSchemaVersionSupported(-1)).toThrow(/非法/);
    expect(() => assertSchemaVersionSupported(1.5)).toThrow(/非法/);
  });
});
