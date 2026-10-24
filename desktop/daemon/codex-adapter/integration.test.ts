/**
 * Real Codex app-server acceptance.
 *
 * This test drives the locally installed Codex CLI over a real stdio session
 * and asserts that the Desktop can obtain real content from it. It is
 * opt-in because it needs the provider binary and the account quota:
 *
 * ```bash
 * LINXBOX_CODEX_E2E=1 pnpm vitest run daemon/codex-adapter/integration.test.ts
 * ```
 *
 * The suite is skipped when the environment flag is absent, so the regular
 * `pnpm check` never depends on a provider account.
 */
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';

import { CodexClient } from './client.js';
import { discoverCodexBinary } from './discovery.js';

const ENABLED = process.env['LINXBOX_CODEX_E2E'] === '1';

describe.skipIf(!ENABLED)('real Codex app-server', () => {
  const binary = discoverCodexBinary();
  let workspace: string;
  let client: CodexClient | null = null;

  beforeAll(() => {
    workspace = mkdtempSync(join(tmpdir(), 'linxbox-codex-e2e-'));
  });

  afterAll(() => {
    client?.close();
    rmSync(workspace, { recursive: true, force: true });
  });

  it('initialises against the installed provider', async () => {
    expect(binary, '未找到 Codex CLI，请安装 ChatGPT.app 或 codex').not.toBeNull();
    const active = new CodexClient({
      binaryPath: binary?.path ?? '',
      cwd: workspace,
      turnTimeoutMs: 180_000,
    });
    client = active;

    const result = await active.initialize();

    expect(result.userAgent).toContain('linxbox_desktop');
    expect(result.userAgent).toMatch(/\d+\.\d+\.\d+/);
    expect(result.platformOs).toBe('macos');
  }, 60_000);

  it('reads the account default model instead of hard-coding one', async () => {
    const active = client;
    expect(active).not.toBeNull();
    const model = await (active as CodexClient).readDefaultModel(workspace);
    expect(model, '未能读取 provider 的默认模型').toBeTruthy();
  }, 60_000);

  it('obtains real content from a real turn', async () => {
    const active = client;
    expect(active).not.toBeNull();
    const instance = active as CodexClient;

    const threadId = await instance.startThread(workspace);
    expect(threadId).toBeTruthy();

    const turnId = await instance.startTurn(
      threadId,
      '只回答一句话：当前工作目录里能看到什么？不要执行任何命令。',
      'linxbox-e2e-1',
    );
    expect(turnId).toBeTruthy();

    const state = await instance.waitForTerminal(turnId, 180_000);

    expect(state.status, `provider 终态为 ${state.status}：${state.error ?? '无错误信息'}`).toBe(
      'completed',
    );
    expect(state.finalText.length).toBeGreaterThan(0);
  }, 240_000);
});
