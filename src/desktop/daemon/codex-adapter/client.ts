/**
 * High level Codex app-server client.
 *
 * Wraps the stdio transport with the four operations the Desktop actually
 * needs - initialize, thread start/resume, turn start/interrupt - and keeps
 * the observable state of the current turn so the UI can display real
 * provider content.
 */
import {
  CodexAdapterError,
  CodexProcess,
  buildChildEnv,
  type CodexProcessEvent,
  type SpawnFn,
} from './process.js';
import {
  CODEX_METHODS,
  configReadResultSchema,
  initializeResultSchema,
  threadResumeResultSchema,
  threadStartResultSchema,
  turnStartResultSchema,
  type InitializeResult,
} from './protocol.js';
import { mapNotification, type CodexDomainEvent } from './events.js';
import {
  normalizeProjectList,
  normalizeSessionDetail,
  normalizeSessionList,
  normalizeTurnDetails,
  type CodexConversation,
  type CodexProjectSummary,
  type CodexSessionDetail,
  type CodexSessionSummary,
} from './history.js';
import { appServerArgs } from './discovery.js';

/** Observable state of the turn the Desktop is currently watching. */
export interface CodexTurnState {
  readonly threadId: string | null;
  readonly turnId: string | null;
  readonly status: 'idle' | 'running' | 'completed' | 'failed' | 'interrupted';
  /** Latest provider commentary, shown as live progress. */
  readonly progress: string;
  /** Accumulated final answer text. */
  readonly finalText: string;
  readonly error: string | null;
  readonly startedAt: string | null;
  readonly completedAt: string | null;
}

export interface CodexAccount {
  readonly authMode: string | null;
  readonly planType: string | null;
}

export interface CodexClientOptions {
  readonly binaryPath: string;
  readonly cwd: string;
  readonly env?: Readonly<Record<string, string>>;
  readonly requestTimeoutMs?: number;
  readonly turnTimeoutMs?: number;
  readonly spawnFn?: SpawnFn;
  readonly now?: () => Date;
}

export type CodexClientListener = (event: CodexDomainEvent) => void;

const EMPTY_TURN_STATE: CodexTurnState = {
  threadId: null,
  turnId: null,
  status: 'idle',
  progress: '',
  finalText: '',
  error: null,
  startedAt: null,
  completedAt: null,
};

/**
 * A client for one local Codex app-server.
 */
export class CodexClient {
  private readonly process: CodexProcess;
  private readonly listeners = new Set<CodexClientListener>();
  private readonly turnTimeoutMs: number;
  private readonly now: () => Date;
  private state: CodexTurnState = EMPTY_TURN_STATE;
  private account: CodexAccount = { authMode: null, planType: null };
  private initialized = false;

  constructor(options: CodexClientOptions) {
    this.turnTimeoutMs = options.turnTimeoutMs ?? 10 * 60_000;
    this.now = options.now ?? (() => new Date());
    this.process = new CodexProcess({
      binaryPath: options.binaryPath,
      args: appServerArgs(),
      cwd: options.cwd,
      ...(options.env === undefined ? {} : { env: options.env }),
      ...(options.requestTimeoutMs === undefined
        ? {}
        : { requestTimeoutMs: options.requestTimeoutMs }),
      ...(options.spawnFn === undefined ? {} : { spawnFn: options.spawnFn }),
    });

    this.process.on((event) => {
      this.handleProcessEvent(event);
    });
  }

