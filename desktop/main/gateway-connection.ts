/**
 * Gateway connection manager for the Electron main process.
 *
 * This is the wiring layer between the pure daemon modules and Electron. It
 * owns exactly one outbound Gateway connection:
 *
 *  - the `GatewayClient` dials out and reconnects on its own;
 *  - the `TaskDispatcher` turns inbound dispatches into local provider runs;
 *  - every task event goes back to the Gateway as a frame.
 *
 * The module holds no protocol logic of its own: the frames are built by the
 * daemon layer, and the local project grants stay on this machine.
 */
import { GatewayClient, type SocketFactory } from '../daemon/gateway-client/client.js';
import { TaskDispatcher, type CodexRunner, type DispatchedTaskEvent } from '../daemon/gateway-client/dispatcher.js';
import { homedir } from 'node:os';
import {
  buildAgentSessionsSyncFrame,
  buildDeviceBindFrame,
  buildMessageAvailableFrame,
  buildQueryResultFrame,
  buildTaskEventFrame,
  publicError,
} from '../daemon/gateway-client/frames.js';
import type { CodexDomainEvent } from '../daemon/codex-adapter/events.js';
import type { AgentSessionCatalogItem, Disposition, GatewayFrame, ProjectCapability } from '../contracts/index.js';
import type { CodexSession } from './codex-session.js';
import { readUtf8Chunk } from '../daemon/gateway-client/content-chunks.js';

/** The local facts the manager needs from the desktop state. */
export interface GatewayConnectionConfig {
  readonly gatewayUrl: string;
  readonly accessToken: string;
  readonly computerName: string;
}

/** How the manager finds the locally authorised project for a `project_id`. */
export type ProjectResolver = (projectId: string) => { rootPath: string; enabled: boolean } | null;

export interface PublishedProject {
  readonly projectId: string;
  readonly displayName: string;
  readonly rootPath: string;
  readonly enabled: boolean;
}

/** What the manager reports back to the app. */
export interface GatewayConnectionStatus {
  readonly configured: boolean;
  readonly connected: boolean;
  readonly lastError: string | null;
  readonly lastActivityAt: string | null;
}

export interface GatewayConnectionManagerOptions {
  /** The local Codex session that runs the tasks. */
  readonly session: CodexSession;
  /** Resolve a remote `project_id` to a locally authorised project. */
  readonly resolveProject: ProjectResolver;
  /** Enumerate projects that may contribute device-visible Codex sessions. */
  readonly listProjects?: () => readonly PublishedProject[];
  /** Called whenever the connection status changes. */
  readonly onStatusChange?: (status: GatewayConnectionStatus) => void;
  readonly onTaskEvent?: (event: DispatchedTaskEvent) => void;
  readonly onProjectChange?: (frame: Extract<GatewayFrame, { type: 'project.upsert' | 'project.revoke' }>) => void;
  /** Desktop version reported in the hello frame. */
  readonly desktopVersion?: string;
  /**
   * How the WebSocket is created.
   *
   * The default opens a real socket. Tests inject a fake one so the status
   * transitions can be exercised without a server.
   */
  readonly socketFactory?: SocketFactory;
  /** How the dispatcher resolves a project, overridable for tests. */
  readonly resolveProjectOverride?: ProjectResolver;
}

/**
 * Adapt the local Codex session to what the dispatcher needs.
 *
 * The dispatcher knows nothing about Electron or about the session object: it
 * only knows how to start a turn, how to continue one, and where a session
 * ran. This adapter is that boundary, so it can be tested on its own.
 */
export function createCodexRunner(session: CodexSession): CodexRunner {
  return {
    startTurn: (input) =>
      session
        .startTurn(input.prompt, input.cwd)
        .then((turn) => ({ threadId: turn.threadId ?? '', turnId: turn.turnId ?? '' })),
    continueTurn: (input) =>
      session
        .continueSession(input.threadId, input.prompt, input.cwd)
        .then((turn) => ({ threadId: turn.threadId ?? '', turnId: turn.turnId ?? '' })),
    sessionCwd: async (threadId) => {
      const detail = await session.readSession(threadId);
      return detail?.cwd ?? null;
    },
    on: (listener: (event: CodexDomainEvent) => void) => session.on(listener),
    status: () => session.status(),
    interrupt: (turnId) => session.interrupt(turnId),
  };
}

