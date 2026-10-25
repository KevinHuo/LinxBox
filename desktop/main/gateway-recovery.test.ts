import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import {
  GatewayConnectionManager,
  type GatewayConnectionManagerOptions,
} from './gateway-connection.js';
import type { CodexSession } from './codex-session.js';
import type { CodexDomainEvent } from '../daemon/codex-adapter/events.js';
import type { SocketLike } from '../daemon/gateway-client/client.js';
import type {
  CodexSessionDetailView,
  CodexSessionSummaryView,
  CodexTurnView,
} from '../shared/ipc.js';

class MemorySocket implements SocketLike {
  readyState = 1;
  readonly sent: { type: string; message_id: string; payload: Record<string, unknown> }[] = [];
  readonly listeners = new Map<string, ((event: unknown) => void)[]>();
  send(raw: string): void {
    this.sent.push(JSON.parse(raw) as (typeof this.sent)[number]);
  }
  close(): void {
    this.readyState = 3;
  }
  addEventListener(type: string, listener: (event: never) => void): void {
    const listeners = this.listeners.get(type) ?? [];
    listeners.push(listener as (event: unknown) => void);
    this.listeners.set(type, listeners);
  }
  emit(type: string, value: unknown = undefined): void {
    for (const listener of this.listeners.get(type) ?? []) listener(value);
  }
  receive(type: string, payload: unknown, id = 'incoming'): void {
    this.emit('message', {
      data: JSON.stringify({
        protocol_version: '1.0',
        message_id: id,
        sequence: 1,
        sent_at: new Date().toISOString(),
        type,
        payload,
      }),
    });
  }
  last(type: string): Record<string, unknown> | undefined {
    return this.sent.filter((frame) => frame.type === type).at(-1)?.payload;
  }
}

const project = {
  projectId: 'project',
  displayName: '本机项目',
  rootPath: '/tmp/project',
  enabled: true,
};
const turn: CodexTurnView = {
  threadId: 'thread',
  turnId: 'turn',
  status: 'running',
  progress: '',
  finalText: '',
  error: null,
  startedAt: null,
  completedAt: null,
};
const detail: CodexSessionDetailView = {
  threadId: 'thread',
  title: '会话',
  cwd: project.rootPath,
  turns: [
    {
      turnId: 'turn',
      status: 'completed',
      startedAt: null,
      messages: [
        { id: 'u', role: 'user', text: '问题', phase: null },
        { id: 'a', role: 'assistant', text: '中🙂'.repeat(300), phase: 'final_answer' },
      ],
    },
  ],
};
const summary = (overrides: Partial<CodexSessionSummaryView> = {}): CodexSessionSummaryView => ({
  threadId: 'thread',
  title: '会话',
  cwd: project.rootPath,
  model: null,
  createdAt: null,
  updatedAt: null,
  status: 'idle',
  ...overrides,
});
const managers: GatewayConnectionManager[] = [];
const config = {
  gatewayUrl: 'ws://localhost:8787/v1/clients/connect',
  accessToken: 'mock-secret',
  computerName: '模拟电脑',
};

function harness(options: Partial<GatewayConnectionManagerOptions> = {}) {
  const listeners = new Set<(event: CodexDomainEvent) => void>();
  const session = {
    startTurn: vi.fn<CodexSession['startTurn']>().mockResolvedValue(turn),
    continueSession: vi.fn<CodexSession['continueSession']>().mockResolvedValue(turn),
    readSession: vi.fn<CodexSession['readSession']>().mockResolvedValue(detail),
    listSessions: vi.fn<CodexSession['listSessions']>().mockResolvedValue([]),
    listProjects: vi.fn<CodexSession['listProjects']>().mockResolvedValue([]),
    createSession: vi.fn<CodexSession['createSession']>().mockResolvedValue('managed'),
    close: vi.fn(),
    interrupt: vi.fn<CodexSession['interrupt']>().mockResolvedValue(undefined),
    on: (listener: (event: CodexDomainEvent) => void) => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    status: () => turn,
  };
  const sockets: MemorySocket[] = [];
  const onTaskEvent = vi.fn();
  const onProjectChange = vi.fn();
  const manager = new GatewayConnectionManager({
    session: session as unknown as CodexSession,
    resolveProject: () => null,
    listProjects: () => [project],
    onTaskEvent,
    onProjectChange,
    socketFactory: () => {
      const socket = new MemorySocket();
      sockets.push(socket);
      return socket;
    },
    ...options,
  });
  managers.push(manager);
  manager.connect(config);
  const socket = sockets[0]!;
  return {
    manager,
    socket,
    sockets,
    session,
    onTaskEvent,
    onProjectChange,
    emit: (event: CodexDomainEvent) => {
      for (const listener of listeners) listener(event);
    },
  };
}

