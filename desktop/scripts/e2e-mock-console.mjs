#!/usr/bin/env node
/**
 * 联调脚本：真实 Desktop 客户端 -> 可视化 Mock Gateway。
 *
 * 验证的是真实链路：
 *   - 真的 WebSocket over TCP；
 *   - 真的 Gateway/Desktop 1.0 帧格式与校验；
 *   - 真的本机 Codex app-server 子进程。
 *
 * 跑法：
 *   pnpm build
 *   node scripts/mock-gateway-console.mjs    # 另开一个终端
 *   node scripts/e2e-mock-console.mjs
 */
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = dirname(dirname(fileURLToPath(import.meta.url)));

const { CodexSession } = await import(join(root, 'dist', 'main', 'codex-session.js'));
const { GatewayClient } = await import(join(root, 'dist', 'daemon', 'gateway-client', 'client.js'));
const { TaskDispatcher } = await import(
  join(root, 'dist', 'daemon', 'gateway-client', 'dispatcher.js')
);
const { buildTaskEventFrame } = await import(
  join(root, 'dist', 'daemon', 'gateway-client', 'frames.js')
);

const workspace = mkdtempSync(join(tmpdir(), 'linxbox-console-e2e-'));
const port = Number(process.env.MOCK_PORT ?? 8787);
const token = process.env.MOCK_TOKEN ?? 'demo-client-token';

console.log('=== 启动本机 Codex app-server ===');
const session = new CodexSession({ processCwd: workspace });
const probe = await session.probe();
console.log(`[desktop] provider: ${probe.binaryPath}`);
console.log(`[desktop] 版本: ${probe.version}`);

const projects = new Map([['prj_demo_0001', { rootPath: workspace, enabled: true }]]);

let eventCounter = 0;
let messageCounter = 0;
const nextMessageId = () =>
  `msg_dsk_${Date.now().toString(36)}_${(messageCounter += 1).toString(36)}`;

const dispatcher = new TaskDispatcher({
  resolveProject: (projectId) => projects.get(projectId) ?? null,
  runner: {
    startTurn: (input) => session.startTurn(input.prompt, input.cwd),
    continueTurn: (input) => session.continueSession(input.threadId, input.prompt),
    sessionCwd: async (threadId) => {
      const detail = await session.readSession(threadId);
      return detail === null ? null : detail.cwd;
    },
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
    console.log(`[desktop] 事件 ${event.type} state=${event.payload?.state ?? '-'}`);
  },
  onDisposition: (disposition, messageId, error) => {
    console.log(`[desktop] disposition=${disposition}${error ? ` (${error})` : ''}`);
  },
});

const client = new GatewayClient({
  url: `ws://127.0.0.1:${port}/v1/clients/connect`,
  token,
  desktopVersion: '0.1.0',
  capabilities: ['project_registry', 'codex_app_server'],
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
      console.log(`[desktop] ack disposition=${outcome.disposition}`);
    });
  }
  if (event.type === 'rejected') {
    console.log(`[desktop] 拒绝帧: ${event.error.message}`);
  }
  if (event.type === 'connected') {
    console.log('[desktop] 已连接 mock gateway');
  }
});

console.log('=== 连接 mock gateway ===');
client.connect();

console.log('');
console.log(`控制台: http://127.0.0.1:${port}/`);
console.log('在控制台的「下达指令」面板点击「下达 task.dispatch」，观察左侧消息流。');
console.log('本脚本会等待最多 150 秒，回传事件后退出。');

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
console.log(`回答: ${finalState.finalText || '(无)'}`);
if (finalState.error) {
  console.log(`错误: ${finalState.error}`);
}

client.close();
session.close();
rmSync(workspace, { recursive: true, force: true });
process.exit(0);
