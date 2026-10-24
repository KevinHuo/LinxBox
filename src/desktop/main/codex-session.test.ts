import { describe, expect, it } from 'vitest';

import { CodexSession } from './codex-session.js';
import { createFakeProcess } from '../daemon/codex-adapter/fake-process.test-support.js';

const initializeResult = {
  userAgent: 'linxbox_desktop/0.164.0 (macos)',
  codexHome: '/Users/demo/.codex',
  platformFamily: 'unix',
  platformOs: 'macos',
};

describe('CodexSession', () => {
  it('reports that no provider is installed', async () => {
    const session = new CodexSession({ binary: null });
    const probe = await session.probe();
    expect(probe.available).toBe(false);
    expect(probe.error).toContain('未找到本机 Codex');
  });

  it('reports the provider facts after the handshake', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: 'codex-cli 0.164.0' },
      spawnFn: harness.spawnFn,
    });

    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    harness.child.emitMessage({
      jsonrpc: '2.0',
      method: 'account/updated',
      params: { authMode: 'chatgpt', planType: 'plus' },
    });

    const probe = await probing;
    expect(probe.available).toBe(true);
    expect(probe.binaryPath).toBe('/opt/codex');
    expect(probe.version).toBe('codex-cli 0.164.0');
    expect(probe.userAgent).toContain('linxbox_desktop');
    session.close();
  });

  it('starts idle', () => {
    const session = new CodexSession({ binary: null });
    expect(session.status().status).toBe('idle');
    expect(session.status().finalText).toBe('');
  });

  it('forwards provider events to session subscribers', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const seen: string[] = [];
    session.on((event) => seen.push(event.type));

    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    harness.child.emitMessage({
      jsonrpc: '2.0',
      method: 'item/completed',
      params: { item: { type: 'agentMessage', phase: 'final_answer', text: 'hi' } },
    });

    expect(seen).toContain('final');
    session.close();
  });

  it('ignores an interrupt when no turn is running', async () => {
    const session = new CodexSession({ binary: null });
    await expect(session.interrupt('tn_1')).resolves.toBeUndefined();
  });
});

describe('CodexSession failure paths', () => {
  it('reports a provider that refuses to start', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });

    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, error: { code: -32000, message: 'boom' } });

    const probe = await probing;
    expect(probe.available).toBe(false);
    // The provider's raw reason stays in the local stderr buffer; the surfaced
    // message is generic so it can never leak provider internals.
    expect(probe.error).toContain('本机 Codex');
    session.close();
  });

  it('reports a provider whose handshake result is unusable', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });

    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: { unexpected: true } });

    const probe = await probing;
    expect(probe.available).toBe(false);
    session.close();
  });
});

describe('CodexSession interrupt', () => {
  it('asks the provider to interrupt a running turn', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const starting = session.startTurn('x', '/tmp/work');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 3, result: { turn: { id: 'tn_1', status: 'inProgress' } } });
    await starting;

    const interrupting = session.interrupt('tn_1');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 4, result: {} });

    await expect(interrupting).resolves.toBeUndefined();
    session.close();
  });
});

describe('CodexSession session browsing', () => {
  it('lists the sessions the provider knows about', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const listing = session.listSessions(5);
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({
      id: 2,
      result: { data: [{ id: 'th_1', preview: '标题', cwd: '/tmp/work', model: 'gpt-x' }] },
    });

    const sessions = await listing;
    expect(sessions).toHaveLength(1);
    expect(sessions[0]?.title).toBe('标题');
    session.close();
  });

  it('reads a session transcript', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const reading = session.readSession('th_1');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({
      id: 2,
      result: {
        thread: {
          id: 'th_1',
          preview: '标题',
          turns: [
            {
              id: 'tn_1',
              status: 'completed',
              items: [{ type: 'userMessage', content: [{ type: 'text', text: '你好' }] }],
            },
          ],
        },
      },
    });

    const detail = await reading;
    expect(detail?.turns[0]?.messages[0]?.text).toBe('你好');
    session.close();
  });

  it('returns null for a session that cannot be read', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const reading = session.readSession('th_missing');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 2, result: { thread: {} } });

    await expect(reading).resolves.toBeNull();
    session.close();
  });
});

describe('CodexSession continuing a conversation', () => {
  it('resumes the thread and starts a new turn in it', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const continuing = session.continueSession('th_1', '继续说');
    await new Promise((resolve) => setTimeout(resolve, 10));
    // 先是 thread/read，再是 thread/resume，最后是 turn/start
    harness.child.emitMessage({ id: 2, result: { thread: { id: 'th_1', cwd: '/tmp/work' } } });
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 3, result: { thread: { id: 'th_1' } } });
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 4, result: { turn: { id: 'tn_9', status: 'inProgress' } } });

    const turn = await continuing;
    expect(turn.turnId).toBe('tn_9');
    expect(turn.status).toBe('running');

    const methods = harness.child.sentMessages().map((message) => message['method']);
    expect(methods).toContain('thread/read');
    expect(methods).toContain('thread/resume');
    expect(methods).toContain('turn/start');
    session.close();
  });

  it('refuses to continue a session that does not exist', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const continuing = session.continueSession('th_missing', '继续说');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 2, result: { thread: {} } });

    await expect(continuing).rejects.toThrow('会话不存在');
    session.close();
  });

  it('refuses to continue a session without a working directory', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const continuing = session.continueSession('th_1', '继续说');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });

    await expect(continuing).rejects.toThrow('没有工作目录');
    session.close();
  });

  it('reports the working directory a session belongs to', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const reading = session.sessionCwd('th_1');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 2, result: { thread: { id: 'th_1', cwd: '/tmp/work' } } });

    await expect(reading).resolves.toBe('/tmp/work');
    session.close();
  });

  it('reports no working directory for a session without one', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const reading = session.sessionCwd('th_1');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 2, result: { thread: {} } });

    await expect(reading).resolves.toBeNull();
    session.close();
  });
});

describe('CodexSession projects and conversations', () => {
  it('lists the provider projects', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const listing = session.listProjects();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({
      id: 2,
      result: { data: [{ id: 'p1', name: 'app', roots: [{ path: '/tmp/app' }], position: 0 }] },
    });

    const projects = await listing;
    expect(projects[0]?.name).toBe('app');
    expect(projects[0]?.roots).toEqual(['/tmp/app']);
    session.close();
  });

  it('reads a conversation with its details', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const reading = session.readConversation('th_1');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 2, result: { thread: { id: 'th_1', preview: '标题' } } });
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({
      id: 3,
      result: {
        data: [
          {
            id: 'tn_1',
            status: 'completed',
            items: [{ type: 'userMessage', content: [{ type: 'text', text: '你好' }] }],
          },
        ],
      },
    });

    const conversation = await reading;
    expect(conversation?.turns[0]?.details[0]?.body).toBe('你好');
    session.close();
  });

  it('reports a conversation that cannot be read', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const reading = session.readConversation('th_1');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 2, result: { thread: {} } });
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 3, result: { data: [] } });

    await expect(reading).resolves.toBeNull();
    session.close();
  });
});

describe('CodexSession connection lifecycle', () => {
  it('fails when no provider binary exists', async () => {
    const session = new CodexSession({ binary: null });
    await expect(session.listProjects()).rejects.toThrow('未找到本机 Codex');
    session.close();
  });

  it('reuses the same provider process across operations', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const listing = session.listProjects();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 2, result: { data: [] } });
    await listing;

    const initializes = harness.child
      .sentMessages()
      .filter((message) => message['method'] === 'initialize');
    expect(initializes).toHaveLength(1);
    session.close();
  });
});