async function open(h: ReturnType<typeof harness>): Promise<void> {
  h.socket.emit('open');
  await h.manager.syncAgentSessions();
}
async function flush(): Promise<void> {
  await vi.advanceTimersByTimeAsync(0);
}
function dispatch(
  h: ReturnType<typeof harness>,
  continuation: unknown = { agent_session_id: 'codex_project', thread_id: 'managed' },
): void {
  h.socket.receive('task.dispatch', {
    task_id: 'task',
    project_id: 'project',
    agent: 'codex',
    capabilities: [],
    prompt: '继续',
    acceptance_criteria: [],
    policy_version: 'test',
    continuation,
  });
}

beforeEach(() => {
  vi.useFakeTimers();
});
afterEach(() => {
  for (const manager of managers.splice(0)) manager.disconnect();
  vi.clearAllTimers();
  vi.useRealTimers();
});

describe('配对与会话同步恢复', () => {
  it('离线配对明确失败；在线等待超时、断线与发送错误分别收敛', async () => {
    const h = harness();
    await expect(h.manager.bindDevice('AA:BB:CC:DD:EE:FF')).resolves.toMatchObject({
      ok: false,
      error: { message: 'Gateway 尚未连接' },
    });
    await open(h);
    const pending = h.manager.bindDevice('AA:BB:CC:DD:EE:FF');
    await vi.advanceTimersByTimeAsync(10_000);
    await expect(pending).resolves.toMatchObject({
      ok: false,
      error: { message: 'Gateway 配对响应超时' },
    });
    const interrupted = h.manager.bindDevice('AA:BB:CC:DD:EE:FF');
    h.manager.disconnect();
    await expect(interrupted).resolves.toMatchObject({
      ok: false,
      error: { message: 'Gateway 连接已断开' },
    });
  });
  it('发送失败清理配对定时器', async () => {
    const h = harness();
    await open(h);
    const error = new Error('模拟发送失败');
    h.socket.send = () => {
      throw error;
    };
    await expect(h.manager.bindDevice('AA:BB:CC:DD:EE:FF')).resolves.toMatchObject({
      ok: false,
      error: { message: error.message },
    });
    expect(vi.getTimerCount()).toBe(1); // 只剩连接心跳。
  });
  it('无对应等待者的配对应答被忽略，目录拒绝原因保留', async () => {
    const h = harness();
    await open(h);
    h.socket.receive('binding.result', {
      reply_to_message_id: 'unknown',
      ok: true,
      data: { device_id: 'AA:BB:CC:DD:EE:FF', desktop_client_id: 'desktop' },
    });
    h.socket.receive('agent_sessions.sync.result', {
      reply_to_message_id: 'sync',
      ok: false,
      error: { code: 'INVALID_ARGUMENT', message: '目录不合法' },
    });
    expect(h.manager.status().lastError).toBe('目录不合法');
    h.socket.receive('agent_sessions.sync.result', {
      reply_to_message_id: 'sync',
      ok: true,
    });
    expect(h.manager.status().lastError).toBe('目录不合法');
  });
  it('成功同步不会遗留已经无用的超时定时器，且复用并发同步', async () => {
    const h = harness();
    h.socket.emit('open');
    const one = h.manager.syncAgentSessions();
    expect(h.manager.syncAgentSessions()).toBe(one);
    await one;
    expect(vi.getTimerCount()).toBe(1);
    await h.manager.syncAgentSessions();
    expect(h.session.createSession).toHaveBeenCalledTimes(1);
  });
  it('创建会话失败不上传假 thread，下次同步重新尝试', async () => {
    const h = harness();
    h.session.createSession.mockRejectedValueOnce(new Error('不可创建')).mockResolvedValue('retry');
    await open(h);
    expect(h.socket.last('agent_sessions.sync')?.['items']).toEqual([]);
    await h.manager.syncAgentSessions();
    expect(h.socket.last('agent_sessions.sync')?.['items']).toMatchObject([{ thread_id: 'retry' }]);
    expect(h.session.createSession).toHaveBeenCalledTimes(2);
  });
  it('provider 超时后采用默认项目，不把空根目录当作项目', async () => {
    const h = harness({ listProjects: () => [] });
    h.session.listProjects.mockReturnValue(new Promise(() => {}));
    h.socket.emit('open');
    const sync = h.manager.syncAgentSessions();
    await vi.advanceTimersByTimeAsync(2000);
    await sync;
    expect(h.socket.last('agent_sessions.sync')?.['items']).toMatchObject([
      { project_id: 'codex-default' },
    ]);
    h.session.listProjects.mockResolvedValue([
      { projectId: 'empty', name: '无根目录', roots: [], position: 1 },
      { projectId: 'real', name: '真实项目', roots: ['/tmp/provider'], position: 2 },
    ]);
    await h.manager.syncAgentSessions();
    expect(h.socket.last('agent_sessions.sync')?.['items']).toMatchObject([
      { project_id: 'real', project_name: '真实项目' },
    ]);
  });
  it('会话查询失败报告错误；断线中的迟到查询不发送旧目录', async () => {
    const h = harness();
    h.session.listSessions.mockRejectedValueOnce(new Error('查询失败'));
    await open(h);
    expect(h.manager.status().lastError).toBe('读取本机 Codex 会话目录失败');
    const late = harness();
    let resolve!: (value: string) => void;
    late.session.createSession.mockReturnValue(
      new Promise((done) => {
        resolve = done;
      }),
    );
    late.socket.emit('open');
    const pending = late.manager.syncAgentSessions();
    late.manager.disconnect();
    resolve('late');
    await pending;
    expect(late.socket.last('agent_sessions.sync')).toBeUndefined();
  });
  it('筛选真实本机会话并优先匹配最深项目根目录', async () => {
    const h = harness({
      listProjects: () => [
        project,
        { ...project, projectId: 'nested', displayName: '子目录', rootPath: '/tmp/project/sub' },
      ],
    });
    h.session.listSessions.mockResolvedValue([
      summary({ threadId: 'null', cwd: null }),
      summary({ threadId: 'outside', cwd: '/tmp/other' }),
      summary({ threadId: 'deep', cwd: '/tmp/project/sub/src', status: 'completed' }),
      summary({ threadId: 'failed', status: 'failed' }),
      summary({ threadId: 'running', status: 'running' }),
      summary({ threadId: 'not-loaded', status: 'notLoaded' }),
    ]);
    await open(h);
    const items = h.socket.last('agent_sessions.sync')?.['items'];
    expect(items).toEqual(
      expect.arrayContaining([
        expect.objectContaining({
          agent_session_id: 'codex_deep',
          project_id: 'nested',
          status: 'completed',
        }),
        expect.objectContaining({ agent_session_id: 'codex_failed', status: 'failed' }),
        expect.objectContaining({ agent_session_id: 'codex_running', status: 'running' }),
        expect.objectContaining({ agent_session_id: 'codex_not-loaded', status: 'idle' }),
      ]),
    );
    expect(JSON.stringify(items)).not.toContain('/tmp/');
    expect(JSON.stringify(items)).not.toContain('codex_outside');
    expect(JSON.stringify(items)).not.toContain('codex_null');
  });
});

