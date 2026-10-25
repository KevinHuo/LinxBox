/**
 * Codex app-server process and JSON-RPC transport.
 *
 * The Desktop talks to the local Codex app-server over stdio. Nothing else is
 * supported: the Desktop never reads Codex's private database, never drives a
 * UI, and never uses a network protocol to reach a provider.
 *
 * Safety rules enforced here:
 *
 *  - the process is started with an argument array and `shell: false`;
 *  - the child environment is an explicit allowlist, so no token, cookie or
 *    `LINXBOX_*` variable leaks into the provider process;
 *  - stderr is consumed locally into a bounded ring buffer and never leaves
 *    the machine;
 *  - a stdout frame larger than the limit is a protocol error, not a buffer
 *    that grows without bound;
 *  - a server-initiated request is rejected, because the Desktop has no
 *    trusted local approval surface.
 */
import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';

import {
  isNotification,
  isResponse,
  isServerRequest,
  JSONRPC_VERSION,
  METHOD_NOT_FOUND,
  parseJsonRpcMessage,
  type JsonRpcResponse,
} from './protocol.js';

/**
 * Environment variables the provider process may inherit.
 *
 * `CODEX_HOME` is included because it selects the provider's own profile
 * directory. Everything else - API keys, cookies, `LINXBOX_*`, editor
 * variables - is dropped.
 */
export const ENV_ALLOWLIST: readonly string[] = [
  'PATH',
  'HOME',
  'TMPDIR',
  'USER',
  'LANG',
  'LC_ALL',
  'TERM',
  'CODEX_HOME',
];

/** Default upper bound for a single stdout frame. */
export const MAX_FRAME_BYTES = 16 * 1024 * 1024;

/** Default number of stderr lines kept in the local ring buffer. */
export const MAX_STDERR_LINES = 200;

/** Default request timeout. */
export const DEFAULT_REQUEST_TIMEOUT_MS = 30_000;

/**
 * Build the child environment from an allowlist.
 *
 * `extra` is applied last so a caller can deliberately add a value; it is
 * never populated from the ambient environment implicitly.
 */
export function buildChildEnv(
  source: NodeJS.ProcessEnv,
  allowlist: readonly string[] = ENV_ALLOWLIST,
  extra: Readonly<Record<string, string>> = {},
): Record<string, string> {
  const env: Record<string, string> = {};
  for (const key of allowlist) {
    const value = source[key];
    if (value !== undefined) {
      env[key] = value;
    }
  }
  return { ...env, ...extra };
}

export type CodexProcessEvent =
  | { readonly type: 'notification'; readonly method: string; readonly params: unknown }
  | {
      readonly type: 'server-request';
      readonly id: number | string;
      readonly method: string;
      readonly params: unknown;
    }
  | { readonly type: 'exit'; readonly code: number | null; readonly signal: NodeJS.Signals | null }
  | { readonly type: 'error'; readonly error: Error };

export type CodexProcessListener = (event: CodexProcessEvent) => void;

export type SpawnFn = (
  command: string,
  args: readonly string[],
  options: {
    cwd: string;
    env: Record<string, string>;
    shell: false;
    stdio: ['pipe', 'pipe', 'pipe'];
  },
) => ChildProcessWithoutNullStreams;

export interface CodexProcessOptions {
  readonly binaryPath: string;
  readonly args: readonly string[];
  readonly cwd: string;
  readonly env?: Readonly<Record<string, string>>;
  readonly requestTimeoutMs?: number;
  readonly maxFrameBytes?: number;
  readonly maxStderrLines?: number;
  readonly spawnFn?: SpawnFn;
}

interface PendingRequest {
  readonly method: string;
  readonly resolve: (value: unknown) => void;
  readonly reject: (error: Error) => void;
  readonly timer: ReturnType<typeof setTimeout>;
}

/** Error raised by the adapter. `code` is a stable, non-localised identifier. */
export class CodexAdapterError extends Error {
  readonly code: string;

  constructor(code: string, message: string) {
    super(message);
    this.name = 'CodexAdapterError';
    this.code = code;
  }
}

/**
 * The stdio JSON-RPC transport to a Codex app-server process.
 */
export class CodexProcess {
  private readonly child: ChildProcessWithoutNullStreams;
  private readonly listeners = new Set<CodexProcessListener>();
  private readonly pending = new Map<number, PendingRequest>();
  private readonly requestTimeoutMs: number;
  private readonly maxFrameBytes: number;
  private readonly maxStderrLines: number;
  private readonly stderrBuffer: string[] = [];
  private buffer = '';
  private sequence = 0;
  private closed = false;

  constructor(options: CodexProcessOptions) {
    this.requestTimeoutMs = options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
    this.maxFrameBytes = options.maxFrameBytes ?? MAX_FRAME_BYTES;
    this.maxStderrLines = options.maxStderrLines ?? MAX_STDERR_LINES;

    const spawnFn: SpawnFn = options.spawnFn ?? spawn;
    this.child = spawnFn(options.binaryPath, options.args, {
      cwd: options.cwd,
      env: options.env ?? buildChildEnv(process.env),
      shell: false,
      stdio: ['pipe', 'pipe', 'pipe'],
    });

    this.child.stdout.setEncoding('utf8');
    this.child.stdout.on('data', (chunk: string) => {
      this.consume(chunk);
    });
    this.child.stderr.setEncoding('utf8');
    this.child.stderr.on('data', (chunk: string) => {
      this.consumeStderr(chunk);
    });
    this.child.on('error', (error: Error) => {
      this.emit({ type: 'error', error });
      this.failAll(new CodexAdapterError('CODEX_UNAVAILABLE', '无法启动本机 Codex'));
    });
    this.child.on('exit', (code: number | null, signal: NodeJS.Signals | null) => {
      this.emit({ type: 'exit', code, signal });
      this.failAll(new CodexAdapterError('CODEX_DISCONNECTED', '本机 Codex 连接已关闭'));
      this.closed = true;
    });
  }

