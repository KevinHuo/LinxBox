/**
 * Outbound Gateway client.
 *
 * The Desktop dials out to the Cloud Gateway over a persistent WebSocket and
 * never listens on a port. The connection carries:
 *
 *  - `client.hello` right after the socket opens;
 *  - `client.heartbeat` on a timer;
 *  - `task.ack` for every delivery the Gateway sends;
 *  - task events for every state change.
 *
 * Reconnection is exponential with a cap. A reconnect resumes from the last
 * acknowledged sequence, so the Gateway can replay whatever was missed.
 */
import { inspect } from 'node:util';

import {
  buildAckFrame,
  buildHelloFrame,
  buildHeartbeatFrame,
  parseInboundFrame,
  publicError,
  GATEWAY_CONNECT_PATH,
} from './frames.js';
import type {
  DesktopFrame,
  Disposition,
  GatewayFrame,
  PublicError,
} from '../../contracts/index.js';
import type { Logger } from '../../shared/logger.js';

/** The subset of the WebSocket API the client relies on. */
export interface SocketLike {
  readonly readyState: number;
  send(data: string): void;
  close(code?: number, reason?: string): void;
  addEventListener(type: 'open', listener: () => void): void;
  addEventListener(type: 'message', listener: (event: { data: unknown }) => void): void;
  addEventListener(type: 'close', listener: (event: { code: number }) => void): void;
  addEventListener(type: 'error', listener: () => void): void;
}

export type SocketFactory = (url: string, protocols: string[]) => SocketLike;

export type GatewayClientEvent =
  | { readonly type: 'connected' }
  | { readonly type: 'disconnected'; readonly reason: string }
  | { readonly type: 'frame'; readonly frame: GatewayFrame }
  | { readonly type: 'rejected'; readonly error: PublicError };

export type GatewayClientListener = (event: GatewayClientEvent) => void;

export interface GatewayClientOptions {
  /** Full connect URL, e.g. `wss://gateway.example/v1/clients/connect`. */
  readonly url: string;
  /** Client token. It identifies this machine to the Gateway. */
  readonly token: string;
  readonly desktopVersion: string;
  readonly capabilities: readonly string[];
  readonly heartbeatMs?: number;
  readonly reconnectBaseMs?: number;
  readonly reconnectMaxMs?: number;
  readonly socketFactory?: SocketFactory;
  readonly now?: () => Date;
  readonly random?: () => number;
  /** Optional structured logger supplied by the Electron wiring layer. */
  readonly logger?: Logger;
}

/**
 * Default socket factory.
 *
 * The standard WebSocket API cannot set an `Authorization` header, so the
 * token travels as a sub-protocol instead. It still never appears in a frame
 * payload, in a log line or in a URL query string.
 */
const defaultSocketFactory: SocketFactory = (url, protocols) => new WebSocket(url, protocols);

/** Build the connect URL from a base URL. */
export function buildConnectUrl(baseUrl: string): string {
  const url = new URL(baseUrl);
  if (!url.pathname || url.pathname === '/') {
    url.pathname = GATEWAY_CONNECT_PATH;
  }
  return url.toString();
}

export class GatewayClient {
  private readonly options: GatewayClientOptions;
  private readonly listeners = new Set<GatewayClientListener>();
  private readonly now: () => Date;
  private readonly random: () => number;
  private socket: SocketLike | null = null;
  private heartbeatTimer: ReturnType<typeof setInterval> | undefined;
  private reconnectTimer: ReturnType<typeof setTimeout> | undefined;
  private attempts = 0;
  private closed = false;
  private sequence = 0;
  private acked = 0;
  private messageCounter = 0;

  constructor(options: GatewayClientOptions) {
    this.options = options;
    this.now = options.now ?? (() => new Date());
    this.random = options.random ?? Math.random;
  }

