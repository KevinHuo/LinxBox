#!/usr/bin/env node
/**
 * 端到端联调：远端 WSS 命令 -> 本机 Codex -> 结果回传。
 *
 * 这条链路是真的：
 *   - 真的 WebSocket over TCP（mock gateway 是"云端"）；
 *   - 真的本机 Codex app-server 子进程（stdio JSON-RPC）；
 *   - 真的 Gateway/Desktop 1.0 帧格式。
 *
 * 跑法：
 *   pnpm build
 *   node scripts/e2e-wss-codex.mjs
 */
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { startMockGateway, DEFAULT_PORT, DEFAULT_TOKEN } from './mock-gateway.mjs';

const root = dirname(dirname(fileURLToPath(import.meta.url)));

const { CodexSession } = await import(join(root, 'dist', 'main', 'codex-session.js'));
const { GatewayClient } = await import(join(root, 'dist', 'daemon', 'gateway-client', 'client.js'));
const { TaskDispatcher } = await import(
  join(root, 'dist', 'daemon', 'gateway-client', 'dispatcher.js')
);
const { buildTaskEventFrame } = await import(
  join(root, 'dist', 'daemon', 'gateway-client', 'frames.js')
);

const workspace = mkdtempSync(join(tmpdir(), 'linxbox-e2e-'));
const port = DEFAULT_PORT;

console.log('=== 启动 mock Gateway（云端）===');
const gateway = await startMockGateway({
  port,
  token: DEFAULT_TOKEN,
  verbose: true,
});
console.log(`[gateway] 监听 ws://127.0.0.1:${port}/v1/clients/connect`);

console.log('');
console.log('=== 启动本机 Codex app-server ===');
const session = new CodexSession({ processCwd: workspace });
const probe = await session.probe();
console.log(`[desktop] provider: ${probe.binaryPath}`);
console.log(`[desktop] 版本: ${probe.version}`);
console.log(`[desktop] 账号: ${probe.authMode ?? '-'} ${probe.planType ?? ''}`.trim());

/** 本地授权项目。远端只给 project_id，路径永远留在本机。 */
const projects = new Map([['prj_demo_0001', { rootPath: workspace, enabled: true }]]);

let eventCounter = 0;
let messageCounter = 0;
const nextMessageId = () =>
  `msg_dsk_${Date.now().toString(36)}_${(messageCounter += 1).toString(36)}`;

const dispatcher = new TaskDispatcher({
  resolveProject: (projectId) => projects.get(projectId) ?? null,
  runner: {
    startTurn: (input) => session.startTurn(input.prompt, input.cwd),
    on: (listener) => session.on(listener),
    status: () => session.status(),
  },
  onEvent: (event) => {
    client.send(
      buildTaskEventFrame({
        messageId: nextMessageId(),
        sequence: 1000 + (eventCounter += 1),
        type: event.type,
        payload: event.payload,
        sentAt: new Date().toISOString(),
      }),
    );
  },
  onDisposition: (disposition, messageId, error) => {
    console.log(`[desktop] disposition=${disposition}${error ? ` (${error})` : ''}`);
  },
});

const client = new GatewayClient({
  url: `ws://127.0.0.1:${port}/v1/clients/connect`,
  token: DEFAULT_TOKEN,
  desktopVersion: '0.1.0',
  capabilities: ['project_registry', 'codex_app_server', 'task_cancel'],
  heartbeatMs: 5000,
});

client.on((event) => {
  if (event.type === 'frame') {
    void dispatcher.handle(event.frame).then((outcome) => {
      client.ack({
        messageId: event.frame.message_id,
        sequence: event.frame.sequence,
        taskId: event.frame.type === 'task.dispatch' ? event.frame.payload.task_id : 'unknown',
        disposition: outcome.disposition,
        ...(outcome.error === null
          ? {}
          : {
              error: {
                code: 'POLICY_DENIED',
                message: outcome.error,
                retryable: false,
                request_id: event.frame.message_id,
              },
            }),
      });
    });
  }
  if (event.type === 'rejected') {
    console.log(`[desktop] 拒绝帧: ${event.error.message}`);
  }
});

client.connect();

console.log('');
console.log('=== 等待远端任务执行 ===');
const deadline = Date.now() + 150_000;
let terminalSeen = false;
while (Date.now() < deadline && !terminalSeen) {
  await new Promise((resolve) => setTimeout(resolve, 500));
  const state = session.status();
  if (state.status === 'completed' || state.status === 'failed' || state.status === 'interrupted') {
    terminalSeen = true;
  }
}

const finalState = session.status();
console.log('');
console.log('=== 本机 Codex 结果 ===');
console.log(`状态: ${finalState.status}`);
console.log(`进度: ${finalState.progress || '(无)'}`);
console.log(`回答: ${finalState.finalText || '(无)'}`);
if (finalState.error) {
  console.log(`错误: ${finalState.error}`);
}

console.log('');
console.log('=== 云端收到的帧 ===');
for (const frame of gateway.received) {
  const detail =
    frame.type === 'task.ack'
      ? `disposition=${frame.payload?.disposition}`
      : frame.type === 'task.progress' ||
          frame.type === 'task.completed' ||
          frame.type === 'task.failed'
        ? `state=${frame.payload?.state} summary=${String(frame.payload?.summary ?? '').slice(0, 60)}`
        : '';
  console.log(`  ${frame.type} ${detail}`);
}

client.close();
session.close();
await gateway.close();
rmSync(workspace, { recursive: true, force: true });
process.exit(0);
