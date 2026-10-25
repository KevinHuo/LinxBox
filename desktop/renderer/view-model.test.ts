import { describe, expect, it } from 'vitest';

import {
  ACTIVE_TASK_STATES,
  clampBallPosition,
  deviceSubtitle,
  getBridge,
  headPill,
  homeHeadline,
  isActiveTask,
  isGlobalView,
  isValidMacAddress,
  normalizeMacAddress,
  pendingTaskCount,
  railAppForView,
  taskRows,
  taskStateLabel,
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

describe('isValidMacAddress', () => {
  it('accepts a colon separated address', () => {
    expect(isValidMacAddress('A4:C1:38:5B:2A:9E')).toBe(true);
  });

  it('accepts lower case input', () => {
    expect(isValidMacAddress('a4:c1:38:5b:2a:9e')).toBe(true);
  });

  it('tolerates surrounding whitespace', () => {
    expect(isValidMacAddress('  A4:C1:38:5B:2A:9E  ')).toBe(true);
  });

  it('rejects a malformed address', () => {
    expect(isValidMacAddress('A4:C1:38:5B:2A')).toBe(false);
    expect(isValidMacAddress('A4-C1-38-5B-2A-9E')).toBe(false);
    expect(isValidMacAddress('')).toBe(false);
    expect(isValidMacAddress('ZZ:C1:38:5B:2A:9E')).toBe(false);
  });
});

describe('normalizeMacAddress', () => {
  it('upper cases and trims the address', () => {
    expect(normalizeMacAddress('  a4:c1:38:5b:2a:9e ')).toBe('A4:C1:38:5B:2A:9E');
  });
});

describe('headPill', () => {
  it('distinguishes configured from connected', () => {
    const pill = headPill({
      configured: true,
      connected: false,
      gatewayUrl: 'wss://example.com/ws',
      tokenHint: '••••abcd',
      computerName: 'mac',
    });
    expect(pill.on).toBe(false);
    expect(pill.text).toContain('已配置');
  });

  it('reports an unconfigured client', () => {
    const pill = headPill({
      configured: false,
      connected: false,
      gatewayUrl: null,
      tokenHint: null,
      computerName: null,
    });
    expect(pill.on).toBe(false);
    expect(pill.text).toContain('未配对');
  });
});

describe('homeHeadline', () => {
  it('claims the device is online only while connected', () => {
    expect(
      homeHeadline({
        configured: true,
        connected: true,
        gatewayUrl: 'wss://example.com/ws',
        tokenHint: '••••abcd',
        computerName: 'mac',
      }, true),
    ).toBe('已配对 · 设备在线');
  });

  it('does not claim a device is online while disconnected', () => {
    expect(
      homeHeadline({
        configured: false,
        connected: false,
        gatewayUrl: null,
        tokenHint: null,
        computerName: null,
      }, true),
    ).toBe('尚未配对 · 等待设备');
  });
});

describe('deviceSubtitle', () => {
  it('reports a live connection', () => {
    expect(
      deviceSubtitle({
        configured: true,
        connected: true,
        gatewayUrl: null,
        tokenHint: null,
        computerName: 'mac',
      }, true),
    ).toBe('已连接 · 这台 Mac');
  });

  it('names the machine that runs the client', () => {
    expect(
      deviceSubtitle({
        configured: true,
        connected: false,
        gatewayUrl: null,
        tokenHint: null,
        computerName: 'mac',
      }, false),
    ).toBe('WSS 已连接 · 等待设备绑定');
  });
});

describe('taskStateLabel', () => {
  it('labels the in-flight states', () => {
    expect(taskStateLabel('dispatched')).toBe('已下发');
    expect(taskStateLabel('accepted')).toBe('已接受');
    expect(taskStateLabel('running')).toBe('运行中');
    expect(taskStateLabel('approval_required')).toBe('等你回复');
    expect(taskStateLabel('cancel_requested')).toBe('正在取消');
  });

  it('labels the terminal states', () => {
    expect(taskStateLabel('succeeded')).toBe('本轮结束');
    expect(taskStateLabel('failed')).toBe('失败');
    expect(taskStateLabel('interrupted')).toBe('已中断');
    expect(taskStateLabel('rejected')).toBe('已拒绝');
  });

  it('falls back for an unknown state', () => {
    expect(taskStateLabel('something_new')).toBe('进行中');
  });
});

describe('taskRows', () => {
  it('resolves the project name into the row detail', () => {
    const rows = taskRows([task()], [project()]);
    expect(rows[0]?.detail).toContain('商城前端');
    expect(rows[0]?.detail).toContain('testing');
  });

  it('marks an unknown project instead of hiding it', () => {
    const rows = taskRows([task()], []);
    expect(rows[0]?.detail).toContain('未授权项目');
  });

  it('highlights running tasks and mutes finished ones', () => {
    const rows = taskRows(
      [task({ taskId: 'a', state: 'running' }), task({ taskId: 'b', state: 'succeeded' })],
      [project()],
    );
    expect(rows[0]?.accent).toBe(true);
    expect(rows[0]?.muted).toBe(false);
    expect(rows[1]?.accent).toBe(false);
    expect(rows[1]?.muted).toBe(true);
  });

  it('returns an empty list for no tasks', () => {
    expect(taskRows([], [])).toEqual([]);
  });
});

describe('pendingTaskCount', () => {
  it('counts only the tasks still in flight', () => {
    const tasks = [
      task({ taskId: 'a', state: 'running' }),
      task({ taskId: 'b', state: 'succeeded' }),
      task({ taskId: 'c', state: 'dispatched' }),
    ];
    expect(pendingTaskCount(tasks)).toBe(2);
  });

  it('counts nothing when every task finished', () => {
    expect(pendingTaskCount([task({ state: 'succeeded' })])).toBe(0);
  });
});

describe('isGlobalView', () => {
  it('treats the settings view as global', () => {
    expect(isGlobalView('settings')).toBe(true);
    expect(isGlobalView('pair')).toBe(false);
    expect(isGlobalView('pairing')).toBe(false);
    expect(isGlobalView('home')).toBe(false);
  });
});

describe('railAppForView', () => {
  it('points the settings view at the settings entry', () => {
    expect(railAppForView('settings')).toBe('settings');
  });

  it('points every other view at the box entry', () => {
    expect(railAppForView('pair')).toBe('box');
    expect(railAppForView('pairing')).toBe('box');
    expect(railAppForView('home')).toBe('box');
  });
});

describe('clampBallPosition', () => {
  it('keeps a position inside the bounds', () => {
    expect(clampBallPosition({ x: 100, y: 120 }, { width: 800, height: 600 }, 62)).toEqual({
      x: 100,
      y: 120,
    });
  });

  it('clamps a negative position to the origin', () => {
    expect(clampBallPosition({ x: -50, y: -30 }, { width: 800, height: 600 }, 62)).toEqual({
      x: 0,
      y: 0,
    });
  });

  it('clamps a position past the far edge', () => {
    expect(clampBallPosition({ x: 5000, y: 5000 }, { width: 800, height: 600 }, 62)).toEqual({
      x: 738,
      y: 538,
    });
  });

  it('copes with a viewport smaller than the ball', () => {
    expect(clampBallPosition({ x: 10, y: 10 }, { width: 40, height: 40 }, 62)).toEqual({
      x: 0,
      y: 0,
    });
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
