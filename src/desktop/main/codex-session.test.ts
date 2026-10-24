import { describe, expect, it } from 'vitest';

import { CodexSession } from './codex-session.js';
import { createFakeProcess } from '../daemon/codex-adapter/fake-process.test-support.js';

const initializeResult = {
  userAgent: 'linxbox_desktop/0.164.0 (macos)',
  codexHome: '/Users/demo/.codex',
  platformFamily: 'unix',
  platformOs: 'macos',
};

describe('CodexSession', () => {
  it('reports that no provider is installed', async () => {
    const session = new CodexSession({ binary: null });
    const probe = await session.probe();
    expect(probe.available).toBe(false);
    expect(probe.error).toContain('未找到本机 Codex');
  });

  it('reports the provider facts after the handshake', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: 'codex-cli 0.164.0' },
      spawnFn: harness.spawnFn,
    });

    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    harness.child.emitMessage({
      jsonrpc: '2.0',
      method: 'account/updated',
      params: { authMode: 'chatgpt', planType: 'plus' },
    });

    const probe = await probing;
    expect(probe.available).toBe(true);
    expect(probe.binaryPath).toBe('/opt/codex');
    expect(probe.version).toBe('codex-cli 0.164.0');
    expect(probe.userAgent).toContain('linxbox_desktop');
    session.close();
  });

  it('starts idle', () => {
    const session = new CodexSession({ binary: null });
    expect(session.status().status).toBe('idle');
    expect(session.status().finalText).toBe('');
  });

  it('forwards provider events to session subscribers', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const seen: string[] = [];
    session.on((event) => seen.push(event.type));

    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    harness.child.emitMessage({
      jsonrpc: '2.0',
      method: 'item/completed',
      params: { item: { type: 'agentMessage', phase: 'final_answer', text: 'hi' } },
    });

    expect(seen).toContain('final');
    session.close();
  });

  it('ignores an interrupt when no turn is running', async () => {
    const session = new CodexSession({ binary: null });
    await expect(session.interrupt('tn_1')).resolves.toBeUndefined();
  });
});

describe('CodexSession failure paths', () => {
  it('reports a provider that refuses to start', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });

    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, error: { code: -32000, message: 'boom' } });

    const probe = await probing;
    expect(probe.available).toBe(false);
    // The provider's raw reason stays in the local stderr buffer; the surfaced
    // message is generic so it can never leak provider internals.
    expect(probe.error).toContain('本机 Codex');
    session.close();
  });

  it('reports a provider whose handshake result is unusable', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });

    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: { unexpected: true } });

    const probe = await probing;
    expect(probe.available).toBe(false);
    session.close();
  });
});

describe('CodexSession interrupt', () => {
  it('asks the provider to interrupt a running turn', async () => {
    const harness = createFakeProcess();
    const session = new CodexSession({
      binary: { path: '/opt/codex', version: '1' },
      spawnFn: harness.spawnFn,
    });
    const probing = session.probe();
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 1, result: initializeResult });
    await probing;

    const starting = session.startTurn('x', '/tmp/work');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 2, result: { thread: { id: 'th_1' } } });
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 3, result: { turn: { id: 'tn_1', status: 'inProgress' } } });
    await starting;

    const interrupting = session.interrupt('tn_1');
    await new Promise((resolve) => setTimeout(resolve, 10));
    harness.child.emitMessage({ id: 4, result: {} });

    await expect(interrupting).resolves.toBeUndefined();
    session.close();
  });
});
