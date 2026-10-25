import { describe, expect, it, vi } from 'vitest';
import {
  TaskDispatcher,
  type CodexRunner,
  type DispatchedTaskEvent,
  type TaskDispatcherOptions,
} from './dispatcher.js';
import type { CodexDomainEvent } from '../codex-adapter/events.js';
import type { GatewayFrame } from '../../contracts/index.js';

const frame = (continuation: unknown = null, messageId = 'dispatch'): GatewayFrame =>
  ({
    protocol_version: '1.0',
    message_id: messageId,
    sequence: 1,
    type: 'task.dispatch',
    sent_at: '2026-10-26T00:00:00Z',
    payload: {
      task_id: 'task',
      project_id: 'project',
      agent: 'codex',
      capabilities: [],
      prompt: '继续',
      acceptance_criteria: [],
      policy_version: 'test',
      continuation,
    },
  }) as GatewayFrame;
const cancel = (taskId = 'task'): GatewayFrame => ({
  ...frame(),
  type: 'task.cancel',
  payload: { task_id: taskId, reason: '用户取消' },
});

function harness(overrides: Partial<TaskDispatcherOptions> = {}) {
  let listener: (event: CodexDomainEvent) => void = () => undefined;
  const runner = {
    startTurn: vi
      .fn<CodexRunner['startTurn']>()
      .mockResolvedValue({ threadId: 'thread', turnId: 'turn' }),
    continueTurn: vi
      .fn<CodexRunner['continueTurn']>()
      .mockResolvedValue({ threadId: 'thread', turnId: 'turn' }),
    sessionCwd: vi.fn<CodexRunner['sessionCwd']>().mockResolvedValue('/tmp/provider'),
    interrupt: vi.fn<(turnId: string) => Promise<void>>().mockResolvedValue(undefined),
    on: (handler: (event: CodexDomainEvent) => void) => {
      listener = handler;
      return () => undefined;
    },
    status: () => ({
      threadId: null,
      turnId: null,
      status: 'idle' as const,
      progress: '',
      finalText: '',
      error: null,
      startedAt: null,
      completedAt: null,
    }),
  };
  const events: DispatchedTaskEvent[] = [];
  const disposition = vi.fn();
  const dispatcher = new TaskDispatcher({
    runner,
    defaultCwd: '/tmp/default',
    onEvent: (event) => events.push(event),
    onDisposition: disposition,
    ...overrides,
  });
  return {
    dispatcher,
    runner,
    events,
    disposition,
    emit: (event: CodexDomainEvent) => listener(event),
  };
}

