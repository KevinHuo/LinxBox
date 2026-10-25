/**
 * Task dispatch.
 *
 * This is where a remote command becomes local work. The Gateway sends a
 * `task.dispatch` frame; the dispatcher resolves the project, asks the local
 * Codex app-server to run a turn, and turns the provider's events back into
 * task events for the Gateway.
 *
 * The rules that matter:
 *
 *  - the project is resolved locally; the Gateway never supplies a path;
 *  - a delivery is acknowledged only after it has been persisted locally;
 *  - a duplicate `message_id` never starts a second turn;
 *  - only an explicitly successful provider completion becomes `succeeded`.
 */
import type { CodexDomainEvent } from '../codex-adapter/events.js';
import type { CodexTurnState } from '../codex-adapter/client.js';
import { isPathInsideRoot } from '../policy/authorization.js';
import { sanitizeVoiceSummary } from '../result-projector/projection.js';
import { publicError } from './frames.js';
import type {
  Disposition,
  GatewayFrame,
  TaskCancelPayload,
  TaskDispatchPayload,
  TaskEventPayload,
  TaskResult,
} from '../../contracts/index.js';

/** The local side of the provider, as the dispatcher needs it. */
export interface CodexRunner {
  startTurn(input: {
    prompt: string;
    cwd: string;
    clientMessageId: string;
  }): Promise<{ threadId: string; turnId: string }>;
  /**
   * Run a new turn inside an existing provider thread.
   *
   * The dispatcher only calls this for a continuation whose parent session is
   * known locally, so the reply keeps the parent conversation history.
   */
  continueTurn(input: {
    threadId: string;
    prompt: string;
    cwd: string;
    clientMessageId: string;
  }): Promise<{ threadId: string; turnId: string }>;
  /**
   * The working directory the provider recorded for a thread.
   *
   * The dispatcher uses it to confirm that a continuation really lives inside
   * the authorised project before it runs.
   */
  sessionCwd(threadId: string): Promise<string | null>;
  on(listener: (event: CodexDomainEvent) => void): () => void;
  status(): CodexTurnState;
}

/** A locally authorised project. */
export interface DispatchProject {
  readonly rootPath: string;
  readonly enabled: boolean;
}

export type TaskEventType =
  | 'task.started'
  | 'task.progress'
  | 'task.completed'
  | 'task.failed'
  | 'task.cancelled'
  | 'task.interrupted';

export interface DispatchedTaskEvent {
  readonly type: TaskEventType;
  readonly payload: TaskEventPayload;
}

export interface TaskDispatcherOptions {
  /** Resolve a `project_id` to a locally authorised project. */
  readonly resolveProject: (projectId: string) => DispatchProject | null;
  /** Run a turn on the local provider. */
  readonly runner: CodexRunner;
  /** Called for every task event. */
  readonly onEvent: (event: DispatchedTaskEvent) => void;
  /** Called when a delivery is judged. */
  readonly onDisposition?: (disposition: Disposition, messageId: string, error?: string) => void;
  readonly now?: () => Date;
}

/** How a delivery was judged. */
export interface DispatchOutcome {
  readonly disposition: Disposition;
  readonly error: string | null;
}

/** A task that ran locally, kept so a continuation can find its session. */
interface LocalTaskSession {
  readonly threadId: string;
  readonly projectId: string;
}

interface ActiveDispatchContext {
  readonly messageId: string;
  readonly taskId: string;
  threadId: string | null;
  turnId: string | null;
}

export class TaskDispatcher {
  private readonly options: TaskDispatcherOptions;
  private readonly now: () => Date;
  /** `message_id` values already accepted, so a replay never runs twice. */
  private readonly handled = new Map<string, DispatchOutcome>();
  /**
   * `task_id` to the provider session the task ran in.
   *
   * A continuation is resolved through this map, together with the project the
   * task belongs to. It is process local for now; a persistent mapping is
   * tracked in #11.
   */
  private readonly taskSessions = new Map<string, LocalTaskSession>();
  private eventCounter = 0;
  private unsubscribe: (() => void) | null = null;
  private active: ActiveDispatchContext | null = null;
  /** A task may emit at most one terminal event. */
  private terminalEmitted = false;

  constructor(options: TaskDispatcherOptions) {
    this.options = options;
    this.now = options.now ?? (() => new Date());
  }