  /** Subscribe to domain events. Returns an unsubscribe function. */
  on(listener: CodexClientListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /** The observable state of the current turn. */
  get turnState(): CodexTurnState {
    return this.state;
  }

  /** The account the provider reported, when it reported one. */
  get accountState(): CodexAccount {
    return this.account;
  }

  /** Recent provider stderr lines, kept locally for diagnostics. */
  get stderrLines(): readonly string[] {
    return this.process.stderrLines;
  }

  /**
   * Perform the provider handshake.
   *
   * The app-server expects `initialize` followed by the `initialized`
   * notification before any other call.
   */
  async initialize(): Promise<InitializeResult> {
    const raw = await this.process.call(CODEX_METHODS.initialize, {
      clientInfo: { name: 'linxbox_desktop', title: 'LinxBox Desktop', version: '0.1.0' },
      capabilities: { experimentalApi: true },
    });
    const parsed = initializeResultSchema.safeParse(raw);
    if (!parsed.success) {
      throw new CodexAdapterError('CODEX_PROTOCOL', '本机 Codex 返回了无法识别的初始化结果');
    }
    this.process.notify(CODEX_METHODS.initialized, {});
    this.initialized = true;
    return parsed.data;
  }

  /**
   * Read the provider's default model.
   *
   * The Desktop must not hard-code a model: the account's plan decides which
   * models are available.
   */
  async readDefaultModel(cwd: string): Promise<string | null> {
    const raw = await this.process.call(CODEX_METHODS.configRead, { cwd, includeLayers: false });
    const parsed = configReadResultSchema.safeParse(raw);
    if (!parsed.success) {
      return null;
    }
    return parsed.data.config.model ?? null;
  }

  /** Start a new thread rooted at `cwd`. Returns the thread id. */
  async startThread(cwd: string): Promise<string> {
    this.assertInitialized();
    const raw = await this.process.call(CODEX_METHODS.threadStart, { cwd });
    const parsed = threadStartResultSchema.safeParse(raw);
    if (!parsed.success) {
      throw new CodexAdapterError('CODEX_PROTOCOL', '本机 Codex 返回了无法识别的 thread 结果');
    }
    return parsed.data.thread.id;
  }

  /** Resume an existing thread. Returns the thread id. */
  async resumeThread(threadId: string, cwd: string): Promise<string> {
    this.assertInitialized();
    const raw = await this.process.call(CODEX_METHODS.threadResume, {
      threadId,
      cwd,
      excludeTurns: true,
    });
    const parsed = threadResumeResultSchema.safeParse(raw);
    if (!parsed.success) {
      throw new CodexAdapterError('CODEX_PROTOCOL', '本机 Codex 返回了无法识别的 thread 结果');
    }
    return parsed.data.thread.id;
  }

  /**
   * Start a turn.
   *
   * The returned turn id identifies the run. The final answer arrives
   * asynchronously as domain events.
   */
  async startTurn(threadId: string, text: string, clientMessageId: string): Promise<string> {
    this.assertInitialized();
    this.state = {
      ...EMPTY_TURN_STATE,
      threadId,
      status: 'running',
      startedAt: this.now().toISOString(),
    };
    const raw = await this.process.call(CODEX_METHODS.turnStart, {
      threadId,
      input: [{ type: 'text', text }],
      clientUserMessageId: clientMessageId,
    });
    const parsed = turnStartResultSchema.safeParse(raw);
    if (!parsed.success) {
      this.state = { ...this.state, status: 'failed' };
      throw new CodexAdapterError('CODEX_PROTOCOL', '本机 Codex 返回了无法识别的 turn 结果');
    }
    this.state = { ...this.state, turnId: parsed.data.turn.id };
    return parsed.data.turn.id;
  }

  /** Ask the provider to interrupt a running turn. */
  async interruptTurn(threadId: string, turnId: string): Promise<void> {
    this.assertInitialized();
    await this.process.call(CODEX_METHODS.turnInterrupt, { threadId, turnId });
  }

  /**
   * Continue an existing session with a new turn.
   *
   * The thread is resumed first so the provider keeps its own history, then a
   * new turn is started inside it. The reply comes back through the normal
   * domain events.
   */
  async continueSession(input: {
    threadId: string;
    prompt: string;
    cwd: string;
    clientMessageId: string;
  }): Promise<{ threadId: string; turnId: string }> {
    this.assertInitialized();
    const resumed = await this.resumeThread(input.threadId, input.cwd);
    const turnId = await this.startTurn(resumed, input.prompt, input.clientMessageId);
    return { threadId: resumed, turnId };
  }

  /**
   * List the sessions the provider knows about.
   *
   * This is a local read of the provider's own thread store. It does not
   * invoke a model, so it works even when the account has no quota left.
   */
  async listSessions(limit = 30): Promise<CodexSessionSummary[]> {
    this.assertInitialized();
    const raw = await this.process.call(CODEX_METHODS.threadList, { limit });
    return normalizeSessionList(raw);
  }

  /**
   * List the projects the provider knows about.
   *
   * A project is the provider's own grouping of working directories.
   */
  async listProjects(): Promise<CodexProjectSummary[]> {
    this.assertInitialized();
    const raw = await this.process.call(CODEX_METHODS.projectList, {});
    return normalizeProjectList(raw);
  }

  /**
   * Read one conversation with every detail entry.
   *
   * The turn list is fetched with full items so command executions, file
   * changes and tool calls are readable, not just the chat messages.
   */
  async readConversation(threadId: string, turnLimit = 50): Promise<CodexConversation | null> {
    this.assertInitialized();
    const meta = await this.process.call(CODEX_METHODS.threadRead, {
      threadId,
      includeTurns: true,
    });
    const parsed = normalizeSessionDetail(meta);
    if (parsed === null) {
      // The provider does not know this thread.
      return null;
    }
    const turns = await this.process.call(CODEX_METHODS.threadTurnsList, {
      threadId,
      limit: turnLimit,
      sortDirection: 'asc',
      itemsView: 'full',
    });
    const raw = turns as { data?: unknown };
    const data = Array.isArray(raw.data) ? raw.data : [];
    return {
      threadId,
      title: parsed.title,
      cwd: parsed.cwd,
      updatedAt: null,
      turns: data.map((turn) => {
        const record = turn as { id?: unknown; status?: unknown; startedAt?: unknown };
        return {
          turnId: typeof record.id === 'string' ? record.id : '',
          status: typeof record.status === 'string' ? record.status : 'unknown',
          startedAt:
            typeof record.startedAt === 'number'
              ? new Date(record.startedAt * 1000).toISOString()
              : null,
          details: normalizeTurnDetails(turn),
        };
      }),
    };
  }

  /**
   * Read the transcript of one session.
   *
   * `includeTurns` asks the provider for the turn list in the same call, so
   * the transcript and the metadata cannot drift apart.
   */
  async readSession(threadId: string): Promise<CodexSessionDetail | null> {
    this.assertInitialized();
    const raw = await this.process.call(CODEX_METHODS.threadRead, {
      threadId,
      includeTurns: true,
    });
    return normalizeSessionDetail(raw);
  }

  /**
   * Wait until the given turn reaches a terminal state.
   *
   * A turn that never reports a terminal state is treated as `interrupted`:
   * the result cannot be proven.
   */
  async waitForTerminal(turnId: string, timeoutMs?: number): Promise<CodexTurnState> {
    if (
      this.state.turnId === turnId &&
      this.state.status !== 'running' &&
      this.state.status !== 'idle'
    ) {
      return this.state;
    }
    return new Promise<CodexTurnState>((resolve) => {
      let settled = false;
      const timer = setTimeout(() => {
        if (settled) {
          return;
        }
        settled = true;
        unsubscribe();
        this.state = {
          ...this.state,
          status: 'interrupted',
          error: '本机 Codex 未在超时时间内返回终态',
          completedAt: this.now().toISOString(),
        };
        resolve(this.state);
      }, timeoutMs ?? this.turnTimeoutMs);
      const unsubscribe = this.on((event) => {
        if (event.type !== 'terminal') {
          return;
        }
        if (event.turnId !== null && event.turnId !== turnId) {
          return;
        }
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);
        unsubscribe();
        resolve(this.state);
      });
    });
  }

