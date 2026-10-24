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
  emit(event: CodexDomainEvent): void;
}

function createHarness(
  options: { projects?: Record<string, { rootPath: string; enabled: boolean }> } = {},
): Harness {
  const events: DispatchedTaskEvent[] = [];
  const started: { prompt: string; cwd: string }[] = [];
  const listeners = new Set<(event: CodexDomainEvent) => void>();

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
    on: (listener) => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    status: () => state,
  };

  const dispatcher = new TaskDispatcher({
    resolveProject: (projectId) => options.projects?.[projectId] ?? null,
    runner,
    onEvent: (event) => events.push(event),
  });

  return {
    dispatcher,
    events,
    started,
    emit: (event) => {
      for (const listener of listeners) {
        listener(event);
      }
    },
  };
}

describe('TaskDispatcher', () => {
  it('accepts a dispatch for an authorised project and starts a turn', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });

    const outcome = await harness.dispatcher.handle(dispatchFrame);

    expect(outcome.disposition).toBe('accepted');
    expect(harness.started).toEqual([{ prompt: '做点事', cwd: '/tmp/app' }]);
    expect(harness.events.map((event) => event.type)).toContain('task.started');
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

  it('maps provider commentary to a progress event', async () => {
    const harness = createHarness({ projects: { prj_1: { rootPath: '/tmp/app', enabled: true } } });
    await harness.dispatcher.handle(dispatchFrame);

    harness.emit({ type: 'progress', threadId: 'th_1', turnId: 'tn_1', text: '正在看代码' });

    const progress = harness.events.find((event) => event.type === 'task.progress');
    expect(progress?.payload.summary).toBe('正在看代码');
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