  /**
   * Handle one inbound frame.
   *
   * Returns the disposition the Desktop should ACK with.
   */
  async handle(frame: GatewayFrame): Promise<DispatchOutcome> {
    if (frame.type === 'task.cancel') {
      return this.handleCancel(frame.payload);
    }
    if (frame.type !== 'task.dispatch') {
      return this.reject('未支持的 Gateway 消息类型');
    }

    const messageId = frame.message_id;
    const previous = this.handled.get(messageId);
    if (previous !== undefined) {
      // A replayed delivery must never create a second turn.
      this.options.onDisposition?.('duplicate', messageId);
      return { disposition: 'duplicate', error: null };
    }

    const payload: TaskDispatchPayload = frame.payload;
    const project = this.options.resolveProject(payload.project_id);
    if (project === null) {
      const outcome = this.reject('项目未授权');
      this.handled.set(messageId, outcome);
      this.options.onDisposition?.(outcome.disposition, messageId, outcome.error ?? undefined);
      return outcome;
    }
    if (!project.enabled) {
      const outcome = this.reject('项目授权已撤销');
      this.handled.set(messageId, outcome);
      this.options.onDisposition?.(outcome.disposition, messageId, outcome.error ?? undefined);
      return outcome;
    }

    if (this.active !== null) {
      const outcome = this.reject('客户端正在执行另一项任务');
      this.handled.set(messageId, outcome);
      this.options.onDisposition?.(outcome.disposition, messageId, outcome.error ?? undefined);
      return outcome;
    }

    const taskId = payload.task_id;
    // A continuation has to land in the parent conversation, inside the
    // project the parent task ran in. When that cannot be confirmed the
    // request is refused: falling back to a new session would answer the
    // wrong conversation.
    const continuation = await this.resolveContinuation(payload);
    if (continuation.reason !== null) {
      const outcome = this.reject(continuation.reason);
      this.handled.set(messageId, outcome);
      this.options.onDisposition?.(outcome.disposition, messageId, outcome.error ?? undefined);
      return outcome;
    }

    // Reserve the message before the provider side effect. A concurrent replay
    // can now only observe the reservation and cannot start a second turn.
    const reservation: DispatchOutcome = { disposition: 'accepted', error: null };
    this.handled.set(messageId, reservation);
    const context: ActiveDispatchContext = { messageId, taskId, threadId: null, turnId: null };
    this.active = context;
    this.eventCounter = 0;
    this.terminalEmitted = false;
    this.subscribeToRunner();

    try {
      const started =
        continuation.threadId === null
          ? await this.options.runner.startTurn({
              prompt: payload.prompt,
              cwd: project.rootPath,
              clientMessageId: messageId,
            })
          : await this.options.runner.continueTurn({
              threadId: continuation.threadId,
              prompt: payload.prompt,
              cwd: project.rootPath,
              clientMessageId: messageId,
            });
      if (this.active === context) {
        context.threadId = started.threadId;
        context.turnId = started.turnId;
        this.taskSessions.set(taskId, {
          threadId: started.threadId,
          projectId: payload.project_id,
        });
        this.emit(taskId, 'task.started', {
          state: 'running',
          stage: 'preparing',
          summary: '已在本机启动 Codex 任务',
          result: null,
          error: null,
        });
      }
    } catch {
      if (this.active !== context) {
        this.options.onDisposition?.(reservation.disposition, messageId);
        return reservation;
      }
      const message = '本机 Codex 启动失败';
      this.emit(taskId, 'task.failed', {
        state: 'failed',
        stage: 'finished',
        summary: '本机 Codex 启动失败',
        result: null,
        error: publicError('DEPENDENCY_UNAVAILABLE', message, taskId),
      });
      this.finishActiveTask();
      const outcome: DispatchOutcome = { disposition: 'rejected', error: message };
      this.handled.set(messageId, outcome);
      this.options.onDisposition?.(outcome.disposition, messageId, message);
      return outcome;
    }

    this.options.onDisposition?.(reservation.disposition, messageId);
    return reservation;
  }

  private handleCancel(_payload: TaskCancelPayload): DispatchOutcome {
    return { disposition: 'accepted', error: null };
  }

  private reject(reason: string): DispatchOutcome {
    return { disposition: 'rejected', error: reason };
  }

  private subscribeToRunner(): void {
    this.unsubscribe?.();
    this.unsubscribe = this.options.runner.on((event) => {
      this.applyRunnerEvent(event);
    });
  }