  /** Stop the provider process. */
  close(): void {
    this.process.close();
  }

  private assertInitialized(): void {
    if (!this.initialized) {
      throw new CodexAdapterError('CODEX_NOT_INITIALIZED', '本机 Codex 尚未初始化');
    }
  }

  private handleProcessEvent(event: CodexProcessEvent): void {
    if (event.type === 'notification') {
      const domain = mapNotification(event.method, event.params);
      if (domain !== null) {
        this.applyDomainEvent(domain);
      }
      return;
    }
    if (event.type === 'exit' || event.type === 'error') {
      if (this.state.status === 'running') {
        this.state = {
          ...this.state,
          status: 'interrupted',
          error: '本机 Codex 进程已退出，无法确认任务结果',
          completedAt: this.now().toISOString(),
        };
      }
    }
  }

  private applyDomainEvent(event: CodexDomainEvent): void {
    switch (event.type) {
      case 'progress':
        this.state = { ...this.state, progress: event.text };
        break;
      case 'final':
        this.state = {
          ...this.state,
          finalText:
            this.state.finalText.length === 0
              ? event.text
              : `${this.state.finalText}\n\n${event.text}`,
        };
        break;
      case 'terminal':
        this.state = {
          ...this.state,
          status: event.outcome,
          error: event.error ?? this.state.error,
          completedAt: this.now().toISOString(),
        };
        break;
      case 'provider-error':
        this.state = { ...this.state, error: event.message };
        break;
      case 'account':
        this.account = { authMode: event.authMode, planType: event.planType };
        break;
      case 'thread-started':
      case 'turn-started':
      case 'warning':
        break;
    }
    for (const listener of this.listeners) {
      listener(event);
    }
  }
}

/** Convenience: build the default child environment for the provider. */
export function defaultCodexEnv(source: NodeJS.ProcessEnv = process.env): Record<string, string> {
  return buildChildEnv(source);
}
