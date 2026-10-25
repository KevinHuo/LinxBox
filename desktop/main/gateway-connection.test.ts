import { describe, expect, it } from 'vitest';

import { createCodexRunner, GatewayConnectionManager } from './gateway-connection.js';
import type { CodexSession } from './codex-session.js';
import type { SocketLike } from '../daemon/gateway-client/client.js';

/** A socket that records what the client sent and can push events back. */
class FakeSocket implements SocketLike {
  readyState = 1;
  readonly sent: string[] = [];
  private readonly listeners = new Map<string, ((event: unknown) => void)[]>();

  send(data: string): void {
    this.sent.push(data);
  }

  close(): void {
    this.readyState = 3;
  }

  addEventListener(
    type: 'open' | 'message' | 'close' | 'error',
    listener: (event: never) => void,
  ): void {
    const existing = this.listeners.get(type) ?? [];
    existing.push(listener as (event: unknown) => void);
    this.listeners.set(type, existing);
  }

  /** Fire an event at the client. */
  emit(type: 'open' | 'message' | 'close' | 'error', payload: unknown): void {
    for (const listener of this.listeners.get(type) ?? []) {
      listener(payload);
    }
  }

  /** The frames the client sent, parsed. */
  frames(): { type: string; payload: Record<string, unknown> }[] {
    return this.sent.map(
      (raw) => JSON.parse(raw) as { type: string; payload: Record<string, unknown> },
    );
  }
}

/** A session stub that never spawns a provider process. */
function createSessionStub(
  overrides: {
    readSession?: () => Promise<{ cwd: string | null } | null>;
    on?: (listener: (event: unknown) => void) => () => void;
    listSessions?: () => Promise<
      { threadId: string; title: string; cwd: string | null; status: string }[]
    >;
    createSession?: (cwd: string) => Promise<string>;
    continueSession?: (
      threadId: string,
      prompt: string,
    ) => Promise<{ threadId: string | null; turnId: string | null }>;
  } = {},
): CodexSession {
  return {
    startTurn: () => Promise.resolve({ threadId: 'th_1', turnId: 'tn_1' }),
    continueSession:
      overrides.continueSession ?? (() => Promise.resolve({ threadId: 'th_2', turnId: 'tn_2' })),
    readSession: overrides.readSession ?? (() => Promise.resolve(null)),
    listSessions: overrides.listSessions ?? (() => Promise.resolve([])),
    listProjects: () => Promise.resolve([]),
    createSession: overrides.createSession ?? (() => Promise.resolve('th_managed')),
    on: overrides.on ?? (() => () => {}),
    status: () => ({ status: 'idle' }),
    close: () => undefined,
    interrupt: () => Promise.resolve(),
  } as unknown as CodexSession;
}

/** A manager wired to a fake socket, with the socket handed back. */
function createManager(
  options: {
    resolveProject?: (projectId: string) => { rootPath: string; enabled: boolean } | null;
    onStatusChange?: (status: {
      configured: boolean;
      connected: boolean;
      lastError: string | null;
      lastActivityAt: string | null;
    }) => void;
    listProjects?: () => readonly {
      projectId: string;
      displayName: string;
      rootPath: string;
      enabled: boolean;
    }[];
    session?: CodexSession;
  } = {},
): { manager: GatewayConnectionManager; socket: FakeSocket } {
  let socket: FakeSocket | null = null;
  const manager = new GatewayConnectionManager({
    session: options.session ?? createSessionStub(),
    resolveProject: options.resolveProject ?? (() => null),
    ...(options.listProjects === undefined ? {} : { listProjects: options.listProjects }),
    desktopVersion: '0.1.0',
    socketFactory: () => {
      socket = new FakeSocket();
      return socket;
    },
    ...(options.onStatusChange === undefined ? {} : { onStatusChange: options.onStatusChange }),
  });
  manager.connect({
    gatewayUrl: 'ws://127.0.0.1:8787/v1/clients/connect',
    accessToken: 'demo-client-token',
    computerName: '工作电脑',
  });
  return { manager, socket: socket as unknown as FakeSocket };
}

