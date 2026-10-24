import { describe, expect, it } from 'vitest';

import {
  normalizeMessage,
  normalizeSessionDetail,
  normalizeSessionList,
  normalizeSessionSummary,
  normalizeTurn,
  normalizeTurnList,
} from './history.js';

/** Shape taken from a real `thread/list` response. */
const rawThread = {
  id: '01a16d9d-5e5f-7290-8f73-c8bc049d0737',
  environments: null,
  sessionId: '01a16d9d-5e5f-7290-8f73-c8bc049d0737',
  preview: '只回答一句话：当前工作目录里能看到什么？',
  ephemeral: false,
  modelProvider: 'openai',
  model: 'gpt-5.6-sol',
  createdAt: 1792840394,
  updatedAt: 1792840398,
  status: { type: 'notLoaded' },
  cwd: '/Users/demo/Projects/app',
  cliVersion: '0.164.0-alpha.11.1',
};

const rawUserItem = {
  type: 'userMessage',
  id: '01a16d9d-7080-7320-a050-150b34c79c83',
  clientId: 'linxbox-mvmavq5b',
  content: [{ type: 'text', text: '修复登录页空值错误', text_elements: [] }],
};

const rawAgentItem = {
  type: 'agentMessage',
  id: '01a16d9d-7081-7320-a050-150b34c79c84',
  phase: 'final_answer',
  text: '已定位表单提交前的空值处理，正在修改。',
};

describe('normalizeSessionSummary', () => {
  it('maps a real thread object', () => {
    const summary = normalizeSessionSummary(rawThread);
    expect(summary?.threadId).toBe('01a16d9d-5e5f-7290-8f73-c8bc049d0737');
    expect(summary?.title).toBe('只回答一句话：当前工作目录里能看到什么？');
    expect(summary?.cwd).toBe('/Users/demo/Projects/app');
    expect(summary?.model).toBe('gpt-5.6-sol');
    expect(summary?.status).toBe('notLoaded');
    expect(summary?.createdAt).toBe('2026-10-24T11:13:14.000Z');
  });

  it('returns null for a thread without an id', () => {
    expect(normalizeSessionSummary({ preview: 'x' })).toBeNull();
    expect(normalizeSessionSummary(null)).toBeNull();
    expect(normalizeSessionSummary('nope')).toBeNull();
  });

  it('falls back through title candidates', () => {
    expect(normalizeSessionSummary({ id: 't1', name: '命名会话' })?.title).toBe('命名会话');
    expect(normalizeSessionSummary({ id: 't1', cwd: '/a/b' })?.title).toBe('/a/b');
    expect(normalizeSessionSummary({ id: 't1' })?.title).toBe('(未命名会话)');
  });

  it('reads the cwd out of the first environment', () => {
    const summary = normalizeSessionSummary({
      id: 't1',
      environments: [{ environmentId: 'local', cwd: '/tmp/work' }],
    });
    expect(summary?.cwd).toBe('/tmp/work');
  });

  it('treats an unknown status as unknown', () => {
    expect(normalizeSessionSummary({ id: 't1', status: 'weird' })?.status).toBe('unknown');
  });
});

describe('normalizeSessionList', () => {
  it('maps the data array of a thread list', () => {
    const list = normalizeSessionList({ data: [rawThread, { preview: 'no id' }] });
    expect(list).toHaveLength(1);
    expect(list[0]?.threadId).toBe('01a16d9d-5e5f-7290-8f73-c8bc049d0737');
  });

  it('returns an empty list for an unexpected payload', () => {
    expect(normalizeSessionList(null)).toEqual([]);
    expect(normalizeSessionList({ data: 'not-an-array' })).toEqual([]);
  });
});

describe('normalizeMessage', () => {
  it('maps a user message', () => {
    const message = normalizeMessage(rawUserItem);
    expect(message?.role).toBe('user');
    expect(message?.text).toBe('修复登录页空值错误');
    expect(message?.phase).toBeNull();
  });

  it('maps an agent message and keeps its phase', () => {
    const message = normalizeMessage(rawAgentItem);
    expect(message?.role).toBe('assistant');
    expect(message?.text).toBe('已定位表单提交前的空值处理，正在修改。');
    expect(message?.phase).toBe('final_answer');
  });

  it('joins several text parts', () => {
    const message = normalizeMessage({
      type: 'userMessage',
      content: [
        { type: 'text', text: '第一段' },
        { type: 'text', text: '第二段' },
      ],
    });
    expect(message?.text).toBe('第一段\n第二段');
  });

  it('ignores items that carry no readable text', () => {
    expect(normalizeMessage({ type: 'commandExecution', text: 'ls' })).toBeNull();
    expect(normalizeMessage({ type: 'userMessage', content: [] })).toBeNull();
    expect(normalizeMessage({ type: 'agentMessage' })).toBeNull();
    expect(normalizeMessage(null)).toBeNull();
  });
});

describe('normalizeTurn', () => {
  it('keeps only readable messages', () => {
    const turn = normalizeTurn({
      id: 'tn_1',
      status: 'completed',
      startedAt: 1792840394,
      items: [rawUserItem, rawAgentItem, { type: 'commandExecution' }],
    });
    expect(turn.turnId).toBe('tn_1');
    expect(turn.status).toBe('completed');
    expect(turn.messages).toHaveLength(2);
    expect(turn.startedAt).toBe('2026-10-24T11:13:14.000Z');
  });

  it('handles a turn without items', () => {
    const turn = normalizeTurn({ id: 'tn_2' });
    expect(turn.status).toBe('unknown');
    expect(turn.messages).toEqual([]);
  });
});

describe('normalizeSessionDetail', () => {
  it('maps a thread with its turns', () => {
    const detail = normalizeSessionDetail({
      thread: {
        ...rawThread,
        turns: [
          {
            id: 'tn_1',
            status: 'completed',
            startedAt: 1792840394,
            items: [rawUserItem, rawAgentItem],
          },
        ],
      },
      readState: { revision: 'x' },
    });
    expect(detail?.threadId).toBe('01a16d9d-5e5f-7290-8f73-c8bc049d0737');
    expect(detail?.turns).toHaveLength(1);
    expect(detail?.turns[0]?.messages).toHaveLength(2);
  });

  it('returns null without a thread id', () => {
    expect(normalizeSessionDetail({ thread: {} })).toBeNull();
    expect(normalizeSessionDetail(null)).toBeNull();
  });
});

describe('normalizeTurnList', () => {
  it('maps the data array of a turn list', () => {
    const turns = normalizeTurnList({ data: [{ id: 'tn_1', status: 'failed', items: [] }] });
    expect(turns).toHaveLength(1);
    expect(turns[0]?.status).toBe('failed');
  });

  it('returns an empty list for an unexpected payload', () => {
    expect(normalizeTurnList({})).toEqual([]);
  });
});
