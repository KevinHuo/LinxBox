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

    await api.probeCodex();
    await api.listCodexSessions();
    await api.listCodexProjects();
    await api.readCodexConversation({ threadId: 'th_1' });
    await api.continueCodexSession({ threadId: 'th_1', prompt: '继续' });
    await api.authorizeCodexSession({ threadId: 'th_1' });
    await api.readCodexSession({ threadId: 'th_1' });
    await api.startCodexTurn({ prompt: 'hi', projectId: 'prj_1' });
    await api.readCodexTurn();
    await api.interruptCodexTurn({ turnId: 'turn_1' });
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
      'linxbox.v1.codex.probe',
      'linxbox.v1.codex.sessions.list',
      'linxbox.v1.codex.projects.list',
      'linxbox.v1.codex.conversation.read',
      'linxbox.v1.codex.session.continue',
      'linxbox.v1.codex.session.authorize',
      'linxbox.v1.codex.sessions.read',
      'linxbox.v1.codex.turn.start',
      'linxbox.v1.codex.turn.status',
      'linxbox.v1.codex.turn.interrupt',
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
        'authorizeCodexSession',
        'cancelTask',
        'continueCodexSession',
        'getTask',
        'interruptCodexTurn',
        'listCodexProjects',
        'listCodexSessions',
        'readCodexConversation',
        'probeCodex',
        'readCodexSession',
        'readCodexTurn',
        'grantProject',
        'listProjects',
        'listTasks',
        'readConnection',
        'startCodexTurn',
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