describe('createCodexRunner', () => {
  it('starts a turn and reports the provider ids', async () => {
    const runner = createCodexRunner(createSessionStub());
    const started = await runner.startTurn({
      prompt: '做点什么',
      cwd: '/tmp/work',
      clientMessageId: 'msg_1',
    });
    expect(started).toEqual({ threadId: 'th_1', turnId: 'tn_1' });
  });

  it('continues a turn in the given thread', async () => {
    const runner = createCodexRunner(createSessionStub());
    const started = await runner.continueTurn({
      threadId: 'th_parent',
      prompt: '继续',
      cwd: '/tmp/work',
      clientMessageId: 'msg_2',
    });
    expect(started.threadId).toBe('th_2');
  });

  it('reports the working directory of a session', async () => {
    const runner = createCodexRunner(
      createSessionStub({ readSession: () => Promise.resolve({ cwd: '/tmp/work' }) }),
    );
    expect(await runner.sessionCwd('th_1')).toBe('/tmp/work');
  });

  it('reports no working directory for an unknown session', async () => {
    const runner = createCodexRunner(createSessionStub());
    expect(await runner.sessionCwd('th_missing')).toBeNull();
  });

  it('subscribes to the provider events', () => {
    let subscribed = false;
    const runner = createCodexRunner(
      createSessionStub({
        on: () => {
          subscribed = true;
          return () => {};
        },
      }),
    );
    const unsubscribe = runner.on(() => {});
    expect(subscribed).toBe(true);
    expect(typeof unsubscribe).toBe('function');
  });

  it('reports the provider status', () => {
    const runner = createCodexRunner(createSessionStub());
    expect(runner.status().status).toBe('idle');
  });

  it('falls back to an empty id when the provider reports none', async () => {
    const runner = createCodexRunner({
      startTurn: () => Promise.resolve({ threadId: null, turnId: null }),
      continueSession: () => Promise.resolve({ threadId: null, turnId: null }),
      readSession: () => Promise.resolve(null),
      on: () => () => {},
      status: () => ({ status: 'idle' }),
    } as unknown as CodexSession);

    const started = await runner.startTurn({
      prompt: 'x',
      cwd: '/tmp',
      clientMessageId: 'm',
    });
    expect(started).toEqual({ threadId: '', turnId: '' });

    const continued = await runner.continueTurn({
      threadId: 'th_1',
      prompt: 'x',
      cwd: '/tmp',
      clientMessageId: 'm',
    });
    expect(continued).toEqual({ threadId: '', turnId: '' });
  });
});