export class GatewayConnectionManager {
  private readonly options: GatewayConnectionManagerOptions;
  private client: GatewayClient | null = null;
  private dispatcher: TaskDispatcher | null = null;
  private config: GatewayConnectionConfig | null = null;
  private connected = false;
  private lastError: string | null = null;
  private lastActivityAt: string | null = null;
  private eventCounter = 0;
  private messageCounter = 0;
  private readonly agentSessions = new Map<
    string,
    { threadId: string; projectId: string; rootPath: string }
  >();
  private readonly managedThreads = new Map<string, string>();
  private syncInFlight: Promise<void> | null = null;
  private publishedProjects = new Map<string, PublishedProject>();
  private readonly gatewayProjectMetadata = new Map<string, { displayName: string; capabilities: readonly ProjectCapability[]; availability: 'online' | 'offline'; grantVersion: number }>();
  private readonly blockedThreads = new Set<string>();
  private readonly bindingWaiters = new Map<string, { resolve: (value: unknown) => void; timer: ReturnType<typeof setTimeout> }>();

  constructor(options: GatewayConnectionManagerOptions) {
    this.options = options;
  }

  /** The current connection status. */
  status(): GatewayConnectionStatus {
    return {
      configured: this.config !== null,
      connected: this.connected,
      lastError: this.lastError,
      lastActivityAt: this.lastActivityAt,
    };
  }

  /**
   * Connect to the Gateway, replacing any previous connection.
   *
   * The connection is dialled out by the client itself, which owns the
   * reconnect and heartbeat timers. Calling this again with the same
   * configuration restarts the connection, which is what a manual reconnect
   * should do.
   */
  connect(config: GatewayConnectionConfig): void {
    this.disconnect();
    this.config = config;
    this.lastError = null;

    const client = new GatewayClient({
      url: config.gatewayUrl,
      token: config.accessToken,
      desktopVersion: this.options.desktopVersion ?? '0.1.0',
      capabilities: ['project_registry', 'codex_app_server'],
      ...(this.options.socketFactory === undefined
        ? {}
        : { socketFactory: this.options.socketFactory }),
    });

    const dispatcher = new TaskDispatcher({
      resolveProject: (projectId) =>
        this.gatewayProjectMetadata.get(projectId)?.availability === 'offline'
          ? null
          :
        this.options.resolveProject(projectId) ??
        (() => {
          const project = this.publishedProjects.get(projectId);
          return project === undefined ? null : { rootPath: project.rootPath, enabled: project.enabled };
        })(),
      defaultCwd: homedir(),
      resolveAgentSession: (agentSessionId) => {
        const target = this.agentSessions.get(agentSessionId);
        return target === undefined ? null : { agentSessionId, ...target };
      },
      runner: createCodexRunner(this.options.session),
      onEvent: (event) => {
        this.options.onTaskEvent?.(event);
        client.send(
          buildTaskEventFrame({
            messageId: this.nextMessageId(),
            sequence: this.nextSequence(),
            type: event.type,
            payload: event.payload,
            sentAt: new Date().toISOString(),
          }),
        );
        if (event.type === 'task.completed' && event.payload.thread_id !== null) {
          void this.publishAvailableMessage(client, event.payload.thread_id);
        }
        if (event.type !== 'task.started' && event.type !== 'task.progress') {
          void this.syncAgentSessions();
        }
        if (
          event.type === 'task.failed' &&
          event.payload.error?.message?.includes('active writer') &&
          event.payload.thread_id !== null
        ) {
          this.blockedThreads.add(event.payload.thread_id);
          void this.syncAgentSessions();
        }
        if (
          event.type === 'task.completed' ||
          event.type === 'task.failed' ||
          event.type === 'task.interrupted' ||
          event.type === 'task.cancelled'
        ) {
          // Codex Desktop must be able to open the same thread after the
          // Gateway task finishes; the stdio app-server otherwise keeps it
          // locked as if another client were editing the conversation.
          setTimeout(() => this.options.session.close(), 0);
        }
      },
    });

    client.on((event) => {
      if (event.type === 'connected') {
        this.connected = true;
        this.lastError = null;
        this.touch();
        this.publishStatus();
        void this.syncAgentSessions();
        return;
      }
      if (event.type === 'disconnected') {
        this.connected = false;
        this.touch();
        this.publishStatus();
        return;
      }
      if (event.type === 'rejected') {
        this.lastError = event.error.message;
        this.touch();
        this.publishStatus();
        return;
      }
      if (event.type === 'frame') {
        if (event.frame.type === 'binding.result') {
          const waiter = this.bindingWaiters.get(event.frame.payload.reply_to_message_id);
          if (waiter !== undefined) {
            clearTimeout(waiter.timer);
            this.bindingWaiters.delete(event.frame.payload.reply_to_message_id);
            waiter.resolve(event.frame.payload);
          }
          return;
        }
        if (event.frame.type === 'agent_sessions.sync.result') {
          if (!event.frame.payload.ok) {
            this.lastError = event.frame.payload.error?.message ?? 'Gateway 未保存会话目录';
            this.publishStatus();
          }
          return;
        }
        if (event.frame.type === 'messages.list' || event.frame.type === 'message.content.get') {
          void this.handleHistoryQuery(event.frame);
          return;
        }
        if (event.frame.type === 'project.upsert' || event.frame.type === 'project.revoke') {
          if (event.frame.type === 'project.upsert') {
            const current = this.gatewayProjectMetadata.get(event.frame.payload.project_id);
            if (current === undefined || event.frame.payload.grant_version >= current.grantVersion) {
              this.gatewayProjectMetadata.set(event.frame.payload.project_id, {
                displayName: event.frame.payload.display_name,
                capabilities: event.frame.payload.capabilities,
                availability: event.frame.payload.availability,
                grantVersion: event.frame.payload.grant_version,
              });
            }
          } else {
            const current = this.gatewayProjectMetadata.get(event.frame.payload.project_id);
            if (current === undefined || event.frame.payload.grant_version >= current.grantVersion) {
              this.gatewayProjectMetadata.set(event.frame.payload.project_id, {
                displayName: current?.displayName ?? event.frame.payload.project_id,
                capabilities: current?.capabilities ?? [],
                availability: 'offline',
                grantVersion: event.frame.payload.grant_version,
              });
            }
          }
          this.options.onProjectChange?.(event.frame);
          return;
        }
        this.handleFrame(event.frame);
      }
    });

    this.client = client;
    this.dispatcher = dispatcher;
    client.connect();
    this.publishStatus();
  }