  /** Subscribe to client events. Returns an unsubscribe function. */
  on(listener: GatewayClientListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /** The last sequence the Gateway acknowledged. */
  get lastAckedSequence(): number {
    return this.acked;
  }

  /** Open the connection. */
  connect(): void {
    if (this.closed) {
      return;
    }
    const factory = this.options.socketFactory ?? defaultSocketFactory;
    const url = buildConnectUrl(this.options.url);
    const protocols = ['linxbox.v1', `linxbox-token.${this.options.token}`];

    let socket: SocketLike;
    try {
      socket = factory(url, protocols);
    } catch (error) {
      this.options.logger?.error('WSS 连接创建失败', {
        url,
        reason: error instanceof Error ? error.message : 'unknown',
      });
      this.emit({
        type: 'rejected',
        error: publicError(
          'DEPENDENCY_UNAVAILABLE',
          error instanceof Error ? error.message : '无法建立 Gateway 连接',
          'gateway',
        ),
      });
      this.scheduleReconnect();
      return;
    }
    this.socket = socket;

    socket.addEventListener('open', () => {
      this.attempts = 0;
      this.options.logger?.info('WSS 已连接', { url });
      this.sendHello();
      this.emit({ type: 'connected' });
      this.startHeartbeat();
    });

    socket.addEventListener('message', (event) => {
      this.handleMessage(event.data);
    });

    socket.addEventListener('close', (event) => {
      this.stopHeartbeat();
      this.socket = null;
      this.options.logger?.warn('WSS 已断开', { url, code: event.code });
      this.emit({ type: 'disconnected', reason: 'Gateway 连接已关闭' });
      this.scheduleReconnect();
    });

    socket.addEventListener('error', () => {
      this.options.logger?.error('WSS 连接异常', { url });
      this.emit({
        type: 'rejected',
        error: publicError('DEPENDENCY_UNAVAILABLE', 'Gateway 连接异常', 'gateway'),
      });
    });
  }

  /** Send a frame to the Gateway. */
  send(frame: DesktopFrame): void {
    const socket = this.socket;
    if (socket === null || socket.readyState !== 1) {
      return;
    }
    const data = JSON.stringify(frame);
    socket.send(data);
    console.warn('[desktop.gateway.raw] desktop->gateway');
    console.warn(
      inspect(frame, { depth: null, colors: true, maxArrayLength: null, maxStringLength: null }),
    );
  }

  /** Acknowledge a delivery. */
  ack(input: {
    messageId: string;
    sequence: number;
    taskId: string;
    disposition: Disposition;
    error?: PublicError;
  }): void {
    this.send(
      buildAckFrame({
        messageId: input.messageId,
        sequence: input.sequence,
        taskId: input.taskId,
        disposition: input.disposition,
        sentAt: this.now().toISOString(),
        ...(input.error === undefined ? {} : { error: input.error }),
      }),
    );
    if (input.sequence > this.acked) {
      this.acked = input.sequence;
    }
  }

  /** Stop the client for good. */
  close(): void {
    this.closed = true;
    this.stopHeartbeat();
    if (this.reconnectTimer !== undefined) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = undefined;
    }
    this.socket?.close(1000, 'client closed');
    this.socket = null;
  }

  private sendHello(): void {
    this.send(
      buildHelloFrame({
        messageId: this.nextMessageId(),
        sequence: this.nextSequence(),
        desktopVersion: this.options.desktopVersion,
        lastAckedSequence: this.acked,
        capabilities: this.options.capabilities,
        sentAt: this.now().toISOString(),
      }),
    );
  }

  private startHeartbeat(): void {
    this.stopHeartbeat();
    const period = this.options.heartbeatMs ?? 15_000;
    this.heartbeatTimer = setInterval(() => {
      this.send(
        buildHeartbeatFrame({
          messageId: this.nextMessageId(),
          sequence: this.nextSequence(),
          clientTime: this.now().toISOString(),
        }),
      );
    }, period);
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer !== undefined) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = undefined;
    }
  }

  private handleMessage(data: unknown): void {
    const text = typeof data === 'string' ? data : String(data);
    let parsed: unknown;
    try {
      parsed = JSON.parse(text);
      console.warn('[desktop.gateway.raw] gateway->desktop');
      console.warn(
        inspect(parsed, { depth: null, colors: true, maxArrayLength: null, maxStringLength: null }),
      );
    } catch {
      console.warn('[desktop.gateway.raw] gateway->desktop 非法 JSON', text);
      this.options.logger?.warn('WSS 收到非法 JSON', {
        direction: 'gateway->desktop',
        bytes: Buffer.byteLength(text),
      });
      this.emit({
        type: 'rejected',
        error: publicError('INVALID_ARGUMENT', 'Gateway 帧不是合法 JSON', 'gateway'),
      });
      return;
    }
    const result = parseInboundFrame(parsed);
    if (!result.ok) {
      this.options.logger?.warn('WSS 收到无效帧', {
        direction: 'gateway->desktop',
        reason: result.error.message,
        bytes: Buffer.byteLength(text),
      });
      this.emit({ type: 'rejected', error: result.error });
      return;
    }
    this.emit({ type: 'frame', frame: result.frame });
  }

  private scheduleReconnect(): void {
    if (this.closed || this.reconnectTimer !== undefined) {
      return;
    }
    const base = this.options.reconnectBaseMs ?? 1000;
    const max = this.options.reconnectMaxMs ?? 30_000;
    const backoff = Math.min(max, base * 2 ** Math.min(this.attempts, 5));
    const jitter = backoff * 0.3 * this.random();
    this.attempts += 1;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = undefined;
      this.connect();
    }, backoff + jitter);
  }

  private nextSequence(): number {
    this.sequence += 1;
    return this.sequence;
  }

  private nextMessageId(): string {
    this.messageCounter += 1;
    return `msg_${this.now().getTime().toString(36)}_${this.messageCounter.toString(36)}`;
  }

  private emit(event: GatewayClientEvent): void {
    for (const listener of this.listeners) {
      listener(event);
    }
  }
}