describe('历史与正文应答', () => {
  it('查询配对原包 ID，正文分页按 UTF-8 字节且不拆字符', async () => {
    const h = harness();
    await open(h);
    h.socket.receive('messages.list', { agent_session_id: 'codex_project' }, 'history');
    await flush();
    expect(h.socket.last('query.result')).toMatchObject({
      reply_to_message_id: 'history',
      ok: true,
      data: {
        items: [
          { message_id: 'u', role: 'user' },
          { message_id: 'a', role: 'assistant' },
        ],
        next_cursor: null,
      },
    });
    h.socket.receive(
      'message.content.get',
      { agent_session_id: 'codex_project', message_id: 'a' },
      'body',
    );
    await flush();
    const result = h.socket.last('query.result') as { data: { text: string; next_offset: number } };
    expect(result).toMatchObject({ reply_to_message_id: 'body', ok: true });
    expect(Buffer.byteLength(result.data.text)).toBeLessThanOrEqual(1024);
    expect(result.data.text).not.toContain('\uFFFD');
    expect(result.data.next_offset).toBe(Buffer.byteLength(result.data.text));
    h.socket.receive(
      'message.content.get',
      { agent_session_id: 'codex_project', message_id: 'u', offset: 0 },
      'small',
    );
    await flush();
    expect(h.socket.last('query.result')).toMatchObject({
      ok: true,
      data: { text: '问题', next_offset: null },
    });
  });
  it.each([
    'unpublished',
    'session-missing',
    'message-missing',
    'provider-error',
    'non-error',
    'unsupported',
    'invalid-offset',
  ])('查询失败保持可配对错误：%s', async (mode) => {
    const h = harness();
    await open(h);
    if (mode === 'session-missing') h.session.readSession.mockResolvedValue(null);
    if (mode === 'provider-error')
      h.session.readSession.mockRejectedValue(new Error('本机查询失败'));
    if (mode === 'non-error') h.session.readSession.mockRejectedValue('未知异常');
    if (mode === 'unsupported')
      h.session.readSession.mockRejectedValue(new Error('list_turns is not supported yet'));
    const bodyQuery = mode === 'message-missing' || mode === 'invalid-offset';
    h.socket.receive(
      bodyQuery ? 'message.content.get' : 'messages.list',
      {
        agent_session_id: mode === 'unpublished' ? 'unknown' : 'codex_project',
        ...(bodyQuery
          ? {
              message_id: mode === 'message-missing' ? 'missing' : 'a',
              offset: mode === 'invalid-offset' ? 1 : 0,
            }
          : {}),
      },
      'query',
    );
    await flush();
    const result = h.socket.last('query.result');
    expect(result?.['reply_to_message_id']).toBe('query');
    if (mode === 'unsupported')
      expect(result).toMatchObject({ ok: true, data: { items: [], next_cursor: null } });
    else {
      expect(result).toMatchObject({ ok: false, error: { code: 'NOT_FOUND' } });
      expect(typeof (result?.['error'] as Record<string, unknown>)['message']).toBe('string');
    }
  });
});

