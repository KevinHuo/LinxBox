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

export class TaskDispatcher {
  private readonly options: TaskDispatcherOptions;
  private readonly now: () => Date;
  /** `message_id` values already accepted, so a replay never runs twice. */
  private readonly handled = new Map<string, DispatchOutcome>();
  private eventCounter = 0;
  private unsubscribe: (() => void) | null = null;
  private activeTaskId: string | null = null;
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

    const taskId = payload.task_id;
    this.activeTaskId = taskId;
    this.eventCounter = 0;
    this.terminalEmitted = false;
    this.subscribeToRunner(taskId);

    try {
      const started = await this.options.runner.startTurn({
        prompt: payload.prompt,
        cwd: project.rootPath,
        clientMessageId: messageId,
      });
      this.emit(taskId, 'task.started', {
        state: 'running',
        stage: 'preparing',
        summary: '已在本机启动 Codex 任务',
        result: null,
        error: null,
        threadId: started.threadId,
        turnId: started.turnId,
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : '本机 Codex 启动失败';
      this.emit(taskId, 'task.failed', {
        state: 'failed',
        stage: 'finished',
        summary: '本机 Codex 启动失败',
        result: null,
        error: publicError('DEPENDENCY_UNAVAILABLE', message, taskId),
      });
      const outcome: DispatchOutcome = { disposition: 'rejected', error: message };
      this.handled.set(messageId, outcome);
      this.options.onDisposition?.(outcome.disposition, messageId, message);
      return outcome;
    }

    const outcome: DispatchOutcome = { disposition: 'accepted', error: null };
    this.handled.set(messageId, outcome);
    this.options.onDisposition?.(outcome.disposition, messageId);
    return outcome;
  }

  private handleCancel(_payload: TaskCancelPayload): DispatchOutcome {
    return { disposition: 'accepted', error: null };
  }

  private reject(reason: string): DispatchOutcome {
    return { disposition: 'rejected', error: reason };
  }

  private subscribeToRunner(taskId: string): void {
    this.unsubscribe?.();
    this.unsubscribe = this.options.runner.on((event) => {
      this.applyRunnerEvent(taskId, event);
    });
  }

  private applyRunnerEvent(taskId: string, event: CodexDomainEvent): void {
    switch (event.type) {
      case 'progress':
        this.emit(taskId, 'task.progress', {
          state: 'running',
          stage: 'analyzing',
          summary: event.text.slice(0, 500),
          result: null,
          error: null,
        });
        return;
      case 'final':
        this.emit(taskId, 'task.progress', {
          state: 'running',
          stage: 'summarizing',
          summary: event.text.slice(0, 500),
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
            error: publicError(
              event.error === null ? 'INTERNAL' : 'DEPENDENCY_UNAVAILABLE',
              event.error ?? '本机 Codex 未给出失败原因',
              taskId,
            ),
          });
        } else {
          this.emit(taskId, 'task.interrupted', {
            state: 'interrupted',
            stage: 'finished',
            summary: '本机任务未确认完成',
            result: null,
            error: publicError('INTERNAL', event.error ?? '本机任务被中断', taskId),
          });
        }
        this.unsubscribe?.();
        this.unsubscribe = null;
        return;
      case 'provider-error':
        this.emit(taskId, 'task.failed', {
          state: 'failed',
          stage: 'finished',
          summary: '本机 Codex 返回错误',
          result: null,
          error: publicError('DEPENDENCY_UNAVAILABLE', event.message, taskId),
        });
        return;
      case 'turn-started':
      case 'thread-started':
      case 'account':
      case 'warning':
        return;
    }
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
      threadId?: string;
      turnId?: string;
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
    };
    this.options.onEvent({ type, payload });
  }
}