  private async publishAvailableMessage(client: GatewayClient, threadId: string): Promise<void> {
    try {
      const session = await this.options.session.readSession(threadId);
      const message = session?.turns
        .flatMap((turn) => turn.messages)
        .reverse()
        .find((item) => item.role === 'assistant' && item.id.length > 0);
      if (message === undefined || this.client !== client || !this.connected) return;
      client.send(buildMessageAvailableFrame({
        messageId: this.nextMessageId(), sequence: this.nextSequence(),
        agentSessionId: `codex_${threadId}`,
        availableMessageId: message.id, sentAt: new Date().toISOString(),
      }));
    } catch {
      // A transcript read failure must not fabricate a message ID.
    }
  }

  private async handleHistoryQuery(frame: Extract<GatewayFrame, { type: 'messages.list' | 'message.content.get' }>): Promise<void> {
    const client = this.client;
    if (!client) return;
    try {
      if (!this.agentSessions.has(frame.payload.agent_session_id)) throw new Error('会话不属于当前 Desktop');
      const session = await this.options.session.readSession(frame.payload.agent_session_id);
      if (!session) throw new Error('会话不存在');
      if (frame.type === 'messages.list') {
        const items = session.turns.flatMap((turn) => turn.messages)
          .filter((message) => message.role === 'user' || message.role === 'assistant')
          .slice(-16)
          .map((message) => ({ message_id: message.id, role: message.role, summary: message.text.slice(0, 120) }));
        client.send(buildQueryResultFrame({ messageId: this.nextMessageId(), sequence: this.nextSequence(), replyToMessageId: frame.message_id, sentAt: new Date().toISOString(), data: { items, next_cursor: null } }));
      } else {
        const message = session.turns.flatMap((turn) => turn.messages).find((item) => item.id === frame.payload.message_id);
        if (!message) throw new Error('消息不存在');
        const offset = frame.payload.offset ?? 0;
        const chunk = readUtf8Chunk(message.text, offset);
        client.send(buildQueryResultFrame({ messageId: this.nextMessageId(), sequence: this.nextSequence(), replyToMessageId: frame.message_id, sentAt: new Date().toISOString(), data: { text: chunk.text, next_offset: chunk.nextOffset } }));
      }
    } catch (error) {
      client.send(buildQueryResultFrame({ messageId: this.nextMessageId(), sequence: this.nextSequence(), replyToMessageId: frame.message_id, sentAt: new Date().toISOString(), error: { code: 'NOT_FOUND', message: error instanceof Error ? error.message : '查询失败' } }));
    }
  }

