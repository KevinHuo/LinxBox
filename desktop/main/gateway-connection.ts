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
import { TaskDispatcher, type CodexRunner } from '../daemon/gateway-client/dispatcher.js';
import { buildTaskEventFrame, publicError } from '../daemon/gateway-client/frames.js';
import type { CodexDomainEvent } from '../daemon/codex-adapter/events.js';
import type { Disposition, GatewayFrame } from '../contracts/index.js';
import type { CodexSession } from './codex-session.js';

/** The local facts the manager needs from the desktop state. */
export interface GatewayConnectionConfig {
  readonly gatewayUrl: string;
  readonly accessToken: string;
  readonly computerName: string;
}

/** How the manager finds the locally authorised project for a `project_id`. */
export type ProjectResolver = (projectId: string) => { rootPath: string; enabled: boolean } | null;

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
  /** Called whenever the connection status changes. */
  readonly onStatusChange?: (status: GatewayConnectionStatus) => void;
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
        .continueSession(input.threadId, input.prompt)
        .then((turn) => ({ threadId: turn.threadId ?? '', turnId: turn.turnId ?? '' })),
    sessionCwd: async (threadId) => {
      const detail = await session.readSession(threadId);
      return detail?.cwd ?? null;
    },
    on: (listener: (event: CodexDomainEvent) => void) => session.on(listener),
    status: () => session.status(),
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
      resolveProject: (projectId) => this.options.resolveProject(projectId),
      runner: createCodexRunner(this.options.session),
      onEvent: (event) => {
        client.send(
          buildTaskEventFrame({
            messageId: this.nextMessageId(),
            sequence: this.nextSequence(),
            type: event.type,
            payload: event.payload,
            sentAt: new Date().toISOString(),
          }),
        );
      },
    });

    client.on((event) => {
      if (event.type === 'connected') {
        this.connected = true;
        this.lastError = null;
        this.touch();
        this.publishStatus();
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
        this.handleFrame(event.frame);
      }
    });

    this.client = client;
    this.dispatcher = dispatcher;
    client.connect();
    this.publishStatus();
  }

  /** Close the connection and stop reconnecting. */
  disconnect(): void {
    this.client?.close();
    this.client = null;
    this.dispatcher = null;
    this.connected = false;
    if (this.config !== null) {
      this.publishStatus();
    }
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