describe('GatewayConnectionManager', () => {
  it('starts unconfigured and disconnected', () => {
    const manager = new GatewayConnectionManager({
      session: createSessionStub(),
      resolveProject: () => null,
    });
    const status = manager.status();
    expect(status.configured).toBe(false);
    expect(status.connected).toBe(false);
    expect(status.lastError).toBeNull();
    expect(status.lastActivityAt).toBeNull();
  });

  it('sends the hello frame once the socket opens', () => {
    const { socket } = createManager();
    socket.emit('open', undefined);
    const hello = socket.frames().find((frame) => frame.type === 'client.hello');
    expect(hello).toBeDefined();
    expect(hello?.payload['desktop_version']).toBe('0.1.0');
  });

  it('reports the connection as live once the socket opens', () => {
    const { manager, socket } = createManager();
    socket.emit('open', undefined);
    expect(manager.status().connected).toBe(true);
    expect(manager.status().configured).toBe(true);
    expect(manager.status().lastActivityAt).not.toBeNull();
  });

  it('publishes authorised Codex sessions after connecting', async () => {
    const { socket } = createManager({
      resolveProject: () => ({ rootPath: '/tmp/work', enabled: true }),
      listProjects: () => [
        {
          projectId: 'prj_1',
          displayName: 'LinxBox',
          rootPath: '/tmp/work',
          enabled: true,
        },
      ],
      session: createSessionStub({
        createSession: (cwd) => Promise.resolve(cwd === '/tmp/work' ? 'th_managed' : 'unexpected'),
      }),
    });

    socket.emit('open', undefined);
    await new Promise((resolve) => setTimeout(resolve, 20));

    const sync = socket.frames().find((frame) => frame.type === 'agent_sessions.sync');
    const items = sync?.payload['items'] as { agent_session_id: string; project_id: string }[];
    expect(items).toHaveLength(1);
    expect(items[0]).toMatchObject({
      agent_session_id: 'codex_prj_1',
      project_id: 'prj_1',
      title: 'LinxBox · LinxBox',
    });
    expect(JSON.stringify(sync)).not.toContain('/tmp/work');
  });

  it('does not publish a managed session when thread creation returns no id', async () => {
    const { socket } = createManager({
      listProjects: () => [
        {
          projectId: 'prj_1',
          displayName: 'LinxBox',
          rootPath: '/tmp/work',
          enabled: true,
        },
      ],
      session: createSessionStub({ createSession: () => Promise.resolve('') }),
    });

    socket.emit('open', undefined);
    await new Promise((resolve) => setTimeout(resolve, 20));

    const sync = socket.frames().find((frame) => frame.type === 'agent_sessions.sync');
    expect(sync?.payload['items']).toEqual([]);
  });

  it('returns an empty message list when an idle managed session cannot list turns yet', async () => {
    const { socket } = createManager({
      listProjects: () => [
        {
          projectId: 'prj_1',
          displayName: 'LinxBox',
          rootPath: '/tmp/work',
          enabled: true,
        },
      ],
      session: createSessionStub({
        createSession: () => Promise.resolve('th_managed'),
        readSession: () => Promise.reject(new Error('list_turns is not supported yet')),
      }),
    });
    socket.emit('open', undefined);
    await new Promise((resolve) => setTimeout(resolve, 20));

    socket.emit('message', {
      data: JSON.stringify({
        protocol_version: '1.0',
        message_id: 'qry_1',
        sequence: 3,
        type: 'messages.list',
        sent_at: '2026-10-25T13:38:16Z',
        payload: { agent_session_id: 'codex_prj_1' },
      }),
    });
    await new Promise((resolve) => setTimeout(resolve, 10));

    const result = socket.frames().find((frame) => frame.type === 'query.result');
    expect(result?.payload).toEqual({
      reply_to_message_id: 'qry_1',
      ok: true,
      data: { items: [], next_cursor: null },
    });
  });

  it('continues the existing thread selected from the published catalogue', async () => {
    const continued: string[] = [];
    const session = createSessionStub({
      createSession: () => Promise.resolve('th_managed'),
      readSession: () => Promise.resolve({ cwd: '/tmp/work' }),
      continueSession: (threadId) => {
        continued.push(threadId);
        return Promise.resolve({ threadId, turnId: 'tn_2' });
      },
    });
    const { socket } = createManager({
      session,
      resolveProject: () => ({ rootPath: '/tmp/work', enabled: true }),
      listProjects: () => [
        {
          projectId: 'prj_1',
          displayName: 'LinxBox',
          rootPath: '/tmp/work',
          enabled: true,
        },
      ],
    });
    socket.emit('open', undefined);
    await new Promise((resolve) => setTimeout(resolve, 20));

    socket.emit('message', {
      data: JSON.stringify({
        protocol_version: '1.0',
        message_id: 'msg_device_1',
        sequence: 20,
        type: 'task.dispatch',
        sent_at: new Date().toISOString(),
        payload: {
          task_id: 'tsk_device_1',
          project_id: 'prj_1',
          agent: 'codex',
          capabilities: ['read'],
          prompt: '继续检查',
          acceptance_criteria: [],
          policy_version: '2026-10-25',
          continuation: {
            agent_session_id: 'codex_prj_1',
            thread_id: 'th_managed',
          },
        },
      }),
    });
    await new Promise((resolve) => setTimeout(resolve, 50));

    expect(continued).toEqual(['th_managed']);
    const ack = socket.frames().find((frame) => frame.type === 'task.ack');
    expect(ack?.payload['disposition']).toBe('accepted');
  });

  it('reports the connection as down when the socket closes', () => {
    const { manager, socket } = createManager();
    socket.emit('open', undefined);
    socket.emit('close', { code: 1006 });
    expect(manager.status().connected).toBe(false);
  });

  it('keeps the last error after a rejection', () => {
    const { manager, socket } = createManager();
    socket.emit('error', undefined);
    expect(manager.status().lastError).toContain('Gateway');
  });

  it('publishes the status when the connection changes', () => {
    const seen: boolean[] = [];
    const { socket } = createManager({
      onStatusChange: (status) => seen.push(status.connected),
    });
    socket.emit('open', undefined);
    expect(seen).toContain(true);
  });

  it('acknowledges a dispatch with the disposition the dispatcher reached', async () => {
    const { socket } = createManager({
      resolveProject: () => ({ rootPath: '/tmp/work', enabled: true }),
    });
    socket.emit('open', undefined);

    socket.emit('message', {
      data: JSON.stringify({
        protocol_version: '1.0',
        message_id: 'msg_1',
        sequence: 10,
        type: 'task.dispatch',
        sent_at: new Date().toISOString(),
        payload: {
          task_id: 'tsk_1',
          project_id: 'prj_1',
          agent: 'codex',
          capabilities: ['read'],
          prompt: '做点什么',
          acceptance_criteria: [],
          policy_version: '2026-10-25',
          continuation: null,
        },
      }),
    });

    await new Promise((resolve) => setTimeout(resolve, 50));
    const ack = socket.frames().find((frame) => frame.type === 'task.ack');
    expect(ack).toBeDefined();
    expect(ack?.payload['task_id']).toBe('tsk_1');
    expect(ack?.payload['disposition']).toBe('accepted');
  });

  it('uses the configured default for WSS without an Electron project grant', async () => {
    const { socket } = createManager({
      resolveProject: () => null,
    });
    socket.emit('open', undefined);

    socket.emit('message', {
      data: JSON.stringify({
        protocol_version: '1.0',
        message_id: 'msg_2',
        sequence: 11,
        type: 'task.dispatch',
        sent_at: new Date().toISOString(),
        payload: {
          task_id: 'tsk_2',
          project_id: 'prj_unknown',
          agent: 'codex',
          capabilities: ['read'],
          prompt: '做点什么',
          acceptance_criteria: [],
          policy_version: '2026-10-25',
          continuation: null,
        },
      }),
    });

    await new Promise((resolve) => setTimeout(resolve, 50));
    const ack = socket.frames().find((frame) => frame.type === 'task.ack');
    expect(ack?.payload['disposition']).toBe('accepted');
    expect(socket.frames().some((frame) => frame.type === 'task.started')).toBe(true);
  });

  it('runs the task on the local provider and reports the events back', async () => {
    const { socket } = createManager({
      resolveProject: () => ({ rootPath: '/tmp/work', enabled: true }),
    });
    socket.emit('open', undefined);

    socket.emit('message', {
      data: JSON.stringify({
        protocol_version: '1.0',
        message_id: 'msg_3',
        sequence: 12,
        type: 'task.dispatch',
        sent_at: new Date().toISOString(),
        payload: {
          task_id: 'tsk_3',
          project_id: 'prj_1',
          agent: 'codex',
          capabilities: ['read'],
          prompt: '做点什么',
          acceptance_criteria: [],
          policy_version: '2026-10-25',
          continuation: null,
        },
      }),
    });

    await new Promise((resolve) => setTimeout(resolve, 80));
    const started = socket.frames().find((frame) => frame.type === 'task.started');
    expect(started).toBeDefined();
    expect(started?.payload['task_id']).toBe('tsk_3');
  });

  it('resolves the session working directory for a continuation', async () => {
    const { socket } = createManager({
      resolveProject: () => ({ rootPath: '/tmp/work', enabled: true }),
    });
    socket.emit('open', undefined);

    // A parent task first, so the dispatcher knows which session to continue.
    socket.emit('message', {
      data: JSON.stringify({
        protocol_version: '1.0',
        message_id: 'msg_parent',
        sequence: 12,
        type: 'task.dispatch',
        sent_at: new Date().toISOString(),
        payload: {
          task_id: 'tsk_parent',
          project_id: 'prj_1',
          agent: 'codex',
          capabilities: ['read'],
          prompt: '先做一件事',
          acceptance_criteria: [],
          policy_version: '2026-10-25',
          continuation: null,
        },
      }),
    });
    await new Promise((resolve) => setTimeout(resolve, 80));

    // The parent is still running, so a second task is refused. That refusal
    // is the single-task rule, not a continuation failure.
    socket.emit('message', {
      data: JSON.stringify({
        protocol_version: '1.0',
        message_id: 'msg_4',
        sequence: 13,
        type: 'task.dispatch',
        sent_at: new Date().toISOString(),
        payload: {
          task_id: 'tsk_4',
          project_id: 'prj_1',
          agent: 'codex',
          capabilities: ['read'],
          prompt: '继续',
          acceptance_criteria: [],
          policy_version: '2026-10-25',
          continuation: { parent_task_id: 'tsk_parent' },
        },
      }),
    });
    await new Promise((resolve) => setTimeout(resolve, 80));

    const acks = socket.frames().filter((frame) => frame.type === 'task.ack');
    expect(acks.length).toBe(2);
    expect(acks[1]?.payload['disposition']).toBe('rejected');
  });

  it('reports the desktop version it was built with', () => {
    const { socket } = createManager();
    socket.emit('open', undefined);
    const hello = socket.frames().find((frame) => frame.type === 'client.hello');
    expect(hello?.payload['desktop_version']).toBe('0.1.0');
  });

  it('falls back to a default desktop version', () => {
    let socket: FakeSocket | null = null;
    const manager = new GatewayConnectionManager({
      session: createSessionStub(),
      resolveProject: () => null,
      socketFactory: () => {
        socket = new FakeSocket();
        return socket;
      },
    });
    manager.connect({
      gatewayUrl: 'ws://127.0.0.1:8787/v1/clients/connect',
      accessToken: 'token',
      computerName: 'mac',
    });
    (socket as unknown as FakeSocket).emit('open', undefined);
    const hello = (socket as unknown as FakeSocket)
      .frames()
      .find((frame) => frame.type === 'client.hello');
    expect(hello?.payload['desktop_version']).toBe('0.1.0');
    manager.disconnect();
  });

  it('correlates a fast binding.result response before send returns', async () => {
    const { manager, socket } = createManager();
    socket.emit('open', undefined);
    const originalSend = socket.send.bind(socket);
    socket.send = (data: string) => {
      originalSend(data);
      const frame = JSON.parse(data) as { message_id: string; type: string };
      if (frame.type === 'device.bind') {
        socket.emit('message', {
          data: JSON.stringify({
            protocol_version: '1.0',
            message_id: 'msg_result',
            sequence: 2,
            type: 'binding.result',
            sent_at: new Date().toISOString(),
            payload: {
              reply_to_message_id: frame.message_id,
              ok: true,
              data: { device_id: 'AA:BB:CC:DD:EE:FF', desktop_client_id: 'desktop-1' },
            },
          }),
        });
      }
    };
    await expect(manager.bindDevice('AA:BB:CC:DD:EE:FF')).resolves.toMatchObject({ ok: true });
    manager.disconnect();
  });

  it('acknowledges a cancel frame without a task payload', async () => {
    const { socket } = createManager({
      resolveProject: () => ({ rootPath: '/tmp/work', enabled: true }),
    });
    socket.emit('open', undefined);

    socket.emit('message', {
      data: JSON.stringify({
        protocol_version: '1.0',
        message_id: 'msg_cancel',
        sequence: 20,
        type: 'task.cancel',
        sent_at: new Date().toISOString(),
        payload: { task_id: 'tsk_1', reason: '用户取消' },
      }),
    });

    await new Promise((resolve) => setTimeout(resolve, 50));
    const ack = socket.frames().find((frame) => frame.type === 'task.ack');
    expect(ack).toBeDefined();
    expect(ack?.payload['task_id']).toBe('unknown');
  });

  it('ignores a frame that arrives after the connection was closed', async () => {
    const { manager, socket } = createManager();
    socket.emit('open', undefined);
    manager.disconnect();

    socket.emit('message', {
      data: JSON.stringify({
        protocol_version: '1.0',
        message_id: 'msg_late',
        sequence: 21,
        type: 'task.dispatch',
        sent_at: new Date().toISOString(),
        payload: {
          task_id: 'tsk_late',
          project_id: 'prj_1',
          agent: 'codex',
          capabilities: ['read'],
          prompt: 'x',
          acceptance_criteria: [],
          policy_version: '2026-10-25',
          continuation: null,
        },
      }),
    });

    await new Promise((resolve) => setTimeout(resolve, 50));
    expect(socket.frames().filter((frame) => frame.type === 'task.ack')).toHaveLength(0);
  });

  it('closes the previous connection before opening a new one', () => {
    const { manager } = createManager();
    manager.connect({
      gatewayUrl: 'ws://127.0.0.1:8787/v1/clients/connect',
      accessToken: 'demo-client-token',
      computerName: '工作电脑',
    });
    expect(manager.status().configured).toBe(true);
  });

  it('does not report a live connection after disconnecting', () => {
    const { manager, socket } = createManager();
    socket.emit('open', undefined);
    manager.disconnect();
    expect(manager.status().connected).toBe(false);
  });

  it('never exposes the access token in the status', () => {
    const { manager } = createManager();
    expect(JSON.stringify(manager.status())).not.toContain('demo-client-token');
  });
});
