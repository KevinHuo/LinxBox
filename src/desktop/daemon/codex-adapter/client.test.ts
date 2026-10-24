import { describe, expect, it } from 'vitest';

import { CodexClient } from './client.js';
import { createFakeProcess } from './fake-process.test-support.js';

function createClient() {
  const harness = createFakeProcess();
  const client = new CodexClient({
    binaryPath: '/opt/codex',
    cwd: '/tmp/work',
    spawnFn: harness.spawnFn,
    requestTimeoutMs: 500,
    turnTimeoutMs: 2000,
  });
  return { client, child: harness.child };
}

const initializeResult = {
  userAgent: 'linxbox_desktop/0.164.0 (macos)',
  codexHome: '/Users/demo/.codex',
  platformFamily: 'unix',
  platformOs: 'macos',
};

describe('CodexClient', () => {
  it('refuses to work before the handshake', async () => {
    const { client } = createClient();
    await expect(client.startThread('/tmp/work')).rejects.toMatchObject({
      code: 'CODEX_NOT_INITIALIZED',
    });
    await expect(client.startTurn('th_1', 'hi', 'm1')).rejects.toMatchObject({
      code: 'CODEX_NOT_INITIALIZED',
    });
    await expect(client.interruptTurn('th_1', 'tn_1')).rejects.toMatchObject({
      code: 'CODEX_NOT_INITIALIZED',
    });
    client.close();
  });

  it('performs the handshake and reports the provider', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    const result = await initializing;

    expect(result.userAgent).toContain('linxbox_desktop');
    expect(result.platformOs).toBe('macos');

    const sent = child.sentMessages();
    expect(sent[0]?.['method']).toBe('initialize');
    expect(sent[1]?.['method']).toBe('initialized');
    client.close();
  });

  it('rejects a handshake result it cannot understand', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: { unexpected: true } });
    await expect(initializing).rejects.toMatchObject({ code: 'CODEX_PROTOCOL' });
    client.close();
  });

  it('reads the provider default model', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const reading = client.readDefaultModel('/tmp/work');
    child.emitMessage({ id: 2, result: { config: { model: 'gpt-x', model_provider: null } } });
    await expect(reading).resolves.toBe('gpt-x');
    client.close();
  });

  it('returns null when the provider has no model configured', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const reading = client.readDefaultModel('/tmp/work');
    child.emitMessage({ id: 2, result: { config: { model: null } } });
    await expect(reading).resolves.toBeNull();
    client.close();
  });

  it('starts a thread and a turn and tracks the turn state', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const startingThread = client.startThread('/tmp/work');
    child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await expect(startingThread).resolves.toBe('th_1');

    const startingTurn = client.startTurn('th_1', '做点事', 'm1');
    child.emitMessage({ id: 3, result: { turn: { id: 'tn_1', status: 'inProgress' } } });
    await expect(startingTurn).resolves.toBe('tn_1');

    expect(client.turnState.status).toBe('running');
    expect(client.turnState.turnId).toBe('tn_1');
    client.close();
  });

  it('resumes a thread', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const resuming = client.resumeThread('th_1', '/tmp/work');
    child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await expect(resuming).resolves.toBe('th_1');
    client.close();
  });

  it('turns provider notifications into observable turn state', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const startingThread = client.startThread('/tmp/work');
    child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await startingThread;

    const startingTurn = client.startTurn('th_1', '做点事', 'm1');
    child.emitMessage({ id: 3, result: { turn: { id: 'tn_1', status: 'inProgress' } } });
    await startingTurn;

    child.emitMessage({
      jsonrpc: '2.0',
      method: 'item/completed',
      params: { item: { type: 'agentMessage', phase: 'commentary', text: '在看代码' } },
    });
    expect(client.turnState.progress).toBe('在看代码');

    child.emitMessage({
      jsonrpc: '2.0',
      method: 'item/completed',
      params: { item: { type: 'agentMessage', phase: 'final_answer', text: '答案是 42' } },
    });
    expect(client.turnState.finalText).toBe('答案是 42');

    child.emitMessage({
      jsonrpc: '2.0',
      method: 'account/updated',
      params: { authMode: 'chatgpt', planType: 'plus' },
    });
    expect(client.accountState.authMode).toBe('chatgpt');
    client.close();
  });

  it('resolves waitForTerminal when the provider reports a terminal state', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const startingThread = client.startThread('/tmp/work');
    child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await startingThread;

    const startingTurn = client.startTurn('th_1', '做点事', 'm1');
    child.emitMessage({ id: 3, result: { turn: { id: 'tn_1', status: 'inProgress' } } });
    const turnId = await startingTurn;

    const waiting = client.waitForTerminal(turnId, 2000);
    child.emitMessage({
      jsonrpc: '2.0',
      method: 'turn/completed',
      params: { turnId, turn: { id: turnId, status: 'completed' } },
    });

    const state = await waiting;
    expect(state.status).toBe('completed');
    client.close();
  });

  it('converges an unprovable turn to interrupted', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const startingThread = client.startThread('/tmp/work');
    child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await startingThread;

    const startingTurn = client.startTurn('th_1', '做点事', 'm1');
    child.emitMessage({ id: 3, result: { turn: { id: 'tn_1', status: 'inProgress' } } });
    await startingTurn;

    child.emit('exit', 1, null);
    await new Promise((resolve) => setTimeout(resolve, 10));

    expect(client.turnState.status).toBe('interrupted');
  });
});

