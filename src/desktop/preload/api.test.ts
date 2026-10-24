import { describe, expect, it } from 'vitest';

import { IPC_METHODS } from '../shared/ipc.js';
import { BRIDGE_NAMESPACE, createLinxboxApi } from './api.js';

describe('preload bridge', () => {
  it('maps every method to exactly one allowlisted channel', async () => {
    const calls: { channel: string; payload: unknown }[] = [];
    const api = createLinxboxApi((channel, payload) => {
      calls.push({ channel, payload });
      return Promise.resolve(null);
    });

    await api.readConnection();
    await api.saveConnection({ gatewayUrl: 'wss://example.com/ws' });
    await api.testConnection();
    await api.listProjects();
    await api.grantProject({ displayName: 'x', capabilities: ['read'] });
    await api.revokeProject({ projectId: 'prj_1' });
    await api.listTasks();
    await api.getTask({ taskId: 'tsk_1' });
    await api.cancelTask({ taskId: 'tsk_1' });
    await api.readDiagnostics();

    expect(calls.map((call) => call.channel)).toEqual([
      'linxbox.v1.connection.read',
      'linxbox.v1.connection.save',
      'linxbox.v1.connection.test',
      'linxbox.v1.projects.list',
      'linxbox.v1.projects.grant',
      'linxbox.v1.projects.revoke',
      'linxbox.v1.tasks.list',
      'linxbox.v1.tasks.get',
      'linxbox.v1.tasks.cancel',
      'linxbox.v1.diagnostics.read',
    ]);
  });

  it('exposes exactly the allowlisted methods and no generic invoke', () => {
    const api = createLinxboxApi(() => Promise.resolve(null));
    expect(Object.keys(api).sort()).toEqual(
      [
        'cancelTask',
        'getTask',
        'grantProject',
        'listProjects',
        'listTasks',
        'readConnection',
        'readDiagnostics',
        'revokeProject',
        'saveConnection',
        'testConnection',
      ].sort(),
    );
    expect('invoke' in api).toBe(false);
    expect('ipcRenderer' in api).toBe(false);
  });

  it('covers every allowlisted method', () => {
    const api = createLinxboxApi(() => Promise.resolve(null));
    expect(Object.keys(api)).toHaveLength(IPC_METHODS.length);
  });

  it('sends an empty payload for read-only methods', async () => {
    const calls: { channel: string; payload: unknown }[] = [];
    const api = createLinxboxApi((channel, payload) => {
      calls.push({ channel, payload });
      return Promise.resolve(null);
    });

    await api.listTasks();
    expect(calls[0]?.payload).toEqual({});
  });

  it('uses a namespaced bridge name', () => {
    expect(BRIDGE_NAMESPACE).toBe('linxbox');
  });
});
