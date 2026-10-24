import { describe, expect, it } from 'vitest';

import {
  ACTIVE_TASK_STATES,
  codexStatusLabel,
  isCodexTurnRunning,
  connectionStatus,
  diagnosticsRows,
  filterTasks,
  flattenSessionMessages,
  formatRootPath,
  formatSessionTime,
  getBridge,
  isActiveTask,
  projectLabel,
  projectStateLabel,
  selectTaskId,
  type ProjectFacts,
  type TaskFacts,
} from './view-model.js';

const task = (overrides: Partial<TaskFacts> = {}): TaskFacts => ({
  taskId: 'tsk_1',
  title: 'title',
  projectId: 'prj_1',
  state: 'running',
  stage: 'testing',
  updatedAt: '2026-10-24T08:00:00.000Z',
  needsAttention: false,
  ...overrides,
});

const project = (overrides: Partial<ProjectFacts> = {}): ProjectFacts => ({
  projectId: 'prj_1',
  displayName: '商城前端',
  rootPath: '/Users/demo/Projects/storefront',
  capabilities: ['read'],
  grantVersion: 1,
  enabled: true,
  ...overrides,
});

describe('isActiveTask', () => {
  it('treats in-flight states as active', () => {
    for (const state of ACTIVE_TASK_STATES) {
      expect(isActiveTask(state)).toBe(true);
    }
  });

  it('treats terminal states as inactive', () => {
    expect(isActiveTask('succeeded')).toBe(false);
    expect(isActiveTask('interrupted')).toBe(false);
    expect(isActiveTask('cancelled')).toBe(false);
  });
});

describe('filterTasks', () => {
  const tasks = [
    task({ taskId: 'a', state: 'running' }),
    task({ taskId: 'b', state: 'succeeded' }),
  ];

  it('returns every task when the toggle is off', () => {
    expect(filterTasks(tasks, false)).toHaveLength(2);
  });

  it('returns only active tasks when the toggle is on', () => {
    expect(filterTasks(tasks, true).map((row) => row.taskId)).toEqual(['a']);
  });

  it('does not mutate the input', () => {
    const input = [...tasks];
    filterTasks(input, true);
    expect(input).toHaveLength(2);
  });
});

describe('selectTaskId', () => {
  const tasks = [task({ taskId: 'a' }), task({ taskId: 'b' })];

  it('keeps a still-visible selection', () => {
    expect(selectTaskId(tasks, 'b')).toBe('b');
  });

  it('falls back to the first row when the selection disappeared', () => {
    expect(selectTaskId(tasks, 'missing')).toBe('a');
  });

  it('returns null for an empty list', () => {
    expect(selectTaskId([], 'a')).toBeNull();
  });
});

describe('projectLabel', () => {
  it('resolves the display name', () => {
    expect(projectLabel([project()], 'prj_1')).toBe('商城前端');
  });

  it('falls back to the id for an unknown project', () => {
    expect(projectLabel([project()], 'prj_missing')).toBe('prj_missing');
  });
});

describe('connectionStatus', () => {
  it('reports an online connection', () => {
    const status = connectionStatus({
      configured: true,
      connected: true,
      gatewayUrl: 'wss://example.com/ws',
      tokenHint: '••••abcd',
      computerName: 'mac',
    });
    expect(status.state).toBe('online');
    expect(status.label).toContain('已连接');
  });

  it('distinguishes configured from connected', () => {
    const status = connectionStatus({
      configured: true,
      connected: false,
      gatewayUrl: 'wss://example.com/ws',
      tokenHint: '••••abcd',
      computerName: 'mac',
    });
    expect(status.state).toBe('offline');
    expect(status.label).toContain('已配置');
  });

  it('reports an unconfigured client', () => {
    const status = connectionStatus({
      configured: false,
      connected: false,
      gatewayUrl: null,
      tokenHint: null,
      computerName: null,
    });
    expect(status.state).toBe('offline');
    expect(status.label).toContain('未连接');
  });
});

describe('formatRootPath', () => {
  it('collapses the home directory', () => {
    expect(formatRootPath('/Users/demo/Projects/storefront')).toBe('~/Projects/storefront');
  });

  it('keeps only the last two segments of a deep path', () => {
    expect(formatRootPath('/var/folders/xy/abc/storefront')).toBe('…/abc/storefront');
  });

  it('keeps a short path intact', () => {
    expect(formatRootPath('/tmp/app')).toBe('/tmp/app');
  });
});

