import { describe, expect, it } from 'vitest';

import {
  IPC_CHANNELS,
  IPC_METHODS,
  channelFor,
  isAllowedChannel,
  maskToken,
  parseIpcRequest,
} from './ipc.js';

describe('IPC allowlist', () => {
  it('exposes only namespaced versioned channels', () => {
    for (const channel of IPC_CHANNELS) {
      expect(channel.startsWith('linxbox.v1.')).toBe(true);
    }
    expect(IPC_CHANNELS).toHaveLength(IPC_METHODS.length);
  });

  it('accepts an allowlisted channel', () => {
    expect(isAllowedChannel('linxbox.v1.connection.read')).toBe(true);
  });

  it('rejects an unknown channel', () => {
    expect(isAllowedChannel('linxbox.v1.tasks.deleteEverything')).toBe(false);
  });

  it('rejects a channel from another namespace', () => {
    expect(isAllowedChannel('electron.readFile')).toBe(false);
  });

  it('rejects a channel without the version segment', () => {
    expect(isAllowedChannel('linxbox.connection.read')).toBe(false);
  });

  it('maps a method to its channel', () => {
    expect(channelFor('tasks.list')).toBe('linxbox.v1.tasks.list');
  });
});

describe('parseIpcRequest', () => {
  it('accepts a valid payload', () => {
    const result = parseIpcRequest('tasks.get', { taskId: 'tsk_1' });
    expect(result.ok).toBe(true);
  });

  it('rejects an unknown method', () => {
    const result = parseIpcRequest('tasks.drop', { taskId: 'tsk_1' });
    expect(result).toEqual({ ok: false, code: 'INVALID_ARGUMENT', message: '未知的桌面端调用' });
  });

  it('rejects an unknown field', () => {
    const result = parseIpcRequest('tasks.get', { taskId: 'tsk_1', force: true });
    expect(result.ok).toBe(false);
  });

  it('rejects a wrong field type', () => {
    const result = parseIpcRequest('tasks.get', { taskId: 42 });
    expect(result.ok).toBe(false);
  });

  it('rejects a missing field', () => {
    const result = parseIpcRequest('tasks.cancel', {});
    expect(result.ok).toBe(false);
  });

  it('rejects a non-object payload where an object is required', () => {
    const result = parseIpcRequest('projects.list', 'not-an-object');
    expect(result.ok).toBe(false);
  });

  it('rejects a project grant that tries to carry its own root path', () => {
    const result = parseIpcRequest('projects.grant', {
      displayName: 'evil',
      capabilities: ['read'],
      rootPath: '/Users/victim/secret',
    });
    expect(result.ok).toBe(false);
  });

  it('rejects a grant with an unknown capability', () => {
    const result = parseIpcRequest('projects.grant', {
      displayName: 'evil',
      capabilities: ['deploy'],
    });
    expect(result.ok).toBe(false);
  });

  it('rejects a grant without capabilities', () => {
    const result = parseIpcRequest('projects.grant', { displayName: 'empty', capabilities: [] });
    expect(result.ok).toBe(false);
  });

  it('rejects a connection save without a token', () => {
    const result = parseIpcRequest('connection.save', {
      gatewayUrl: 'wss://example.com/ws',
      computerName: 'mac',
    });
    expect(result.ok).toBe(false);
  });

  it('rejects a connection save with a non-url gateway', () => {
    const result = parseIpcRequest('connection.save', {
      gatewayUrl: 'not-a-url',
      accessToken: 'token',
      computerName: 'mac',
    });
    expect(result.ok).toBe(false);
  });

  it('accepts an empty payload for a read-only method', () => {
    expect(parseIpcRequest('connection.read', {}).ok).toBe(true);
    expect(parseIpcRequest('diagnostics.read', {}).ok).toBe(true);
  });

  it('rejects an empty payload for a method that takes arguments', () => {
    expect(parseIpcRequest('tasks.get', {}).ok).toBe(false);
  });

  it('accepts an optional limit on the task list', () => {
    expect(parseIpcRequest('tasks.list', {}).ok).toBe(true);
    expect(parseIpcRequest('tasks.list', { limit: 10 }).ok).toBe(true);
  });

  it('rejects an out-of-range limit', () => {
    expect(parseIpcRequest('tasks.list', { limit: 0 }).ok).toBe(false);
    expect(parseIpcRequest('tasks.list', { limit: 9999 }).ok).toBe(false);
  });
});

describe('maskToken', () => {
  it('keeps only the last four characters', () => {
    expect(maskToken('abcdefghijkl')).toBe('••••••••ijkl');
  });

  it('fully masks a short token', () => {
    expect(maskToken('abcd')).toBe('••••');
  });

  it('fully masks an empty token', () => {
    expect(maskToken('')).toBe('');
  });
});
