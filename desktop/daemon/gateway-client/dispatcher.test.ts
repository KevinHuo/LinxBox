import { describe, expect, it } from 'vitest';

import { TaskDispatcher, type CodexRunner, type DispatchedTaskEvent } from './dispatcher.js';
import type { CodexDomainEvent } from '../codex-adapter/events.js';
import type { CodexTurnState } from '../codex-adapter/client.js';
import type { GatewayFrame } from '../../contracts/index.js';

const dispatchFrame = {
  protocol_version: '1.0',
  message_id: 'msg_1',
  sequence: 1043,
  type: 'task.dispatch',
  sent_at: '2026-10-24T01:00:00Z',
  payload: {
    task_id: 'tsk_1',
    project_id: 'prj_1',
    agent: 'codex',
    capabilities: ['read'],
    prompt: '做点事',
    acceptance_criteria: [],
    policy_version: '2026-10-24',
    continuation: null,
  },
} as unknown as GatewayFrame;

interface Harness {
  readonly dispatcher: TaskDispatcher;
  readonly events: DispatchedTaskEvent[];
  readonly started: { prompt: string; cwd: string }[];
  readonly continued: { threadId: string; prompt: string; cwd: string }[];
  revoke(projectId: string): void;
  emit(event: CodexDomainEvent): void;
}

function createHarness(
  options: {
    projects?: Record<string, { rootPath: string; enabled: boolean }>;
    sessionCwd?: string | null | ((threadId: string) => string | null);
    agentSessions?: Record<string, { threadId: string; projectId: string; rootPath: string }>;
  } = {},
): Harness {
  const events: DispatchedTaskEvent[] = [];
  const started: { prompt: string; cwd: string }[] = [];
  const continued: { threadId: string; prompt: string; cwd: string }[] = [];
  const listeners = new Set<(event: CodexDomainEvent) => void>();
  const projects = options.projects ?? {};

  const resolveSessionCwd = (threadId: string): string | null => {
    if (typeof options.sessionCwd === 'function') {
      return options.sessionCwd(threadId);
    }
    return options.sessionCwd === undefined ? '/tmp/app' : options.sessionCwd;
  };

  const state: CodexTurnState = {
    threadId: 'th_1',
    turnId: 'tn_1',
    status: 'running',
    progress: '',
    finalText: '',
    error: null,
    startedAt: '2026-10-24T01:00:00Z',
    completedAt: null,
  };

  const runner: CodexRunner = {
    startTurn: (input) => {
      started.push({ prompt: input.prompt, cwd: input.cwd });
      return Promise.resolve({ threadId: 'th_1', turnId: 'tn_1' });
    },
    continueTurn: (input) => {
      continued.push({ threadId: input.threadId, prompt: input.prompt, cwd: input.cwd });
      return Promise.resolve({ threadId: input.threadId, turnId: 'tn_2' });
    },
    sessionCwd: (threadId) => Promise.resolve(resolveSessionCwd(threadId)),
    on: (listener) => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    status: () => state,
  };

  const dispatcher = new TaskDispatcher({
    resolveProject: (projectId) => projects[projectId] ?? null,
    defaultCwd: '/tmp/app',
    resolveAgentSession: (agentSessionId) => {
      const target = options.agentSessions?.[agentSessionId];
      return target === undefined ? null : { agentSessionId, ...target };
    },
    runner,
    onEvent: (event) => events.push(event),
  });

  return {
    dispatcher,
    events,
    started,
    continued,
    revoke: (projectId) => {
      const project = projects[projectId];
      if (project !== undefined) {
        project.enabled = false;
      }
    },
    emit: (event) => {
      for (const listener of listeners) {
        listener(event);
      }
    },
  };
}

