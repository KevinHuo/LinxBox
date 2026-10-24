#!/usr/bin/env node
/**
 * Mock Gateway。
 *
 * 扮演云端 Gateway：接受 Desktop 的出站 WebSocket，等 `client.hello`，然后下发一个
 * `task.dispatch`，并把 Desktop 回传的 ack 与任务事件逐条打印出来。
 *
 * 单独运行（供 Electron 客户端联调）：
 *   node scripts/mock-gateway.mjs --port 8787 --token demo-token
 *
 * 也可以被 e2e 脚本作为模块引入。
 */
import { WebSocketServer } from 'ws';

export const DEFAULT_PORT = 8787;
export const DEFAULT_TOKEN = 'demo-client-token';

/** 下发给 Desktop 的任务。 */
export function buildDispatchFrame(overrides = {}) {
  const now = Date.now();
  return {
    protocol_version: '1.0',
    message_id: 'msg_demo_0001',
    sequence: 1043,
    type: 'task.dispatch',
    sent_at: new Date(now).toISOString(),
    expires_at: new Date(now + 30 * 60_000).toISOString(),
    payload: {
      task_id: 'tsk_demo_0001',
      project_id: 'prj_demo_0001',
      agent: 'codex',
      capabilities: ['read', 'write', 'test'],
      prompt: '只回答一句话：当前工作目录里能看到什么？不要执行任何命令。',
      acceptance_criteria: ['给出一句话回答'],
      policy_version: '2026-10-24',
      continuation: null,
      ...overrides,
    },
  };
}

/**
 * 启动 mock Gateway。
 *
 * @param {object} options
 * @param {number} [options.port]
 * @param {string} [options.token]
 * @param {(event: object) => void} [options.onFrame] 每收到一帧调用一次
 * @param {boolean} [options.verbose]
 */
export async function startMockGateway(options = {}) {
  const port = options.port ?? DEFAULT_PORT;
  const token = options.token ?? DEFAULT_TOKEN;
  const onFrame = options.onFrame ?? (() => {});
  const verbose = options.verbose ?? false;

  const server = new WebSocketServer({ port, path: '/v1/clients/connect' });
  const received = [];

  await new Promise((resolve, reject) => {
    server.once('listening', resolve);
    server.once('error', reject);
  });

  server.on('connection', (socket, request) => {
    const protocols = request.headers['sec-websocket-protocol'] ?? '';
    const tokenOk = protocols.includes(`linxbox-token.${token}`);
    if (verbose) {
      console.log(
        `[gateway] 连接建立，子协议: ${protocols}，token 校验: ${tokenOk ? '通过' : '失败'}`,
      );
    }
    if (!tokenOk) {
      socket.close(4401, 'unauthorized');
      return;
    }

    socket.on('message', (data) => {
      let frame;
      try {
        frame = JSON.parse(data.toString());
      } catch {
        return;
      }
      received.push(frame);
      onFrame(frame);
      if (verbose) {
        const summary =
          frame.type === 'task.ack'
            ? `disposition=${frame.payload?.disposition}`
            : `state=${frame.payload?.state ?? '-'} summary=${String(frame.payload?.summary ?? '').slice(0, 80)}`;
        console.log(`[gateway] 收到 ${frame.type} ${summary}`);
      }
      if (frame.type === 'client.hello') {
        if (verbose) {
          console.log(
            `[gateway] hello: desktop=${frame.payload?.desktop_version} protocols=${JSON.stringify(frame.payload?.protocol_versions)} lastAcked=${frame.payload?.last_acked_sequence}`,
          );
        }
        setTimeout(() => {
          socket.send(JSON.stringify(buildDispatchFrame()));
          if (verbose) {
            console.log('[gateway] 已下发 task.dispatch (tsk_demo_0001)');
          }
        }, 200);
      }
    });
  });

  return {
    port,
    received,
    close: () =>
      new Promise((resolve) => {
        for (const client of server.clients) {
          client.terminate();
        }
        server.close(() => resolve());
      }),
  };
}

const isMain = process.argv[1] && process.argv[1].endsWith('mock-gateway.mjs');
if (isMain) {
  const args = process.argv.slice(2);
  const portIndex = args.indexOf('--port');
  const tokenIndex = args.indexOf('--token');
  const gateway = await startMockGateway({
    port: portIndex >= 0 ? Number(args[portIndex + 1]) : DEFAULT_PORT,
    token: tokenIndex >= 0 ? args[tokenIndex + 1] : DEFAULT_TOKEN,
    verbose: true,
  });
  console.log(`[gateway] 监听 ws://127.0.0.1:${gateway.port}/v1/clients/connect`);
}