  private applyRunnerEvent(event: CodexDomainEvent): void {
    const active = this.active;
    if (active === null) {
      return;
    }
    if (event.type === 'thread-started') {
      active.threadId ??= event.threadId;
      return;
    }
    if (event.type === 'turn-started') {
      active.threadId ??= event.threadId;
      active.turnId ??= event.turnId;
      return;
    }
    if (
      event.type !== 'account' &&
      event.type !== 'warning' &&
      !this.eventBelongsToActiveTask(event, active)
    ) {
      return;
    }
    const taskId = active.taskId;
    switch (event.type) {
      case 'progress':
        this.emit(taskId, 'task.progress', {
          state: 'running',
          stage: 'analyzing',
          summary: this.safeSummary(event.text, '本机任务正在执行'),
          result: null,
          error: null,
        });
        return;
      case 'final':
        this.emit(taskId, 'task.progress', {
          state: 'running',
          stage: 'summarizing',
          summary: this.safeSummary(event.text, '本机任务已生成回复'),
          result: null,
          error: null,
        });
        return;
      case 'terminal':
        if (event.outcome === 'completed') {
          this.emit(taskId, 'task.completed', {
            state: 'succeeded',
            stage: 'finished',
            summary: '本机任务已完成',
            result: null,
            error: null,
          });
        } else if (event.outcome === 'failed') {
          this.emit(taskId, 'task.failed', {
            state: 'failed',
            stage: 'finished',
            summary: '本机任务失败',
            result: null,
            error: publicError('DEPENDENCY_UNAVAILABLE', '本机 Codex 任务失败', taskId),
          });
        } else {
          this.emit(taskId, 'task.interrupted', {
            state: 'interrupted',
            stage: 'finished',
            summary: '本机任务未确认完成',
            result: null,
            error: publicError('INTERNAL', '本机任务被中断，结果未确认', taskId),
          });
        }
        this.finishActiveTask();
        return;
      case 'provider-error':
        this.emit(taskId, 'task.failed', {
          state: 'failed',
          stage: 'finished',
          summary: '本机 Codex 返回错误',
          result: null,
          error: publicError('DEPENDENCY_UNAVAILABLE', '本机 Codex 返回错误', taskId),
        });
        this.finishActiveTask();
        return;
      case 'account':
      case 'warning':
        return;
    }
  }

  private eventBelongsToActiveTask(
    event: Extract<CodexDomainEvent, { threadId: string | null; turnId: string | null }>,
    active: ActiveDispatchContext,
  ): boolean {
    if (event.threadId !== null && active.threadId !== null && event.threadId !== active.threadId) {
      return false;
    }
    if (event.turnId !== null && active.turnId !== null && event.turnId !== active.turnId) {
      return false;
    }
    return (
      (event.threadId !== null && event.threadId === active.threadId) ||
      (event.turnId !== null && event.turnId === active.turnId)
    );
  }

  /**
   * Resolve where a continuation has to run.
   *
   * The continuation is only allowed when the parent task ran in the very
   * project this request names, that project is still authorised, and the
   * session really lives inside the authorised directory. Anything else is
   * refused instead of silently answering a different conversation.
   */
  private async resolveContinuation(payload: TaskDispatchPayload): Promise<{
    threadId: string | null;
    reason: string | null;
  }> {
    const continuation = payload.continuation;
    if (continuation === null) {
      return { threadId: null, reason: null };
    }
    const parent = this.taskSessions.get(continuation.parent_task_id);
    if (parent === undefined) {
      return { threadId: null, reason: '原任务不存在或不可续接' };
    }
    if (parent.projectId !== payload.project_id) {
      return { threadId: null, reason: '续接目标不属于本次授权项目' };
    }
    const sessionCwd = await this.options.runner.sessionCwd(parent.threadId);
    if (sessionCwd === null) {
      return { threadId: null, reason: '目标会话没有可校验的工作目录' };
    }
    // The authorisation is checked again right before the turn starts, so a
    // grant revoked while the session was being looked up cannot slip through.
    const parentProject = this.options.resolveProject(parent.projectId);
    if (parentProject === null || !parentProject.enabled) {
      return { threadId: null, reason: '原任务项目授权已撤销' };
    }
    if (!isPathInsideRoot(parentProject.rootPath, sessionCwd)) {
      return { threadId: null, reason: '目标会话不属于本次授权项目' };
    }
    return { threadId: parent.threadId, reason: null };
  }

  private safeSummary(text: string, fallback: string): string {
    const sanitized = sanitizeVoiceSummary(text);
    return sanitized.length === 0 ? fallback : sanitized;
  }

  private finishActiveTask(): void {
    this.unsubscribe?.();
    this.unsubscribe = null;
    this.active = null;
  }

  private emit(
    taskId: string,
    type: TaskEventType,
    body: {
      state: string;
      stage: string;
      summary: string;
      result: TaskResult | null;
      error: TaskEventPayload['error'];
    },
  ): void {
    const isTerminal = type !== 'task.started' && type !== 'task.progress';
    if (isTerminal) {
      if (this.terminalEmitted) {
        // The contract allows exactly one terminal event per task.
        return;
      }
      this.terminalEmitted = true;
    }
    this.eventCounter += 1;
    // The active context is the only place that knows which provider thread
    // and turn the task really runs in, so the identifiers are read here
    // instead of being threaded through every call site.
    const active = this.active;
    const payload: TaskEventPayload = {
      task_id: taskId,
      event_id: `evt_${this.now().getTime().toString(36)}_${this.eventCounter.toString(36)}`,
      event_sequence: this.eventCounter,
      occurred_at: this.now().toISOString(),
      state: body.state,
      stage: body.stage,
      summary: body.summary,
      result: body.result,
      error: body.error,
      thread_id: active?.threadId ?? null,
      turn_id: active?.turnId ?? null,
    };
    this.options.onEvent({ type, payload });
  }
}