describe('CodexClient waitForTerminal timeout', () => {
  it('converges to interrupted when the provider never finishes', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const startingThread = client.startThread('/tmp/work');
    child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await startingThread;

    const startingTurn = client.startTurn('th_1', '做点事', 'm1');
    child.emitMessage({ id: 3, result: { turn: { id: 'tn_1', status: 'inProgress' } } });
    const turnId = await startingTurn;

    const state = await client.waitForTerminal(turnId, 30);
    expect(state.status).toBe('interrupted');
    expect(state.error).toContain('未在超时时间内返回终态');
    client.close();
  });
});

describe('CodexClient lifecycle', () => {
  it('refuses calls after the client is closed', async () => {
    const { client } = createClient();
    client.close();
    await expect(client.initialize()).rejects.toMatchObject({ code: 'CODEX_DISCONNECTED' });
  });

  it('converges to interrupted when the provider process reports an error', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const startingThread = client.startThread('/tmp/work');
    child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await startingThread;

    const startingTurn = client.startTurn('th_1', 'x', 'm1');
    child.emitMessage({ id: 3, result: { turn: { id: 'tn_1', status: 'inProgress' } } });
    await startingTurn;

    child.emit('error', new Error('provider died'));
    await new Promise((resolve) => setTimeout(resolve, 10));

    expect(client.turnState.status).toBe('interrupted');
  });

  it('reports a turn result it cannot understand', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const startingThread = client.startThread('/tmp/work');
    child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await startingThread;

    const startingTurn = client.startTurn('th_1', 'x', 'm1');
    child.emitMessage({ id: 3, result: { turn: { id: 'tn_1' } } });

    await expect(startingTurn).rejects.toMatchObject({ code: 'CODEX_PROTOCOL' });
    client.close();
  });
});

describe('CodexClient session history', () => {
  it('lists the sessions the provider knows about', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const listing = client.listSessions(10);
    child.emitMessage({
      id: 2,
      result: {
        data: [
          {
            id: 'th_1',
            preview: '会话标题',
            cwd: '/tmp/work',
            model: 'gpt-x',
            createdAt: 1792840394,
            updatedAt: 1792840398,
            status: { type: 'notLoaded' },
          },
        ],
        nextCursor: null,
      },
    });

    const sessions = await listing;
    expect(sessions).toHaveLength(1);
    expect(sessions[0]?.title).toBe('会话标题');
    expect(sessions[0]?.threadId).toBe('th_1');
    client.close();
  });

  it('reads the transcript of one session', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const reading = client.readSession('th_1');
    child.emitMessage({
      id: 2,
      result: {
        thread: {
          id: 'th_1',
          preview: '会话标题',
          turns: [
            {
              id: 'tn_1',
              status: 'completed',
              items: [
                {
                  type: 'userMessage',
                  content: [{ type: 'text', text: '你好' }],
                },
                { type: 'agentMessage', phase: 'final_answer', text: '在的' },
              ],
            },
          ],
        },
      },
    });

    const session = await reading;
    expect(session?.threadId).toBe('th_1');
    expect(session?.turns[0]?.messages).toHaveLength(2);
    client.close();
  });

  it('returns null for a session that cannot be read', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const reading = client.readSession('th_missing');
    child.emitMessage({ id: 2, result: { thread: {} } });

    await expect(reading).resolves.toBeNull();
    client.close();
  });
});
