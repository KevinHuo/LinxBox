/**
 * The Desktop's session with the local Codex app-server.
 *
 * One app-server process is kept alive for the lifetime of the Desktop and
 * reused across turns. The session owns nothing that leaves the machine: the
 * binary path, the provider's user agent and the provider's own output are
 * local facts.
 */
import { homedir } from 'node:os';

import { CodexClient, type CodexTurnState } from '../daemon/codex-adapter/client.js';
import type { SpawnFn } from '../daemon/codex-adapter/process.js';
import type { CodexDomainEvent } from '../daemon/codex-adapter/events.js';
import { discoverCodexBinary, type CodexBinary } from '../daemon/codex-adapter/discovery.js';
import type {
  CodexConversationView,
  CodexProbeView,
  CodexProjectView,
  CodexSessionDetailView,
  CodexSessionSummaryView,
  CodexTurnView,
} from '../shared/ipc.js';

export interface CodexSessionOptions {
  /** Override the discovered binary. Used by tests. */
  readonly binary?: CodexBinary | null;
  /** Override the process working directory. */
  readonly processCwd?: string;
  /** Override the process spawner. Used by tests. */
  readonly spawnFn?: SpawnFn;
}

const EMPTY_TURN_VIEW: CodexTurnView = {
  threadId: null,
  turnId: null,
  status: 'idle',
  progress: '',
  finalText: '',
  error: null,
  startedAt: null,
  completedAt: null,
};

function toTurnView(state: CodexTurnState): CodexTurnView {
  return {
    threadId: state.threadId,
    turnId: state.turnId,
    status: state.status,
    progress: state.progress,
    finalText: state.finalText,
    error: state.error,
    startedAt: state.startedAt,
    completedAt: state.completedAt,
  };
}

export class CodexSession {
  private readonly binaryOverride: CodexBinary | null | undefined;
  private readonly processCwd: string;
  private readonly spawnFn: SpawnFn | undefined;
  private readonly listeners = new Set<(event: CodexDomainEvent) => void>();
  private client: CodexClient | null = null;
  private connecting: Promise<CodexClient> | null = null;
  private userAgent: string | null = null;
  private authMode: string | null = null;
  private planType: string | null = null;

  constructor(options: CodexSessionOptions = {}) {
    this.binaryOverride = options.binary;
    this.processCwd = options.processCwd ?? homedir();
    this.spawnFn = options.spawnFn;
  }

  /**
   * Subscribe to provider events.
   *
   * The listener is registered on the session, not on the client, so it keeps
   * working across a provider restart.
   */
  on(listener: (event: CodexDomainEvent) => void): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /**
   * Report what the local provider looks like.
   *
   * The probe starts the app-server and performs the handshake, so it fails
   * loudly when the provider is missing or refuses to start.
   */
  async probe(): Promise<CodexProbeView> {
    const binary = this.resolveBinary();
    if (binary === null) {
      return {
        available: false,
        binaryPath: null,
        version: null,
        userAgent: null,
        authMode: null,
        planType: null,
        error: '未找到本机 Codex CLI',
      };
    }
    try {
      await this.connect();
      await this.waitForAccount();
      return {
        available: true,
        binaryPath: binary.path,
        version: binary.version,
        userAgent: this.userAgent,
        authMode: this.authMode,
        planType: this.planType,
        error: null,
      };
    } catch (error) {
      this.reset();
      return {
        available: false,
        binaryPath: binary.path,
        version: binary.version,
        userAgent: null,
        authMode: null,
        planType: null,
        error: error instanceof Error ? error.message : '本机 Codex 启动失败',
      };
    }
  }

  /**
   * Start a real turn in `cwd` and return immediately.
   *
   * The caller polls `status()` for the provider's progress and answer.
   */
  async startTurn(prompt: string, cwd: string): Promise<CodexTurnView> {
    const client = await this.connect();
    const threadId = await client.startThread(cwd);
    await client.startTurn(threadId, prompt, `linxbox-${Date.now().toString(36)}`);
    return this.status();
  }

  /** Create an idle thread owned by this app-server process. */
  async createSession(cwd: string): Promise<string> {
    const client = await this.connect();
    return client.startThread(cwd);
  }

  /** List the projects the provider knows about. */
  async listProjects(): Promise<CodexProjectView[]> {
    const client = await this.connect();
    const projects = await client.listProjects();
    return projects.map((project) => ({
      projectId: project.projectId,
      name: project.name,
      roots: project.roots,
      position: project.position,
    }));
  }