describe('TaskDispatcher', () => {
  it('forwards a WSS dispatch without an Electron project grant', async () => {
    const harness = createHarness();
    const outcome = await harness.dispatcher.handle(dispatchFrame);

    expect(outcome.disposition).toBe('accepted');
    expect(harness.started).toEqual([{ prompt: '做点事', cwd: '/tmp/app' }]);
  });

  it('accepts a dispatch for an authorised project and starts a turn', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });

    const outcome = await harness.dispatcher.handle(dispatchFrame);

    expect(outcome.disposition).toBe('accepted');
    expect(harness.started).toEqual([{ prompt: '做点事', cwd: '/tmp/app' }]);
    expect(harness.events.map((event) => event.type)).toContain('task.started');
  });

  it('reports the executed thread and turn on the started event', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });

    await harness.dispatcher.handle(dispatchFrame);

    const started = harness.events.find((event) => event.type === 'task.started');
    expect(started?.payload.thread_id).toBe('th_1');
    expect(started?.payload.turn_id).toBe('tn_1');
  });

  it('continues a locally published Codex session selected by the device', async () => {
    const harness = createHarness({
      projects: { prj_1: { rootPath: '/tmp/app', enabled: true } },
      agentSessions: {
        codex_th_existing: {
          threadId: 'th_existing',
          projectId: 'prj_1',
          rootPath: '/tmp/app',
        },
      },
    });
    const frame = {
      ...dispatchFrame,
      payload: {
        ...dispatchFrame.payload,
        continuation: {
          agent_session_id: 'codex_th_existing',
          thread_id: 'th_existing',
        },
      },
    } as unknown as GatewayFrame;

    const outcome = await harness.dispatcher.handle(frame);

    expect(outcome.disposition).toBe('accepted');
    expect(harness.started).toHaveLength(0);
    expect(harness.continued).toEqual([
      { threadId: 'th_existing', prompt: '做点事', cwd: '/tmp/app' },
    ]);
  });

  it('rejects a remote thread that was not published by this Desktop', async () => {
    const harness = createHarness({
      projects: { prj_1: { rootPath: '/tmp/app', enabled: true } },
    });
    const frame = {
      ...dispatchFrame,
      payload: {
        ...dispatchFrame.payload,
        continuation: { agent_session_id: 'unknown', thread_id: 'th_injected' },
      },
    } as unknown as GatewayFrame;

    const outcome = await harness.dispatcher.handle(frame);

    expect(outcome.disposition).toBe('rejected');
    expect(harness.continued).toHaveLength(0);
  });

  it('refuses a continuation with an unknown parent instead of starting a new session', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });

    const outcome = await harness.dispatcher.handle({
      ...dispatchFrame,
      message_id: 'msg_2',
      payload: { ...dispatchFrame.payload, continuation: { parent_task_id: 'tsk_missing' } },
    } as unknown as GatewayFrame);

    expect(outcome.disposition).toBe('rejected');
    expect(outcome.error).toBe('原任务不存在或不可续接');
    expect(harness.started).toHaveLength(0);
    expect(harness.continued).toHaveLength(0);
  });

  it('continues the parent conversation when the parent task is known', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });

    await harness.dispatcher.handle(dispatchFrame);
    harness.emit({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'completed',
      error: null,
    });

    const outcome = await harness.dispatcher.handle({
      ...dispatchFrame,
      message_id: 'msg_2',
      payload: {
        ...dispatchFrame.payload,
        task_id: 'tsk_2',
        continuation: { parent_task_id: 'tsk_1' },
      },
    } as unknown as GatewayFrame);

    expect(outcome.disposition).toBe('accepted');
    expect(harness.continued).toEqual([{ threadId: 'th_1', prompt: '做点事', cwd: '/tmp/app' }]);
    expect(harness.started).toHaveLength(1);
  });

  it('refuses a continuation whose parent task belongs to another project', async () => {
    const harness = createHarness({
      projects: {
        prj_1: { rootPath: '/tmp/app', enabled: true },
        prj_2: { rootPath: '/tmp/other', enabled: true },
      },
    });

    await harness.dispatcher.handle(dispatchFrame);
    harness.emit({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'completed',
      error: null,
    });

    const outcome = await harness.dispatcher.handle({
      ...dispatchFrame,
      message_id: 'msg_2',
      payload: {
        ...dispatchFrame.payload,
        project_id: 'prj_2',
        task_id: 'tsk_2',
        continuation: { parent_task_id: 'tsk_1' },
      },
    } as unknown as GatewayFrame);

    expect(outcome.disposition).toBe('rejected');
    expect(outcome.error).toBe('续接目标不属于本次授权项目');
    expect(harness.continued).toHaveLength(0);
  });

  it('refuses a continuation after the parent project authorisation was revoked', async () => {
    const harness = createHarness({
      projects: { prj_1: { rootPath: '/tmp/app', enabled: true } },
    });

    await harness.dispatcher.handle(dispatchFrame);
    harness.emit({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'completed',
      error: null,
    });
    harness.revoke('prj_1');

    const outcome = await harness.dispatcher.handle({
      ...dispatchFrame,
      message_id: 'msg_2',
      payload: {
        ...dispatchFrame.payload,
        task_id: 'tsk_2',
        continuation: { parent_task_id: 'tsk_1' },
      },
    } as unknown as GatewayFrame);

    expect(outcome.disposition).toBe('rejected');
    expect(outcome.error).toBe('项目授权已撤销');
    expect(harness.continued).toHaveLength(0);
  });

  it('refuses a continuation when the authorisation is revoked while the session is looked up', async () => {
    let revokeProject: () => void = () => undefined;
    const harness = createHarness({
      projects: { prj_1: { rootPath: '/tmp/app', enabled: true } },
      sessionCwd: () => {
        revokeProject();
        return '/tmp/app';
      },
    });
    revokeProject = () => harness.revoke('prj_1');

    await harness.dispatcher.handle(dispatchFrame);
    harness.emit({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'completed',
      error: null,
    });

    const outcome = await harness.dispatcher.handle({
      ...dispatchFrame,
      message_id: 'msg_2',
      payload: {
        ...dispatchFrame.payload,
        task_id: 'tsk_2',
        continuation: { parent_task_id: 'tsk_1' },
      },
    } as unknown as GatewayFrame);

    expect(outcome.disposition).toBe('rejected');
    expect(outcome.error).toBe('原任务项目授权已撤销');
    expect(harness.continued).toHaveLength(0);
  });

  it('refuses a continuation whose session directory is outside the authorised project', async () => {
    const harness = createHarness({
      projects: { prj_1: { rootPath: '/tmp/app', enabled: true } },
      sessionCwd: '/tmp/elsewhere',
    });

    await harness.dispatcher.handle(dispatchFrame);
    harness.emit({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'completed',
      error: null,
    });

    const outcome = await harness.dispatcher.handle({
      ...dispatchFrame,
      message_id: 'msg_2',
      payload: {
        ...dispatchFrame.payload,
        task_id: 'tsk_2',
        continuation: { parent_task_id: 'tsk_1' },
      },
    } as unknown as GatewayFrame);

    expect(outcome.disposition).toBe('rejected');
    expect(outcome.error).toBe('目标会话不属于本次授权项目');
    expect(harness.continued).toHaveLength(0);
  });

  it('refuses a continuation whose session has no verifiable working directory', async () => {
    const harness = createHarness({
      projects: { prj_1: { rootPath: '/tmp/app', enabled: true } },
      sessionCwd: null,
    });

    await harness.dispatcher.handle(dispatchFrame);
    harness.emit({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'completed',
      error: null,
    });

    const outcome = await harness.dispatcher.handle({
      ...dispatchFrame,
      message_id: 'msg_2',
      payload: {
        ...dispatchFrame.payload,
        task_id: 'tsk_2',
        continuation: { parent_task_id: 'tsk_1' },
      },
    } as unknown as GatewayFrame);

    expect(outcome.disposition).toBe('rejected');
    expect(outcome.error).toBe('目标会话没有可校验的工作目录');
    expect(harness.continued).toHaveLength(0);
  });

  it('rejects a dispatch for an unknown project', async () => {
    const harness = createHarness();

    const outcome = await harness.dispatcher.handle(dispatchFrame);

    expect(outcome.disposition).toBe('rejected');
    expect(harness.started).toHaveLength(0);
  });

  it('rejects a dispatch for a revoked project', async () => {
    const harness = createHarness({
      projects: { prj_1: { rootPath: '/tmp/app', enabled: false } },
    });

    const outcome = await harness.dispatcher.handle(dispatchFrame);

    expect(outcome.disposition).toBe('rejected');
    expect(harness.started).toHaveLength(0);
  });

  it('treats a replayed message id as duplicate and never runs a second turn', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });

    const first = await harness.dispatcher.handle(dispatchFrame);
    const second = await harness.dispatcher.handle(dispatchFrame);

    expect(first.disposition).toBe('accepted');
    expect(second.disposition).toBe('duplicate');
    expect(harness.started).toHaveLength(1);
  });

  it('reserves a message before awaiting the provider side effect', async () => {
    let resolveStart: ((value: { threadId: string; turnId: string }) => void) | undefined;
    let starts = 0;
    const dispatcher = new TaskDispatcher({
      resolveProject: () => ({ rootPath: '/tmp/app', enabled: true }),
      runner: {
        startTurn: () => {
          starts += 1;
          return new Promise((resolve) => {
            resolveStart = resolve;
          });
        },
        continueTurn: () => Promise.resolve({ threadId: 'th_1', turnId: 'tn_1' }),
        sessionCwd: () => Promise.resolve('/tmp/app'),
        on: () => () => undefined,
        status: () => ({
          threadId: null,
          turnId: null,
          status: 'running',
          progress: '',
          finalText: '',
          error: null,
          startedAt: null,
          completedAt: null,
        }),
      },
      onEvent: () => undefined,
    });

    const first = dispatcher.handle(dispatchFrame);
    await Promise.resolve();
    const replay = await dispatcher.handle(dispatchFrame);

    expect(replay.disposition).toBe('duplicate');
    expect(starts).toBe(1);
    resolveStart?.({ threadId: 'th_1', turnId: 'tn_1' });
    await expect(first).resolves.toMatchObject({ disposition: 'accepted' });
  });

  it('keeps an accepted delivery when a terminal event arrives before start rejects', async () => {
    const events: DispatchedTaskEvent[] = [];
    let rejectStart: ((reason: Error) => void) | undefined;
    let listener: ((event: CodexDomainEvent) => void) | undefined;
    const dispatcher = new TaskDispatcher({
      resolveProject: () => ({ rootPath: '/tmp/app', enabled: true }),
      runner: {
        startTurn: () =>
          new Promise((_resolve, reject) => {
            rejectStart = reject;
          }),
        continueTurn: () =>
          new Promise((_resolve, reject) => {
            rejectStart = reject;
          }),
        sessionCwd: () => Promise.resolve('/tmp/app'),
        on: (next) => {
          listener = next;
          return () => undefined;
        },
        status: () => ({
          threadId: 'th_1',
          turnId: 'tn_1',
          status: 'running',
          progress: '',
          finalText: '',
          error: null,
          startedAt: null,
          completedAt: null,
        }),
      },
      onEvent: (event) => events.push(event),
    });

    const pending = dispatcher.handle(dispatchFrame);
    await Promise.resolve();
    listener?.({ type: 'turn-started', threadId: 'th_1', turnId: 'tn_1' });
    listener?.({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'interrupted',
      error: 'provider exited',
    });
    rejectStart?.(new Error('provider exited'));

    await expect(pending).resolves.toEqual({ disposition: 'accepted', error: null });
    expect(events.map((event) => event.type)).toEqual(['task.interrupted']);
  });

  it('rejects a second task while one provider turn is active', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);
    const secondFrame = {
      ...dispatchFrame,
      message_id: 'msg_2',
      payload: { ...dispatchFrame.payload, task_id: 'tsk_2' },
    } as GatewayFrame;

    const second = await harness.dispatcher.handle(secondFrame);

    expect(second).toEqual({ disposition: 'rejected', error: '客户端正在执行另一项任务' });
    expect(harness.started).toHaveLength(1);
  });

  it('maps provider commentary to a progress event', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);

    harness.emit({ type: 'progress', threadId: 'th_1', turnId: 'tn_1', text: '正在看代码' });

    const progress = harness.events.find((event) => event.type === 'task.progress');
    expect(progress?.payload.summary).toBe('正在看代码');
  });

  it('ignores events that belong to another provider turn', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);

    harness.emit({ type: 'progress', threadId: 'th_other', turnId: 'tn_other', text: '串线内容' });
    harness.emit({
      type: 'terminal',
      threadId: 'th_other',
      turnId: 'tn_other',
      outcome: 'completed',
      error: null,
    });

    expect(harness.events.map((event) => event.type)).toEqual(['task.started']);
  });

  it('sanitizes provider text and never uploads an internal error', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);

    harness.emit({
      type: 'progress',
      threadId: 'th_1',
      turnId: 'tn_1',
      text: '读取 /Users/demo/private.txt，token sk-1234567890abcdef password=hunter2',
    });
    harness.emit({
      type: 'provider-error',
      threadId: 'th_1',
      turnId: 'tn_1',
      message: 'secret at /Users/demo/private.txt: sk-1234567890abcdef',
    });

    const serialized = JSON.stringify(harness.events);
    expect(serialized).not.toContain('/Users/demo');
    expect(serialized).not.toContain('sk-1234567890abcdef');
    expect(serialized).not.toContain('hunter2');
    expect(serialized).toContain('[路径]');
    expect(serialized).toContain('[凭据]');
    expect(serialized).toContain('本机 Codex 返回错误');
  });

  it('emits exactly one terminal event', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);

    harness.emit({
      type: 'provider-error',
      threadId: 'th_1',
      turnId: 'tn_1',
      message: 'usage limit',
    });
    harness.emit({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'failed',
      error: 'usage limit',
    });

    const terminals = harness.events.filter(
      (event) => event.type !== 'task.started' && event.type !== 'task.progress',
    );
    expect(terminals).toHaveLength(1);
    expect(terminals[0]?.type).toBe('task.failed');
  });

  it('maps a proven success to task.completed', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);

    harness.emit({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'completed',
      error: null,
    });

    const terminal = harness.events.find((event) => event.type === 'task.completed');
    expect(terminal?.payload.state).toBe('succeeded');
  });

  it('maps an unprovable outcome to task.interrupted', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);

    harness.emit({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'interrupted',
      error: null,
    });

    const terminal = harness.events.find((event) => event.type === 'task.interrupted');
    expect(terminal?.payload.state).toBe('interrupted');
  });

  it('maps an explicit provider failure to task.failed', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);

    harness.emit({
      type: 'terminal',
      threadId: 'th_1',
      turnId: 'tn_1',
      outcome: 'failed',
      error: 'private provider detail',
    });

    const terminal = harness.events.find((event) => event.type === 'task.failed');
    expect(terminal?.payload.error?.message).toBe('本机 Codex 任务失败');
    expect(JSON.stringify(terminal)).not.toContain('private provider detail');
  });

  it('rejects an unsupported message type', async () => {
    const harness = createHarness();
    const frame = {
      ...dispatchFrame,
      type: 'project.upsert',
      payload: {
        project_id: 'prj_1',
        display_name: 'x',
        capabilities: [],
        availability: 'online',
        grant_version: 1,
      },
    } as unknown as GatewayFrame;

    const outcome = await harness.dispatcher.handle(frame);

    expect(outcome.disposition).toBe('rejected');
  });
});

