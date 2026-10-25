import { describe, expect, it } from 'vitest';

import {
  buildCancelFrame,
  buildDispatchFrame,
  summarizeFrame,
  toHistoryEntry,
  CONNECT_PATH,
  DEFAULT_PORT,
  DEFAULT_TOKEN,
  MAX_HISTORY,
} from './mock-gateway-console.mjs';

describe('mock gateway console defaults', () => {
  it('uses the same connect path as the Desktop client', () => {
    expect(CONNECT_PATH).toBe('/v1/clients/connect');
  });

  it('exposes a stable default port and token', () => {
    expect(DEFAULT_PORT).toBe(8787);
    expect(DEFAULT_TOKEN).toBe('demo-client-token');
  });

  it('bounds the history so a long session cannot grow without limit', () => {
    expect(MAX_HISTORY).toBeGreaterThan(0);
  });
});

describe('buildDispatchFrame', () => {
  it('builds a frame the Desktop accepts', () => {
    const frame = buildDispatchFrame();
    expect(frame.protocol_version).toBe('1.0');
    expect(frame.type).toBe('task.dispatch');
    expect(frame.message_id.length).toBeGreaterThan(0);
    expect(typeof frame.sequence).toBe('number');
  });

  it('carries every field of the dispatch payload', () => {
    const { payload } = buildDispatchFrame();
    expect(payload.task_id.length).toBeGreaterThan(0);
    expect(payload.project_id.length).toBeGreaterThan(0);
    expect(payload.agent.length).toBeGreaterThan(0);
    expect(Array.isArray(payload.capabilities)).toBe(true);
    expect(typeof payload.prompt).toBe('string');
    expect(Array.isArray(payload.acceptance_criteria)).toBe(true);
    expect(payload.policy_version.length).toBeGreaterThan(0);
    expect(payload.continuation).toBeNull();
  });

  it('lets the caller override the payload', () => {
    const { payload } = buildDispatchFrame({ prompt: '自定义提示词', task_id: 'tsk_fixed' });
    expect(payload.prompt).toBe('自定义提示词');
    expect(payload.task_id).toBe('tsk_fixed');
  });

  it('sets an expiry in the future', () => {
    const frame = buildDispatchFrame();
    expect(frame.expires_at).toBeDefined();
    const expiresAt = Date.parse(frame.expires_at ?? '');
    expect(expiresAt).toBeGreaterThan(Date.now());
  });
});

describe('buildCancelFrame', () => {
  it('builds a cancel frame for the given task', () => {
    const frame = buildCancelFrame('tsk_123');
    expect(frame.type).toBe('task.cancel');
    expect(frame.payload.task_id).toBe('tsk_123');
    expect(frame.payload.reason.length).toBeGreaterThan(0);
  });

  it('uses the caller reason when given', () => {
    const frame = buildCancelFrame('tsk_123', '测试原因');
    expect(frame.payload.reason).toBe('测试原因');
  });
});

describe('summarizeFrame', () => {
  it('summarises the client hello', () => {
    const summary = summarizeFrame({
      type: 'client.hello',
      payload: {
        desktop_version: '0.1.0',
        protocol_versions: ['1.0'],
        last_acked_sequence: 7,
      },
    });
    expect(summary).toContain('0.1.0');
    expect(summary).toContain('lastAcked 7');
  });

  it('summarises the ack disposition', () => {
    const summary = summarizeFrame({
      type: 'task.ack',
      payload: { task_id: 'tsk_1', disposition: 'accepted' },
    });
    expect(summary).toContain('tsk_1');
    expect(summary).toContain('disposition=accepted');
  });

  it('reports the ack error when the frame carries one', () => {
    const summary = summarizeFrame({
      type: 'task.ack',
      payload: {
        task_id: 'tsk_1',
        disposition: 'rejected',
        error: { message: '客户端正在执行另一项任务' },
      },
    });
    expect(summary).toContain('rejected');
    expect(summary).toContain('客户端正在执行另一项任务');
  });

  it('summarises a task event by state', () => {
    const summary = summarizeFrame({
      type: 'task.started',
      payload: { task_id: 'tsk_1', state: 'running', summary: '已在本机启动 Codex 任务' },
    });
    expect(summary).toContain('tsk_1');
    expect(summary).toContain('state=running');
  });

  it('summarises the heartbeat', () => {
    const summary = summarizeFrame({
      type: 'client.heartbeat',
      payload: { client_time: '2026-10-25T00:00:00.000Z' },
    });
    expect(summary).toContain('client_time');
  });

  it('returns an empty summary for an unknown frame type', () => {
    expect(summarizeFrame({ type: 'who.knows', payload: {} })).toBe('');
  });

  it('survives a frame without a payload', () => {
    expect(summarizeFrame({ type: 'client.hello' })).toContain('desktop -');
  });
});

describe('toHistoryEntry', () => {
  it('builds an entry in the same shape the live stream sends', () => {
    const entry = toHistoryEntry(
      { type: 'task.ack', payload: { task_id: 'tsk_1', disposition: 'accepted' } },
      'inbound',
    );
    expect(entry.direction).toBe('inbound');
    expect(entry.type).toBe('task.ack');
    expect(entry.summary).toContain('accepted');
    expect(entry.id.length).toBeGreaterThan(0);
    expect(entry.frame.type).toBe('task.ack');
  });

  it('marks the outbound direction', () => {
    const entry = toHistoryEntry({ type: 'task.dispatch', payload: {} }, 'outbound');
    expect(entry.direction).toBe('outbound');
  });
});