  /** Read one conversation with every detail entry. */
  async readConversation(threadId: string): Promise<CodexConversationView | null> {
    const client = await this.connect();
    const conversation = await client.readConversation(threadId);
    if (conversation === null) {
      return null;
    }
    return {
      threadId: conversation.threadId,
      title: conversation.title,
      cwd: conversation.cwd,
      turns: conversation.turns.map((turn) => ({
        turnId: turn.turnId,
        status: turn.status,
        startedAt: turn.startedAt,
        details: turn.details.map((detail) => ({
          kind: detail.kind,
          id: detail.id,
          title: detail.title,
          body: detail.body,
          meta: detail.meta,
        })),
      })),
    };
  }

  /**
   * List the sessions the provider knows about.
   *
   * This is a local read of the provider's own thread store. It does not
   * invoke a model, so it keeps working when the account has no quota.
   */
  async listSessions(limit = 30): Promise<CodexSessionSummaryView[]> {
    const client = await this.connect();
    const sessions = await client.listSessions(limit);
    return sessions.map((session) => ({
      threadId: session.threadId,
      title: session.title,
      cwd: session.cwd,
      model: session.model,
      createdAt: session.createdAt,
      updatedAt: session.updatedAt,
      status: session.status,
    }));
  }

  /** Read the transcript of one session. */
  async readSession(threadId: string): Promise<CodexSessionDetailView | null> {
    const client = await this.connect();
    const session = await client.readSession(threadId);
    if (session === null) {
      return null;
    }
    return {
      threadId: session.threadId,
      title: session.title,
      cwd: session.cwd,
      turns: session.turns.map((turn) => ({
        turnId: turn.turnId,
        status: turn.status,
        startedAt: turn.startedAt,
        messages: turn.messages.map((message) => ({
          id: message.id,
          role: message.role,
          text: message.text,
          phase: message.phase,
        })),
      })),
    };
  }

  /**
   * Continue an existing session with a new turn.
   *
   * The working directory comes from the provider's own record of the
   * session, never from the caller.
   */
  async continueSession(
    threadId: string,
    prompt: string,
    knownCwd?: string,
  ): Promise<CodexTurnView> {
    const client = await this.connect();
    let cwd = knownCwd;
    if (cwd === undefined) {
      const session = await client.readSession(threadId);
      if (session === null) {
        throw new Error('会话不存在');
      }
      if (session.cwd === null) {
        throw new Error('该会话没有工作目录，无法继续');
      }
      cwd = session.cwd;
    }
    await client.continueSession({
      threadId,
      prompt,
      cwd,
      clientMessageId: `linxbox-${Date.now().toString(36)}`,
    });
    return this.status();
  }

  /** The observable state of the current turn. */
  status(): CodexTurnView {
    return this.client === null ? EMPTY_TURN_VIEW : toTurnView(this.client.turnState);
  }

  /** Interrupt the running turn. */
  async interrupt(turnId: string): Promise<void> {
    const client = this.client;
    if (client === null) {
      return;
    }
    const state = client.turnState;
    if (state.threadId === null) {
      return;
    }
    await client.interruptTurn(state.threadId, turnId);
  }

  /** Stop the provider process. */
  close(): void {
    this.reset();
  }

  /**
   * Give the provider a moment to report the account.
   *
   * The `account/updated` notification arrives shortly after the handshake.
   * A provider that never reports an account is not an error: the Desktop
   * simply reports that it does not know the account.
   */
  private async waitForAccount(timeoutMs = 3000): Promise<void> {
    if (this.authMode !== null) {
      return;
    }
    const deadline = Date.now() + timeoutMs;
    while (this.authMode === null && Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }

  private resolveBinary(): CodexBinary | null {
    if (this.binaryOverride !== undefined) {
      return this.binaryOverride;
    }
    return discoverCodexBinary();
  }

  private async connect(): Promise<CodexClient> {
    if (this.client !== null) {
      return this.client;
    }
    if (this.connecting !== null) {
      return this.connecting;
    }
    const binary = this.resolveBinary();
    if (binary === null) {
      throw new Error('未找到本机 Codex CLI');
    }
    this.connecting = (async () => {
      const client = new CodexClient({
        binaryPath: binary.path,
        cwd: this.processCwd,
        ...(this.spawnFn === undefined ? {} : { spawnFn: this.spawnFn }),
      });
      client.on((event) => {
        if (event.type === 'account') {
          this.authMode = event.authMode;
          this.planType = event.planType;
        }
        for (const listener of this.listeners) {
          listener(event);
        }
      });
      const result = await client.initialize();
      this.userAgent = result.userAgent;
      this.client = client;
      this.connecting = null;
      return client;
    })();
    try {
      return await this.connecting;
    } catch (error) {
      this.connecting = null;
      throw error;
    }
  }

  private reset(): void {
    this.client?.close();
    this.client = null;
    this.connecting = null;
  }
}
