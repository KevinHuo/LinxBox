import { EventEmitter } from 'node:events';
import { PassThrough } from 'node:stream';
import type { ChildProcessWithoutNullStreams } from 'node:child_process';
import { describe, expect, it } from 'vitest';

import { CodexProcess, buildChildEnv, type SpawnFn } from './process.js';

/** A fake provider process the test can drive. */
class FakeChild extends EventEmitter {
  readonly stdin = new PassThrough();
  readonly stdout = new PassThrough();
  readonly stderr = new PassThrough();
  killed = false;
  written: string[] = [];

  constructor() {
    super();
    this.stdin.on('data', (chunk: Buffer) => {
      this.written.push(chunk.toString('utf8'));
    });
  }

  kill(): boolean {
    this.killed = true;
    return true;
  }
}

interface Harness {
  readonly process: CodexProcess;
  readonly child: FakeChild;
  readonly spawnArgs: { command: string; args: readonly string[]; options: unknown }[];
}

function createProcess(options: { maxFrameBytes?: number; maxStderrLines?: number } = {}): Harness {
  const child = new FakeChild();
  const spawnArgs: Harness['spawnArgs'] = [];
  const spawnFn: SpawnFn = (command, args, spawnOptions) => {
    spawnArgs.push({ command, args, options: spawnOptions });
    return child as unknown as ChildProcessWithoutNullStreams;
  };
  const process = new CodexProcess({
    binaryPath: '/opt/codex',
    args: ['app-server', '--stdio'],
    cwd: '/tmp/work',
    env: { PATH: '/usr/bin' },
    spawnFn,
    requestTimeoutMs: 500,
    ...(options.maxFrameBytes === undefined ? {} : { maxFrameBytes: options.maxFrameBytes }),
    ...(options.maxStderrLines === undefined ? {} : { maxStderrLines: options.maxStderrLines }),
  });
  return { process, child, spawnArgs };
}

describe('buildChildEnv', () => {
  it('keeps only the allowlisted variables', () => {
    const env = buildChildEnv(
      {
        PATH: '/usr/bin',
        HOME: '/Users/demo',
        OPENAI_API_KEY: 'sk-secret',
        LINXBOX_TOKEN: 'token',
        cookie: 'session',
        RANDOM_VAR: 'x',
      },
      ['PATH', 'HOME'],
    );
    expect(env).toEqual({ PATH: '/usr/bin', HOME: '/Users/demo' });
  });

  it('drops every credential-shaped variable by default', () => {
    const env = buildChildEnv({
      PATH: '/usr/bin',
      OPENAI_API_KEY: 'sk-secret',
      LINXBOX_CLIENT_TOKEN: 'token',
      COOKIE: 'session',
    });
    expect(env['OPENAI_API_KEY']).toBeUndefined();
    expect(env['LINXBOX_CLIENT_TOKEN']).toBeUndefined();
    expect(env['COOKIE']).toBeUndefined();
    expect(env['PATH']).toBe('/usr/bin');
  });

  it('applies the explicit override last', () => {
    const env = buildChildEnv({ PATH: '/usr/bin' }, ['PATH'], { CODEX_HOME: '/custom' });
    expect(env['CODEX_HOME']).toBe('/custom');
  });
});

describe('CodexProcess', () => {
  it('starts the provider without a shell and with an argument array', () => {
    const harness = createProcess();
    expect(harness.spawnArgs).toHaveLength(1);
    expect(harness.spawnArgs[0]?.command).toBe('/opt/codex');
    expect(harness.spawnArgs[0]?.args).toEqual(['app-server', '--stdio']);
    const options = harness.spawnArgs[0]?.options as { shell: boolean; cwd: string };
    expect(options.shell).toBe(false);
    expect(options.cwd).toBe('/tmp/work');
  });

  it('resolves a call when the matching response arrives', async () => {
    const harness = createProcess();
    const call = harness.process.call('initialize', {});
    harness.child.stdout.write(`${JSON.stringify({ id: 1, result: { ok: true } })}\n`);
    await expect(call).resolves.toEqual({ ok: true });
  });

  it('rejects a call when the provider returns an error', async () => {
    const harness = createProcess();
    const call = harness.process.call('turn/start', {});
    harness.child.stdout.write(
      `${JSON.stringify({ id: 1, error: { code: -32000, message: 'boom' } })}\n`,
    );
    await expect(call).rejects.toMatchObject({ code: 'CODEX_RPC' });
  });

  it('rejects a call that never gets a response', async () => {
    const harness = createProcess();
    await expect(harness.process.call('initialize', {})).rejects.toMatchObject({
      code: 'CODEX_TIMEOUT',
    });
  });

  it('rejects pending calls when the provider sends garbage', async () => {
    const harness = createProcess();
    const call = harness.process.call('initialize', {});
    harness.child.stdout.write('this is not json\n');
    await expect(call).rejects.toMatchObject({ code: 'CODEX_PROTOCOL' });
  });

  it('rejects pending calls when a frame exceeds the size limit', async () => {
    const harness = createProcess({ maxFrameBytes: 64 });
    const call = harness.process.call('initialize', {});
    harness.child.stdout.write('x'.repeat(512));
    await expect(call).rejects.toMatchObject({ code: 'CODEX_PROTOCOL' });
  });

  it('refuses a server-initiated request instead of approving it', async () => {
    const harness = createProcess();
    harness.child.stdout.write(
      `${JSON.stringify({ jsonrpc: '2.0', id: 7, method: 'item/fileChange/requestApproval', params: {} })}\n`,
    );
    await new Promise((resolve) => setTimeout(resolve, 10));
    const response = harness.child.written.join('').trim();
    expect(response).toContain('"code":-32601');
    expect(response).toContain('user input is required');
  });

  it('keeps stderr in a bounded local buffer', async () => {
    const harness = createProcess({ maxStderrLines: 2 });
    harness.child.stderr.write('one\ntwo\nthree\n');
    await new Promise((resolve) => setTimeout(resolve, 10));
    expect(harness.process.stderrLines).toEqual(['two', 'three']);
  });

  it('rejects pending calls when the provider exits', async () => {
    const harness = createProcess();
    const call = harness.process.call('initialize', {});
    harness.child.emit('exit', 1, null);
    await expect(call).rejects.toMatchObject({ code: 'CODEX_DISCONNECTED' });
  });

  it('reports notifications to subscribers', async () => {
    const harness = createProcess();
    const seen: string[] = [];
    harness.process.on((event) => {
      if (event.type === 'notification') {
        seen.push(event.method);
      }
    });
    harness.child.stdout.write(
      `${JSON.stringify({ jsonrpc: '2.0', method: 'turn/started', params: { turn: { id: 'tn_1' } } })}\n`,
    );
    await new Promise((resolve) => setTimeout(resolve, 10));
    expect(seen).toEqual(['turn/started']);
  });

  it('stops the provider process on close', () => {
    const harness = createProcess();
    harness.process.close();
    expect(harness.child.killed).toBe(true);
  });
});