  /** Subscribe to process events. Returns an unsubscribe function. */
  on(listener: CodexProcessListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /** The most recent stderr lines, kept locally for diagnostics only. */
  get stderrLines(): readonly string[] {
    return this.stderrBuffer;
  }

  /** Whether the transport has been closed. */
  get isClosed(): boolean {
    return this.closed;
  }

  /**
   * Send a request and wait for the matching response.
   *
   * A timeout rejects the call; the provider is never retried automatically
   * because a timed-out turn may still have side effects.
   */
  call(method: string, params: unknown, timeoutMs?: number): Promise<unknown> {
    if (this.closed) {
      return Promise.reject(new CodexAdapterError('CODEX_DISCONNECTED', '本机 Codex 未连接'));
    }
    const id = ++this.sequence;
    return new Promise<unknown>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new CodexAdapterError('CODEX_TIMEOUT', '本机 Codex 请求超时'));
      }, timeoutMs ?? this.requestTimeoutMs);
      this.pending.set(id, { method, resolve, reject, timer });
      try {
        this.write({ jsonrpc: JSONRPC_VERSION, id, method, params });
      } catch (error) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(
          error instanceof Error
            ? error
            : new CodexAdapterError('CODEX_DISCONNECTED', '本机 Codex 输入连接已关闭'),
        );
      }
    });
  }

  /** Send a notification. */
  notify(method: string, params: unknown): void {
    this.write({ jsonrpc: JSONRPC_VERSION, method, params });
  }

  /** Stop the provider process. */
  close(): void {
    if (this.closed) {
      return;
    }
    this.closed = true;
    this.child.kill();
    // Codex keeps the thread writer lock until the app-server exits. A
    // graceful signal can take a moment, so force termination is scheduled as
    // a bounded cleanup rather than leaving a stale writer behind.
    const child = this.child;
    let exited = false;
    const forceKill = setTimeout(() => {
      if (!exited) {
        child.kill('SIGKILL');
      }
    }, 500);
    child.once('exit', () => {
      exited = true;
      clearTimeout(forceKill);
    });
    this.failAll(new CodexAdapterError('CODEX_DISCONNECTED', '本机 Codex 连接已停止'));
  }

  private write(message: unknown): void {
    this.child.stdin.write(`${JSON.stringify(message)}\n`);
  }

  private consume(chunk: string): void {
    this.buffer += chunk;
    if (Buffer.byteLength(this.buffer, 'utf8') > this.maxFrameBytes) {
      this.failAll(new CodexAdapterError('CODEX_PROTOCOL', '本机 Codex 响应超过大小上限'));
      this.close();
      return;
    }
    let end = this.buffer.indexOf('\n');
    while (end !== -1) {
      const line = this.buffer.slice(0, end);
      this.buffer = this.buffer.slice(end + 1);
      this.handleLine(line);
      end = this.buffer.indexOf('\n');
    }
  }

  private handleLine(line: string): void {
    if (line.trim().length === 0) {
      return;
    }
    const message = parseJsonRpcMessage(line);
    if (message === null) {
      this.failAll(new CodexAdapterError('CODEX_PROTOCOL', '本机 Codex 返回了无法解析的消息'));
      return;
    }
    if (isResponse(message)) {
      this.settle(message);
      return;
    }
    if (isServerRequest(message)) {
      this.emit({
        type: 'server-request',
        id: message.id,
        method: message.method,
        params: message.params,
      });
      this.rejectServerRequest(message.id);
      return;
    }
    if (isNotification(message)) {
      this.emit({ type: 'notification', method: message.method, params: message.params });
    }
  }

  private settle(response: JsonRpcResponse): void {
    const pending = this.pending.get(response.id);
    if (!pending) {
      return;
    }
    this.pending.delete(response.id);
    clearTimeout(pending.timer);
    if ('error' in response) {
      const detail = response.error.message ?? `本机 Codex 调用失败：${pending.method}`;
      pending.reject(new CodexAdapterError('CODEX_RPC', detail));
      return;
    }
    pending.resolve(response.result);
  }

  /**
   * Refuse a server-initiated request.
   *
   * The Desktop has no trusted local approval surface in this build, so an
   * approval request is declined instead of silently accepted.
   */
  private rejectServerRequest(id: number | string): void {
    try {
      this.write({
        jsonrpc: JSONRPC_VERSION,
        id,
        error: {
          code: METHOD_NOT_FOUND,
          message: 'Unsupported interaction; user input is required in Codex',
        },
      });
    } catch {
      // The provider is gone; the exit handler will surface the disconnect.
    }
  }

  private consumeStderr(chunk: string): void {
    for (const line of chunk.split('\n')) {
      if (line.trim().length === 0) {
        continue;
      }
      this.stderrBuffer.push(line);
      if (this.stderrBuffer.length > this.maxStderrLines) {
        this.stderrBuffer.shift();
      }
    }
  }

  private failAll(error: Error): void {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
  }

  private emit(event: CodexProcessEvent): void {
    for (const listener of this.listeners) {
      listener(event);
    }
  }
}