describe('projectStateLabel', () => {
  it('labels an enabled project', () => {
    expect(projectStateLabel(project())).toContain('可供灵矽使用');
  });

  it('labels a revoked project', () => {
    expect(projectStateLabel(project({ enabled: false }))).toContain('已停用');
  });
});

describe('diagnosticsRows', () => {
  it('produces one row per local fact', () => {
    const rows = diagnosticsRows({
      desktopVersion: '0.1.0',
      protocolVersions: ['1.0'],
      databasePath: null,
      connected: false,
      lastError: null,
    });
    expect(rows).toHaveLength(5);
    expect(rows[2]?.value).toBe('尚未创建');
    expect(rows[4]?.value).toBe('无');
  });
});

describe('getBridge', () => {
  it('returns null outside a browser context', () => {
    expect(getBridge()).toBeNull();
  });

  it('returns the bridge the preload exposed', () => {
    Object.assign(globalThis, {
      window: { linxbox: { readConnection: () => Promise.resolve(null) } },
    });
    try {
      expect(Object.keys(getBridge() ?? {})).toContain('readConnection');
    } finally {
      Reflect.deleteProperty(globalThis, 'window');
    }
  });

  it('returns null when the window carries no bridge', () => {
    Object.assign(globalThis, { window: {} });
    try {
      expect(getBridge()).toBeNull();
    } finally {
      Reflect.deleteProperty(globalThis, 'window');
    }
  });

  it('returns null when the bridge is not an object', () => {
    Object.assign(globalThis, { window: { linxbox: 'not-a-bridge' } });
    try {
      expect(getBridge()).toBeNull();
    } finally {
      Reflect.deleteProperty(globalThis, 'window');
    }
  });
});

describe('codexStatusLabel', () => {
  it('labels every provider status', () => {
    expect(codexStatusLabel('idle')).toBe('空闲');
    expect(codexStatusLabel('running')).toBe('执行中');
    expect(codexStatusLabel('completed')).toBe('已完成');
    expect(codexStatusLabel('failed')).toBe('失败');
    expect(codexStatusLabel('interrupted')).toBe('已中断');
  });
});

describe('isCodexTurnRunning', () => {
  it('is true only while the turn is in flight', () => {
    const base = {
      threadId: null,
      turnId: null,
      progress: '',
      finalText: '',
      error: null,
      startedAt: null,
      completedAt: null,
    };
    expect(isCodexTurnRunning({ ...base, status: 'running' })).toBe(true);
    expect(isCodexTurnRunning({ ...base, status: 'completed' })).toBe(false);
    expect(isCodexTurnRunning({ ...base, status: 'idle' })).toBe(false);
  });
});

describe('flattenSessionMessages', () => {
  it('flattens every turn into one chronological list', () => {
    const messages = flattenSessionMessages({
      threadId: 'th_1',
      title: '会话',
      cwd: '/tmp/work',
      turns: [
        {
          turnId: 'tn_1',
          status: 'completed',
          startedAt: null,
          messages: [{ id: 'm1', role: 'user', text: '你好', phase: null }],
        },
        {
          turnId: 'tn_2',
          status: 'completed',
          startedAt: null,
          messages: [{ id: 'm2', role: 'assistant', text: '在的', phase: 'final_answer' }],
        },
      ],
    });
    expect(messages.map((message) => message.text)).toEqual(['你好', '在的']);
  });

  it('returns an empty list for a session without turns', () => {
    const messages = flattenSessionMessages({
      threadId: 'th_1',
      title: '会话',
      cwd: null,
      turns: [],
    });
    expect(messages).toEqual([]);
  });
});

describe('formatSessionTime', () => {
  it('formats an ISO timestamp', () => {
    expect(formatSessionTime('2026-10-24T09:26:34.000Z')).not.toBe('-');
  });

  it('keeps a value it cannot parse', () => {
    expect(formatSessionTime('not-a-date')).toBe('not-a-date');
  });

  it('reports a missing timestamp', () => {
    expect(formatSessionTime(null)).toBe('-');
  });
});
