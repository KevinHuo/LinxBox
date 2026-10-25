import { describe, expect, it } from 'vitest';

import { TaskDispatcher, type DispatchedTaskEvent } from './dispatcher.js';
import type { CodexDomainEvent } from '../codex-adapter/events.js';
import type { GatewayFrame } from '../../contracts/index.js';

function harness() {
  const events: DispatchedTaskEvent[] = [];
  const listeners = new Set<(event: CodexDomainEvent) => void>();
  const dispatcher = new TaskDispatcher({
    defaultCwd: '/private/local-project',
    resolveAgentSession: () => ({
      agentSessionId: 'session',
      threadId: 'thread',
      projectId: 'project',
      rootPath: '/private/local-project',
    }),
    runner: {
      startTurn: () => Promise.reject(new Error('确认追问不能新建会话')),
      continueTurn: () => Promise.resolve({ threadId: 'thread', turnId: 'turn' }),
      sessionCwd: () => Promise.resolve('/private/local-project'),
      on(listener) {
        listeners.add(listener);
        return () => listeners.delete(listener);
      },
      status() {
        return {
          threadId: 'thread',
          turnId: 'turn',
          status: 'running',
          progress: '',
          finalText: '',
          error: null,
          startedAt: null,
          completedAt: null,
        };
      },
    },
    onEvent: (event) => events.push(event),
  });
  const frame: GatewayFrame = {
    protocol_version: '1.0',
    message_id: 'dispatch',
    sequence: 1,
    type: 'task.dispatch',
    sent_at: '2026-10-25T00:00:00Z',
    payload: {
      task_id: 'task',
      project_id: 'project',
      agent: 'codex',
      capabilities: [],
      prompt: '继续原会话',
      acceptance_criteria: [],
      policy_version: 'request-confirm/v1',
      continuation: { agent_session_id: 'session', thread_id: 'thread' },
    },
  };
  return {
    events,
    dispatcher,
    frame,
    emit(event: CodexDomainEvent) {
      for (const listener of listeners) listener(event);
    },
  };
}

describe('确认追问的结果事实', () => {
  it('只在 provider 明确完成后产生结构化本轮结果，不编造改动或验证', async () => {
    const h = harness();
    expect((await h.dispatcher.handle(h.frame)).disposition).toBe('accepted');
    h.emit({
      type: 'final',
      threadId: 'thread',
      turnId: 'turn',
      text: '私人正文 /private/local-project token=private',
    });
    expect(h.events.some((event) => event.type === 'task.completed')).toBe(false);
    h.emit({
      type: 'terminal',
      threadId: 'thread',
      turnId: 'turn',
      outcome: 'completed',
      error: null,
    });
    const completed = h.events.find((event) => event.type === 'task.completed');
    expect(completed?.payload.result).toMatchObject({
      contract_version: 'task-result-v1',
      status: 'succeeded',
      changes: [],
      verification: [],
      needs_user_action: null,
    });
    expect(completed?.payload.result?.risks[0]?.code).toBe('EXECUTION_DETAILS_NOT_PROJECTED');
    expect(JSON.stringify(completed)).not.toContain('/private/');
    expect(JSON.stringify(completed)).not.toContain('token=private');
    expect(completed?.payload.thread_id).toBe('thread');
    expect(completed?.payload.turn_id).toBe('turn');
    expect((await h.dispatcher.handle(h.frame)).disposition).toBe('duplicate');
  });

  it('带错误的 completed 不能假报成功', async () => {
    const h = harness();
    await h.dispatcher.handle(h.frame);
    h.emit({
      type: 'terminal',
      threadId: 'thread',
      turnId: 'turn',
      outcome: 'completed',
      error: 'provider facts conflict',
    });
    expect(h.events.some((event) => event.type === 'task.completed')).toBe(false);
    expect(h.events.at(-1)?.type).toBe('task.interrupted');
  });
});