  bindDevice(macAddress: string): Promise<unknown> {
    const client = this.client;
    if (!this.connected || client === null) {
      return Promise.resolve({ ok: false, error: { code: 'DEPENDENCY_UNAVAILABLE', message: 'Gateway 尚未连接' } });
    }
    const messageId = this.nextMessageId();
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.bindingWaiters.delete(messageId);
        resolve({ ok: false, error: { code: 'DEPENDENCY_UNAVAILABLE', message: 'Gateway 配对响应超时' } });
      }, 10_000);
      this.bindingWaiters.set(messageId, { resolve, timer });
      try {
        client.send(buildDeviceBindFrame({
          messageId,
          sequence: this.nextSequence(),
          macAddress,
          sentAt: new Date().toISOString(),
        }));
      } catch (error) {
        clearTimeout(timer);
        this.bindingWaiters.delete(messageId);
        resolve({ ok: false, error: { code: 'DEPENDENCY_UNAVAILABLE', message: error instanceof Error ? error.message : 'Gateway 配对发送失败' } });
      }
    });
  }

  /** Close the connection and stop reconnecting. */
  disconnect(): void {
    this.client?.close();
    this.client = null;
    this.dispatcher = null;
    this.connected = false;
    this.agentSessions.clear();
    for (const waiter of this.bindingWaiters.values()) {
      clearTimeout(waiter.timer);
      waiter.resolve({ ok: false, error: { code: 'DEPENDENCY_UNAVAILABLE', message: 'Gateway 连接已断开' } });
    }
    this.bindingWaiters.clear();
    if (this.config !== null) {
      this.publishStatus();
    }
  }

  /** Publish all authorised Codex conversations as one replace-all catalogue. */
  syncAgentSessions(): Promise<void> {
    if (this.syncInFlight !== null) {
      return this.syncInFlight;
    }
    this.syncInFlight = this.performAgentSessionSync().finally(() => {
      this.syncInFlight = null;
    });
    return this.syncInFlight;
  }

  private async performAgentSessionSync(): Promise<void> {
    const client = this.client;
    if (!this.connected || client === null) {
      return;
    }
    const configured = (this.options.listProjects?.() ?? []).filter((project) => project.enabled);
    const projects = configured.length > 0
      ? configured
      : (await this.withTimeout(this.options.session.listProjects(), 2000, [])).flatMap((project) =>
          project.roots[0]
            ? [{ projectId: project.projectId, displayName: project.name, rootPath: project.roots[0], enabled: true }]
            : [],
        );
    const effectiveProjects = projects.length > 0
      ? projects
      : [{ projectId: 'codex-default', displayName: 'Codex 默认项目', rootPath: homedir(), enabled: true }];
    this.publishedProjects = new Map(effectiveProjects.map((project) => [project.projectId, project]));

    try {
      for (const project of effectiveProjects) {
        if (!this.managedThreads.has(project.projectId)) {
          let threadId = '';
          try {
            threadId = await this.withTimeout(
              this.options.session.createSession(project.rootPath),
              2000,
              '',
            );
          } catch {
            // A project can still receive a new task when Codex has no
            // browsable session yet; thread creation happens when dispatching.
          }
          this.managedThreads.set(project.projectId, threadId);
        }
      }
      if (!this.connected || this.client !== client) {
        return;
      }
      const items: AgentSessionCatalogItem[] = [];
      const next = new Map<string, { threadId: string; projectId: string; rootPath: string }>();
      const existingSessions = await this.withTimeout(
        this.options.session.listSessions(100),
        2000,
        [],
      );
      for (const session of existingSessions) {
        const sessionCwd = session.cwd;
        if (sessionCwd === null) continue;
        if (this.blockedThreads.has(session.threadId)) continue;
        const project = effectiveProjects
          .filter(
            (candidate) =>
              sessionCwd === candidate.rootPath || sessionCwd.startsWith(`${candidate.rootPath}/`),
          )
          .sort((left, right) => right.rootPath.length - left.rootPath.length)[0];
        if (project === undefined) continue;
        const agentSessionId = `codex_${session.threadId}`;
        items.push({
          agent_session_id: agentSessionId,
          thread_id: session.threadId,
          project_id: project.projectId,
          project_name: project.displayName,
          agent: 'codex',
          title: session.title,
          status: session.status === 'running' || session.status === 'completed' || session.status === 'failed'
            ? session.status
            : 'idle',
          summary: '本机 Codex 已有对话',
        });
        next.set(agentSessionId, {
          threadId: session.threadId,
          projectId: project.projectId,
          rootPath: sessionCwd,
        });
      }
      for (const project of effectiveProjects) {
        const threadId = this.managedThreads.get(project.projectId);
        if (threadId === undefined) {
          continue;
        }
        const agentSessionId = `codex_${project.projectId}`;
        items.push({
          agent_session_id: agentSessionId,
          thread_id: threadId,
          project_id: project.projectId,
          project_name: project.displayName,
          agent: 'codex',
          title: `${project.displayName} · LinxBox`,
          status: 'idle',
          summary: '由 LinxBox 管理的 Codex 会话',
        });
        next.set(agentSessionId, {
          threadId,
          projectId: project.projectId,
          rootPath: project.rootPath,
        });
      }
      this.agentSessions.clear();
      for (const [id, target] of next) {
        this.agentSessions.set(id, target);
      }
      client.send(
        buildAgentSessionsSyncFrame({
          messageId: this.nextMessageId(),
          sequence: this.nextSequence(),
          items,
          sentAt: new Date().toISOString(),
        }),
      );
    } catch {
      this.lastError = '读取本机 Codex 会话目录失败';
      this.publishStatus();
    }
  }

  private async withTimeout<T>(promise: Promise<T>, timeoutMs: number, fallback: T): Promise<T> {
    return await Promise.race([
      promise,
      new Promise<T>((resolve) => setTimeout(() => resolve(fallback), timeoutMs)),
    ]);
  }

  /** Handle one inbound frame and answer it with an ack. */
  private handleFrame(frame: GatewayFrame): void {
    const dispatcher = this.dispatcher;
    const client = this.client;
    if (dispatcher === null || client === null) {
      return;
    }

    this.touch();

    void dispatcher.handle(frame).then((outcome) => {
      client.ack({
        messageId: frame.message_id,
        sequence: frame.sequence,
        taskId: frame.type === 'task.dispatch' ? frame.payload.task_id : 'unknown',
        disposition: outcome.disposition,
        ...(outcome.error === null
          ? {}
          : {
              error: publicError('POLICY_DENIED', outcome.error, frame.message_id),
            }),
      });
    });
  }

  private touch(): void {
    this.lastActivityAt = new Date().toISOString();
  }

  private publishStatus(): void {
    this.options.onStatusChange?.(this.status());
  }

  private nextSequence(): number {
    this.eventCounter += 1;
    return 1000 + this.eventCounter;
  }

  private nextMessageId(): string {
    this.messageCounter += 1;
    return `msg_dsk_${Date.now().toString(36)}_${this.messageCounter.toString(36)}`;
  }
}

/** Disposition names, re-exported for the renderer view. */
export const GATEWAY_DISPOSITIONS: readonly Disposition[] = [
  'accepted',
  'duplicate',
  'expired',
  'rejected',
];