describe('执行事实与连接生命周期', () => {
  it.each(['completed', 'failed', 'interrupted'] as const)(
    '真实终态 %s 回传并释放 provider，失败串线不标成功',
    async (outcome) => {
      const h = harness();
      await open(h);
      dispatch(h);
      await flush();
      expect(h.session.continueSession).toHaveBeenCalledWith('managed', '继续', project.rootPath);
      h.emit({
        type: 'terminal',
        threadId: 'thread',
        turnId: 'turn',
        outcome,
        error: outcome === 'completed' ? null : 'active writer conflict',
      });
      await flush();
      expect(
        h.socket.last(
          outcome === 'completed'
            ? 'task.completed'
            : outcome === 'failed'
              ? 'task.failed'
              : 'task.interrupted',
        ),
      ).toBeDefined();
      expect(h.session.close).toHaveBeenCalledTimes(1);
      expect(h.onTaskEvent).toHaveBeenCalled();
      if (outcome === 'completed')
        expect(h.socket.last('message.available')).toMatchObject({
          agent_session_id: 'codex_thread',
          message_id: 'a',
        });
    },
  );
  it('无最终回复时有限重试，不伪造新消息通知', async () => {
    const h = harness();
    await open(h);
    dispatch(h);
    await flush();
    h.session.readSession
      .mockRejectedValueOnce(new Error('尚未写入'))
      .mockResolvedValue({ ...detail, turns: [] });
    h.emit({
      type: 'terminal',
      threadId: 'thread',
      turnId: 'turn',
      outcome: 'completed',
      error: null,
    });
    await vi.advanceTimersByTimeAsync(1000);
    expect(h.session.readSession).toHaveBeenCalledTimes(4);
    expect(h.socket.last('message.available')).toBeUndefined();
  });
  it('重连前完成的旧查询不能向新连接发布通知', async () => {
    const h = harness();
    await open(h);
    dispatch(h);
    await flush();
    let resolve!: (value: CodexSessionDetailView | null) => void;
    h.session.readSession.mockReturnValue(
      new Promise((done) => {
        resolve = done;
      }),
    );
    h.emit({
      type: 'terminal',
      threadId: 'thread',
      turnId: 'turn',
      outcome: 'completed',
      error: null,
    });
    h.manager.disconnect();
    resolve(detail);
    await flush();
    expect(h.socket.last('message.available')).toBeUndefined();
  });
  it('项目版本只采用不更旧的元数据，同时保留本机目录解析', async () => {
    const h = harness();
    await open(h);
    const upsert = (version: number, availability: 'online' | 'offline') =>
      h.socket.receive('project.upsert', {
        project_id: 'project',
        display_name: '远端展示',
        capabilities: ['read'],
        availability,
        grant_version: version,
      });
    const revoke = (version: number) =>
      h.socket.receive('project.revoke', {
        project_id: 'project',
        grant_version: version,
        revoked_at: new Date().toISOString(),
      });
    upsert(2, 'online');
    upsert(1, 'offline');
    revoke(1);
    dispatch(h, null);
    await flush();
    expect(h.session.startTurn).toHaveBeenCalledWith('继续', project.rootPath);
    expect(h.onProjectChange).toHaveBeenCalledTimes(3);
    const second = harness();
    await open(second);
    second.socket.receive('project.revoke', {
      project_id: 'unseen',
      grant_version: 1,
      revoked_at: new Date().toISOString(),
    });
    second.socket.receive('project.revoke', {
      project_id: 'project',
      grant_version: 3,
      revoked_at: new Date().toISOString(),
    });
    dispatch(second, null);
    await flush();
    expect(second.session.startTurn).toHaveBeenCalled(); // 离线展示元数据不改变 main 已确认的默认目录规则。
  });
});
