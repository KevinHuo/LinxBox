import { describe, expect, it } from 'vitest';

import {
  REDACTED,
  REDACTED_PATH,
  consoleSink,
  createLogger,
  redactFields,
  redactText,
  redactValue,
  type LogRecord,
} from './logger.js';

describe('redactValue', () => {
  it('redacts secret-looking keys', () => {
    expect(redactValue('accessToken', 'super-secret')).toBe(REDACTED);
    expect(redactValue('client_secret', 'super-secret')).toBe(REDACTED);
    expect(redactValue('sessionId', 'abc')).toBe(REDACTED);
  });

  it('keeps numbers and booleans', () => {
    expect(redactValue('count', 3)).toBe(3);
    expect(redactValue('ok', true)).toBe(true);
  });

  it('redacts null and undefined', () => {
    expect(redactValue('value', null)).toBe(REDACTED);
    expect(redactValue('value', undefined)).toBe(REDACTED);
  });

  it('redacts a path inside a plain field', () => {
    expect(redactValue('note', 'see /Users/demo/project/file.ts')).toContain(REDACTED_PATH);
  });
});

describe('redactText', () => {
  it('replaces the macOS home prefix', () => {
    expect(redactText('opened /Users/demo/project')).toBe(`opened ${REDACTED_PATH}`);
  });

  it('keeps the path visible but marked outside the home prefix', () => {
    expect(redactText('opened /opt/data/file')).toBe(`opened ${REDACTED_PATH}/opt/data/file`);
  });

  it('truncates very long text', () => {
    const long = 'a'.repeat(500);
    const result = redactText(long);
    expect(result.length).toBeLessThanOrEqual(201);
    expect(result.endsWith('…')).toBe(true);
  });
});

describe('redactFields', () => {
  it('redacts every field', () => {
    const result = redactFields({ accessToken: 'abc', projectId: 'prj_1' });
    expect(result['accessToken']).toBe(REDACTED);
    expect(result['projectId']).toBe('prj_1');
  });
});

describe('createLogger', () => {
  const fixedDate = new Date('2026-10-24T08:00:00.000Z');

  it('emits a redacted record', () => {
    const records: LogRecord[] = [];
    const logger = createLogger('test', {
      sink: (record) => records.push(record),
      now: () => fixedDate,
    });

    logger.info('hello', { accessToken: 'secret' });

    expect(records).toHaveLength(1);
    expect(records[0]?.level).toBe('info');
    expect(records[0]?.scope).toBe('test');
    expect(records[0]?.at).toBe('2026-10-24T08:00:00.000Z');
    expect(records[0]?.fields['accessToken']).toBe(REDACTED);
  });

  it('drops records below the minimum level', () => {
    const records: LogRecord[] = [];
    const logger = createLogger('test', {
      sink: (record) => records.push(record),
      minLevel: 'warn',
    });

    logger.debug('noise');
    logger.info('noise');
    logger.warn('kept');
    logger.error('kept');

    expect(records.map((record) => record.level)).toEqual(['warn', 'error']);
  });

  it('writes errors and warnings to the console sink', () => {
    const errors: unknown[][] = [];
    const warns: unknown[][] = [];
    const originalError = console.error;
    const originalWarn = console.warn;
    console.error = (...args: unknown[]) => errors.push(args);
    console.warn = (...args: unknown[]) => warns.push(args);

    try {
      const logger = createLogger('console');
      logger.error('bad', { code: 'INTERNAL' });
      logger.warn('meh');
    } finally {
      console.error = originalError;
      console.warn = originalWarn;
    }

    expect(errors).toHaveLength(1);
    expect(errors[0]?.[0]).toContain('INTERNAL');
    expect(warns).toHaveLength(1);
    expect(warns[0]?.[0]).not.toContain('INTERNAL');
  });

  it('exposes a console sink directly', () => {
    expect(typeof consoleSink).toBe('function');
  });
});