describe('TaskDispatcher cancel and failure paths', () => {
  it('accepts a cancel request without starting a turn', async () => {
    const harness = createHarness();
    const frame = {
      protocol_version: '1.0',
      message_id: 'msg_cancel',
      sequence: 9,
      type: 'task.cancel',
      sent_at: '2026-10-24T01:00:00Z',
      payload: { task_id: 'tsk_1', reason: 'user_requested' },
    } as unknown as GatewayFrame;

    const outcome = await harness.dispatcher.handle(frame);

    expect(outcome.disposition).toBe('accepted');
    expect(harness.started).toHaveLength(0);
  });

  it('reports a rejected disposition when the provider cannot start', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    const failing = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    void failing;

    const dispatcher = new TaskDispatcher({
      resolveProject: () => ({ rootPath: '/tmp/app', enabled: true }),
      runner: {
        startTurn: () => Promise.reject(new Error('provider unavailable')),
        continueTurn: () => Promise.reject(new Error('provider unavailable')),
        sessionCwd: () => Promise.resolve('/tmp/app'),
        on: () => () => undefined,
        status: () => ({
          threadId: null,
          turnId: null,
          status: 'idle',
          progress: '',
          finalText: '',
          error: null,
          startedAt: null,
          completedAt: null,
        }),
      },
      onEvent: (event) => harness.events.push(event),
    });

    const outcome = await dispatcher.handle(dispatchFrame);

    expect(outcome.disposition).toBe('rejected');
    expect(harness.events.some((event) => event.type === 'task.failed')).toBe(true);
  });

  it('reports every disposition through the optional callback', async () => {
    const dispositions: string[] = [];
    const listeners = new Set<(event: CodexDomainEvent) => void>();
    const dispatcher = new TaskDispatcher({
      resolveProject: (projectId) =>
        projectId === 'prj_1' ? { rootPath: '/tmp/app', enabled: true } : null,
      runner: {
        startTurn: () => Promise.resolve({ threadId: 'th_1', turnId: 'tn_1' }),
        continueTurn: () => Promise.resolve({ threadId: 'th_1', turnId: 'tn_2' }),
        sessionCwd: () => Promise.resolve('/tmp/app'),
        on: (listener) => {
          listeners.add(listener);
          return () => listeners.delete(listener);
        },
        status: () => ({
          threadId: 'th_1',
          turnId: 'tn_1',
          status: 'running',
          progress: '',
          finalText: '',
          error: null,
          startedAt: null,
          completedAt: null,
        }),
      },
      onEvent: () => undefined,
      onDisposition: (disposition, messageId, error) => {
        dispositions.push([disposition, messageId, error ?? ''].join(':'));
      },
    });

    await dispatcher.handle({
      ...dispatchFrame,
      message_id: 'unknown',
      payload: { ...dispatchFrame.payload, project_id: 'missing' },
    } as GatewayFrame);
    await dispatcher.handle(dispatchFrame);
    await dispatcher.handle(dispatchFrame);
    await dispatcher.handle({ ...dispatchFrame, message_id: 'busy' });

    expect(dispositions).toEqual([
      'rejected:unknown:项目未授权',
      'accepted:msg_1:',
      'duplicate:msg_1:',
      'rejected:busy:客户端正在执行另一项任务',
    ]);
  });
});

describe('TaskDispatcher provider event mapping', () => {
  it('maps a final answer to a progress event', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);

    harness.emit({
      type: 'final',
      threadId: 'th_1',
      turnId: 'tn_1',
      text: '最终回答',
    });

    const progress = harness.events.find((event) => event.type === 'task.progress');
    expect(progress?.payload.stage).toBe('summarizing');
    expect(progress?.payload.summary).toBe('最终回答');
  });

  it('ignores events that carry no task meaning', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);

    harness.emit({ type: 'turn-started', threadId: 'th_1', turnId: 'tn_1' });
    harness.emit({ type: 'thread-started', threadId: 'th_1' });
    harness.emit({ type: 'warning', threadId: 'th_1', message: '配置被忽略' });
    harness.emit({ type: 'account', authMode: 'chatgpt', planType: 'plus' });

    expect(harness.events.filter((event) => event.type === 'task.progress')).toHaveLength(0);
  });
});