describe('分发边界与 provider 迟到事实', () => {
  it.each([null, ''])('Codex 无法解析未发布 thread 时拒绝，不能回落到新任务：%s', async (cwd) => {
    const h = harness();
    h.runner.sessionCwd.mockResolvedValue(cwd);
    const result = await h.dispatcher.handle(
      frame({ agent_session_id: 'unknown', thread_id: 'old-thread' }),
    );
    expect(result).toEqual({ disposition: 'rejected', error: '目标会话不存在或不可续接' });
    expect(h.runner.startTurn).not.toHaveBeenCalled();
    expect(h.runner.continueTurn).not.toHaveBeenCalled();
    expect(h.disposition).toHaveBeenCalledWith('rejected', 'dispatch', result.error);
  });
  it('仅使用本机 Codex 返回的会话目录，不从远端 ID 推导路径', async () => {
    const h = harness();
    expect(
      (await h.dispatcher.handle(frame({ agent_session_id: 'unknown', thread_id: 'old-thread' })))
        .disposition,
    ).toBe('accepted');
    expect(h.runner.sessionCwd).toHaveBeenCalledWith('old-thread');
    expect(h.runner.continueTurn).toHaveBeenCalledWith({
      threadId: 'old-thread',
      cwd: '/tmp/provider',
      prompt: '继续',
      clientMessageId: 'dispatch',
    });
    expect(h.runner.startTurn).not.toHaveBeenCalled();
  });
  it('已发布会话仍须匹配本次目标项目', async () => {
    const h = harness({
      resolveAgentSession: () => ({
        agentSessionId: 'session',
        threadId: 'thread',
        projectId: 'other',
        rootPath: '/tmp/other',
      }),
    });
    expect(
      (await h.dispatcher.handle(frame({ agent_session_id: 'session', thread_id: 'thread' })))
        .disposition,
    ).toBe('rejected');
    expect(h.runner.continueTurn).not.toHaveBeenCalled();
  });
  it('空父任务目录不续接，重复拒绝也不触发 provider 副作用', async () => {
    const h = harness();
    await h.dispatcher.handle(frame());
    h.emit({
      type: 'terminal',
      threadId: 'thread',
      turnId: 'turn',
      outcome: 'completed',
      error: null,
    });
    h.runner.sessionCwd.mockResolvedValue('');
    const next = frame({ parent_task_id: 'task' }, 'next');
    expect((await h.dispatcher.handle(next)).error).toBe('目标会话没有可校验的工作目录');
    expect((await h.dispatcher.handle(next)).disposition).toBe('duplicate');
    expect(h.runner.continueTurn).not.toHaveBeenCalled();
  });
  it.each(['error-object', 'non-error'] as const)(
    '续接启动失败报告真实失败而非成功：%s',
    async (mode) => {
      const h = harness();
      h.runner.continueTurn.mockRejectedValue(
        mode === 'error-object' ? new Error('无法续接') : '非 Error',
      );
      const result = await h.dispatcher.handle(
        frame({ agent_session_id: 'session', thread_id: 'thread' }),
      );
      expect(result.disposition).toBe('rejected');
      expect(result.error).toContain('目标 Codex 会话续接失败');
      expect(h.events.at(-1)?.type).toBe('task.failed');
      expect(h.events.some((event) => event.type === 'task.completed')).toBe(false);
    },
  );
  it('取消只针对已启动的原任务，provider 拒绝中断不伪造取消终态', async () => {
    const h = harness();
    await h.dispatcher.handle(frame());
    expect((await h.dispatcher.handle(cancel('other'))).disposition).toBe('rejected');
    h.runner.interrupt.mockRejectedValue(new Error('尚未中断'));
    expect((await h.dispatcher.handle(cancel())).disposition).toBe('accepted');
    await Promise.resolve();
    expect(h.runner.interrupt).toHaveBeenCalledWith('turn');
    expect(h.events.some((event) => event.type === 'task.cancelled')).toBe(false);
  });
  it('未提供中断能力时明确拒绝取消', async () => {
    const base = harness();
    const { interrupt: _interrupt, ...runner } = base.runner;
    const h = harness({ runner });
    await h.dispatcher.handle(frame());
    expect(await h.dispatcher.handle(cancel())).toEqual({
      disposition: 'rejected',
      error: '本机 Codex 不支持取消',
    });
  });
  it('忽略其他 turn、无身份和终态后的迟到事件', async () => {
    const h = harness();
    await h.dispatcher.handle(frame());
    const count = h.events.length;
    h.emit({ type: 'progress', threadId: 'thread', turnId: 'other', text: '串线' });
    h.emit({ type: 'progress', threadId: null, turnId: null, text: '无归属' });
    expect(h.events).toHaveLength(count);
    h.emit({ type: 'progress', threadId: null, turnId: 'turn', text: '' });
    expect(h.events.at(-1)?.payload.summary).toBe('本机任务正在执行');
    h.emit({
      type: 'terminal',
      threadId: 'thread',
      turnId: 'turn',
      outcome: 'completed',
      error: null,
    });
    const ended = h.events.length;
    h.emit({ type: 'progress', threadId: 'thread', turnId: 'turn', text: '迟到' });
    expect(h.events).toHaveLength(ended);
  });
  it('provider 在启动 Promise 完成前报告终态，不再追加 started 或重发终态', async () => {
    const h = harness();
    let reject!: (error: unknown) => void;
    h.runner.startTurn.mockReturnValue(
      new Promise((_resolve, fail) => {
        reject = fail;
      }),
    );
    const pending = h.dispatcher.handle(frame());
    await Promise.resolve();
    h.emit({ type: 'thread-started', threadId: 'thread' });
    h.emit({ type: 'turn-started', threadId: 'thread', turnId: 'turn' });
    h.emit({
      type: 'terminal',
      threadId: 'thread',
      turnId: 'turn',
      outcome: 'failed',
      error: '已失败',
    });
    reject(new Error('迟到的启动失败'));
    await pending;
    expect(h.events.map((event) => event.type)).toEqual(['task.failed']);
  });
});
