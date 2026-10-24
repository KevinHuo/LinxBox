import { describe, expect, it } from 'vitest';

import { CodexClient, defaultCodexEnv } from './client.js';
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

  it('returns null when the provider config result is malformed', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const reading = client.readDefaultModel('/tmp/work');
    child.emitMessage({ id: 2, result: { unexpected: true } });
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

  it('rejects malformed thread start and resume results', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const starting = client.startThread('/tmp/work');
    child.emitMessage({ id: 2, result: {} });
    await expect(starting).rejects.toMatchObject({ code: 'CODEX_PROTOCOL' });

    const resuming = client.resumeThread('th_1', '/tmp/work');
    child.emitMessage({ id: 3, result: {} });
    await expect(resuming).rejects.toMatchObject({ code: 'CODEX_PROTOCOL' });
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
    child.emitMessage({
      jsonrpc: '2.0',
      method: 'item/completed',
      params: { item: { type: 'agentMessage', phase: 'final_answer', text: '补充答案' } },
    });
    expect(client.turnState.finalText).toBe('答案是 42\n\n补充答案');

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
      method: 'item/completed',
      params: {
        threadId: 'th_1',
        turnId,
        item: { type: 'agentMessage', phase: 'commentary', text: '仍在运行' },
      },
    });
    child.emitMessage({
      jsonrpc: '2.0',
      method: 'turn/completed',
      params: {
        threadId: 'th_1',
        turnId: 'tn_other',
        turn: { id: 'tn_other', status: 'completed' },
      },
    });
    child.emitMessage({
      jsonrpc: '2.0',
      method: 'turn/completed',
      params: { threadId: 'th_1', turnId, turn: { id: turnId, status: 'completed' } },
    });

    const state = await waiting;
    expect(state.status).toBe('completed');
    await expect(client.waitForTerminal(turnId)).resolves.toBe(state);
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

    const events: unknown[] = [];
    client.on((event) => events.push(event));
    child.emit('exit', 1, null);
    await new Promise((resolve) => setTimeout(resolve, 10));

    expect(client.turnState.status).toBe('interrupted');
    expect(events).toContainEqual({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'interrupted',
      error: '本机 Codex 进程已退出，无法确认任务结果',
    });
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

  it('ignores unknown notifications and process exits while idle', async () => {
    const { client, child } = createClient();
    const events: unknown[] = [];
    client.on((event) => events.push(event));
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    child.emitMessage({ jsonrpc: '2.0', method: 'unknown/event', params: {} });
    child.emit('exit', 0, null);
    await new Promise((resolve) => setTimeout(resolve, 10));

    expect(events).toEqual([]);
    expect(client.turnState.status).toBe('idle');
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

describe('CodexClient projects and conversations', () => {
  it('lists the provider projects', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const listing = client.listProjects();
    child.emitMessage({
      id: 2,
      result: {
        data: [{ id: 'p1', name: 'app', roots: [{ path: '/tmp/app' }], position: 0 }],
      },
    });

    const projects = await listing;
    expect(projects).toHaveLength(1);
    expect(projects[0]?.roots).toEqual(['/tmp/app']);
    client.close();
  });

  it('reads a conversation with every detail entry', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const reading = client.readConversation('th_1');
    await new Promise((resolve) => setTimeout(resolve, 10));
    child.emitMessage({
      id: 2,
      result: { thread: { id: 'th_1', preview: '标题', cwd: '/tmp/app' } },
    });
    await new Promise((resolve) => setTimeout(resolve, 10));
    child.emitMessage({
      id: 3,
      result: {
        data: [
          {
            id: 'tn_1',
            status: 'completed',
            startedAt: 1792840394,
            items: [
              { type: 'userMessage', content: [{ type: 'text', text: '你好' }] },
              { type: 'commandExecution', command: 'ls', status: 'completed' },
            ],
          },
        ],
      },
    });

    const conversation = await reading;
    expect(conversation?.title).toBe('标题');
    expect(conversation?.cwd).toBe('/tmp/app');
    expect(conversation?.turns[0]?.details.map((detail) => detail.kind)).toEqual([
      'user',
      'command',
    ]);
    client.close();
  });

  it('requests the newest conversation page and displays it chronologically', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const reading = client.readConversation('th_long', 2);
    await new Promise((resolve) => setTimeout(resolve, 10));
    child.emitMessage({
      id: 2,
      result: { thread: { id: 'th_long', preview: '长对话', cwd: '/tmp/app' } },
    });
    await new Promise((resolve) => setTimeout(resolve, 10));
    const turnRequest = child
      .sentMessages()
      .find((message) => message['method'] === 'thread/turns/list');
    expect(turnRequest?.['params']).toMatchObject({
      threadId: 'th_long',
      limit: 2,
      sortDirection: 'desc',
      itemsView: 'full',
    });
    child.emitMessage({
      id: 3,
      result: {
        data: [
          { id: 'tn_100', status: 'completed', startedAt: 100, items: [] },
          { id: 'tn_99', status: 'completed', startedAt: 99, items: [] },
        ],
        nextCursor: 'older',
      },
    });

    const conversation = await reading;
    expect(conversation?.turns.map((turn) => turn.turnId)).toEqual(['tn_99', 'tn_100']);
    client.close();
  });

  it('reads an empty conversation', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const reading = client.readConversation('th_1');
    await new Promise((resolve) => setTimeout(resolve, 10));
    child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await new Promise((resolve) => setTimeout(resolve, 10));
    child.emitMessage({ id: 3, result: { data: [] } });

    const conversation = await reading;
    expect(conversation?.turns).toEqual([]);
    client.close();
  });

  it('returns null when conversation metadata is missing', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const reading = client.readConversation('th_missing');
    child.emitMessage({ id: 2, result: { thread: {} } });

    await expect(reading).resolves.toBeNull();
    client.close();
  });

  it('normalizes a malformed newest-page turn without inventing metadata', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const reading = client.readConversation('th_1');
    await new Promise((resolve) => setTimeout(resolve, 10));
    child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await new Promise((resolve) => setTimeout(resolve, 10));
    child.emitMessage({ id: 3, result: { data: [{}] } });

    const conversation = await reading;
    expect(conversation?.turns).toEqual([
      { turnId: '', status: 'unknown', startedAt: null, details: [] },
    ]);
    client.close();
  });
});

describe('CodexClient interrupt and reuse', () => {
  it('interrupts a running turn', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const interrupting = client.interruptTurn('th_1', 'tn_1');
    child.emitMessage({ id: 2, result: {} });
    await expect(interrupting).resolves.toBeUndefined();
    client.close();
  });

  it('reuses the same provider process for a second operation', async () => {
    const { client, child } = createClient();
    const initializing = client.initialize();
    child.emitMessage({ id: 1, result: initializeResult });
    await initializing;

    const listing = client.listProjects();
    child.emitMessage({ id: 2, result: { data: [] } });
    await listing;

    const second = client.listProjects();
    child.emitMessage({ id: 3, result: { data: [] } });
    await second;

    const spawns = child.sentMessages().filter((message) => message['method'] === 'initialize');
    expect(spawns).toHaveLength(1);
    client.close();
  });
});

describe('defaultCodexEnv', () => {
  it('filters both an explicit environment and the process environment', () => {
    expect(defaultCodexEnv({ PATH: '/bin', SECRET_VALUE: 'hidden' })).toEqual({ PATH: '/bin' });
    expect(defaultCodexEnv()).not.toHaveProperty('SECRET_VALUE');
  });
});
