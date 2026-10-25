/**
 * A fake provider process for tests.
 *
 * It lets a test drive the JSON-RPC transport without starting Codex: the
 * test writes into `stdout` and reads what the transport wrote to `stdin`.
 */
import { EventEmitter } from 'node:events';
import { PassThrough } from 'node:stream';
import type { ChildProcessWithoutNullStreams } from 'node:child_process';

import type { SpawnFn } from './process.js';

export class FakeChildProcess extends EventEmitter {
  readonly stdin = new PassThrough();
  readonly stdout = new PassThrough();
  readonly stderr = new PassThrough();
  killed = false;
  /** Everything the transport wrote, one JSON message per entry. */
  readonly written: string[] = [];

  constructor() {
    super();
    this.stdin.on('data', (chunk: Buffer) => {
      for (const line of chunk.toString('utf8').split('\n')) {
        if (line.trim().length > 0) {
          this.written.push(line);
        }
      }
    });
  }

  kill(): boolean {
    this.killed = true;
    return true;
  }

  /** Push a JSON message towards the transport. */
  emitMessage(message: unknown): void {
    this.stdout.write(`${JSON.stringify(message)}\n`);
  }

  /** Push a raw line towards the transport. */
  emitRaw(line: string): void {
    this.stdout.write(`${line}\n`);
  }

  /** The JSON messages the transport wrote, parsed. */
  sentMessages(): Record<string, unknown>[] {
    return this.written.map((line) => JSON.parse(line) as Record<string, unknown>);
  }
}

export interface FakeProcessHarness {
  readonly child: FakeChildProcess;
  readonly spawnFn: SpawnFn;
}

/** Create a fake child plus the `spawnFn` that returns it. */
export function createFakeProcess(): FakeProcessHarness {
  const child = new FakeChildProcess();
  const spawnFn: SpawnFn = () => child as unknown as ChildProcessWithoutNullStreams;
  return { child, spawnFn };
}
