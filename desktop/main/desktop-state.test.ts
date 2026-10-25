import { describe, expect, it } from 'vitest';

import {
  cancelTask,
  findAuthorizedProject,
  createDesktopState,
  getTask,
  grantProject,
  listProjects,
  listTasks,
  readConnectionView,
  readDiagnostics,
  revokeProject,
  saveConnection,
  type ProjectRecord,
  type TaskRecord,
} from './desktop-state.js';

const task = (overrides: Partial<TaskRecord> = {}): TaskRecord => ({
  taskId: 'tsk_1',
  title: '修复登录页空值错误',
  projectId: 'prj_1',
  state: 'running',
  stage: 'testing',
  intent: '修复空值',
  summary: '正在测试',
  updatedAt: '2026-10-24T08:00:00.000Z',
  needsAttention: false,
  changes: [],
  verification: [],
  risks: [],
  needsUserAction: null,
  localDiff: null,
  ...overrides,
});

describe('connection', () => {
  it('reports an unconfigured connection', () => {
    const state = createDesktopState();
    const view = readConnectionView(state);
    expect(view.configured).toBe(false);
    expect(view.connected).toBe(false);
    expect(view.tokenHint).toBeNull();
  });

  it('never exposes the raw token', () => {
    const state = createDesktopState();
    saveConnection(state, {
      profile: 'online',
      gatewayUrl: 'wss://example.com/ws',
      accessToken: 'super-secret-token',
      computerName: 'demo mac',
    });
    const view = readConnectionView(state);
    expect(view.tokenHint).toBe('••••••••oken');
    expect(JSON.stringify(view)).not.toContain('super-secret-token');
  });
});

describe('projects', () => {
  it('grants a project with an increasing grant version', () => {
    const state = createDesktopState();
    const first = grantProject(
      state,
      { displayName: '商城前端', capabilities: ['read'] },
      '/tmp/app',
    );
    expect(first.grantVersion).toBe(1);
    expect(first.enabled).toBe(true);

    const second = grantProject(
      state,
      { displayName: '商城前端', capabilities: ['read'] },
      '/tmp/app',
    );
    expect(second.projectId).toBe(first.projectId);
    expect(second.grantVersion).toBe(2);
  });

  it('keeps the root path on the local record', () => {
    const state = createDesktopState();
    grantProject(state, { displayName: 'app', capabilities: ['read'] }, '/tmp/app');
    expect(listProjects(state)[0]?.rootPath).toBe('/tmp/app');
  });

  it('revokes a grant and bumps its version', () => {
    const state = createDesktopState();
    const project = grantProject(state, { displayName: 'app', capabilities: ['read'] }, '/tmp/app');
    expect(revokeProject(state, project.projectId)).toBe(true);

    const revoked = listProjects(state)[0];
    expect(revoked?.enabled).toBe(false);
    expect(revoked?.grantVersion).toBe(2);
  });

  it('reports an unknown project on revoke', () => {
    expect(revokeProject(createDesktopState(), 'prj_missing')).toBe(false);
  });

  it('accepts a seeded project', () => {
    const seeded: ProjectRecord = {
      projectId: 'prj_seed',
      displayName: 'seed',
      rootPath: '/tmp/seed',
      capabilities: ['read'],
      grantVersion: 3,
      enabled: true,
    };
    const state = createDesktopState({ projects: [seeded] });
    expect(listProjects(state)).toHaveLength(1);
  });
});

describe('tasks', () => {
  it('lists tasks most recently updated first', () => {
    const state = createDesktopState({
      tasks: [
        task({ taskId: 'tsk_old', updatedAt: '2026-10-23T08:00:00.000Z' }),
        task({ taskId: 'tsk_new', updatedAt: '2026-10-24T08:00:00.000Z' }),
      ],
    });
    expect(listTasks(state).map((row) => row.taskId)).toEqual(['tsk_new', 'tsk_old']);
  });

  it('honours the limit', () => {
    const state = createDesktopState({
      tasks: [task({ taskId: 'tsk_a' }), task({ taskId: 'tsk_b' })],
    });
    expect(listTasks(state, 1)).toHaveLength(1);
  });

  it('returns the full local detail for a known task', () => {
    const state = createDesktopState({ tasks: [task({ localDiff: '--- a\n+++ b' })] });
    const detail = getTask(state, 'tsk_1');
    expect(detail?.localDiff).toBe('--- a\n+++ b');
    expect(detail?.intent).toBe('修复空值');
  });

  it('returns null for an unknown task', () => {
    expect(getTask(createDesktopState(), 'tsk_missing')).toBeNull();
  });

  it('records a cancel request without claiming a terminal state', () => {
    const state = createDesktopState({ tasks: [task()] });
    expect(cancelTask(state, 'tsk_1')).toBe(true);
    expect(getTask(state, 'tsk_1')?.state).toBe('cancel_requested');
  });

  it('reports an unknown task on cancel', () => {
    expect(cancelTask(createDesktopState(), 'tsk_missing')).toBe(false);
  });
});

describe('diagnostics', () => {
  it('reports local facts without a data directory that does not exist yet', () => {
    const diagnostics = readDiagnostics(createDesktopState({ desktopVersion: '9.9.9' }));
    expect(diagnostics.desktopVersion).toBe('9.9.9');
    expect(diagnostics.databasePath).toBeNull();
    expect(diagnostics.connected).toBe(false);
    expect(diagnostics.lastError).toBeNull();
  });
});

describe('findAuthorizedProject', () => {
  const state = () =>
    createDesktopState({
      projects: [
        {
          projectId: 'prj_1',
          displayName: 'app',
          rootPath: '/Users/demo/app',
          capabilities: ['read'],
          grantVersion: 1,
          enabled: true,
        },
      ],
    });

  it('covers the project root itself', () => {
    expect(findAuthorizedProject(state(), '/Users/demo/app')?.projectId).toBe('prj_1');
  });

  it('covers a directory inside the project', () => {
    expect(findAuthorizedProject(state(), '/Users/demo/app/src')?.projectId).toBe('prj_1');
  });

  it('does not cover a sibling that shares a name prefix', () => {
    expect(findAuthorizedProject(state(), '/Users/demo/app-other')).toBeNull();
  });

  it('does not cover a directory outside every project', () => {
    expect(findAuthorizedProject(state(), '/Users/demo/other')).toBeNull();
  });

  it('does not cover a revoked project', () => {
    const revoked = createDesktopState({
      projects: [
        {
          projectId: 'prj_1',
          displayName: 'app',
          rootPath: '/Users/demo/app',
          capabilities: ['read'],
          grantVersion: 2,
          enabled: false,
        },
      ],
    });
    expect(findAuthorizedProject(revoked, '/Users/demo/app')).toBeNull();
  });
});
