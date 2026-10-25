#!/usr/bin/env node
/**
 * Mock Gateway（可视化版）。
 *
 * 扮演云端 Gateway：接受 Desktop 的出站 WebSocket，记录每一帧，并提供一个
 * 浏览器控制台，用来实时查看收到的消息、手动下达指令。
 *
 * 两个端点共用一个端口：
 *
 *   ws://127.0.0.1:8787/v1/clients/connect   Desktop 的出站连接
 *   http://127.0.0.1:8787/                   可视化控制台
 *
 * 控制台的实时推送走 SSE（Server-Sent Events）：只需要服务端单向推给浏览器，
 * 不必再开一条 WebSocket。
 *
 * 单独运行：
 *   node scripts/mock-gateway-console.mjs --port 8787 --token demo-client-token
 *
 * 这个脚本只用于本机联调，不做任何鉴权之外的授权判断，也不是生产实现。
 */
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { WebSocketServer } from 'ws';

const here = dirname(fileURLToPath(import.meta.url));

export const DEFAULT_PORT = 8787;
export const DEFAULT_TOKEN = 'demo-client-token';

/** Path the Desktop connects to. */
export const CONNECT_PATH = '/v1/clients/connect';

/** Maximum number of frames kept for the console. */
export const MAX_HISTORY = 500;

/**
 * Build a `task.dispatch` frame.
 *
 * The payload matches Gateway/Desktop `1.0`: the Desktop expects every field
 * of `TaskDispatchPayload` and rejects the frame when one is missing.
 */
export function buildDispatchFrame(overrides = {}) {
  const now = Date.now();
  return {
    protocol_version: '1.0',
    message_id: `msg_ui_${now.toString(36)}_${Math.floor(now % 1000)}`,
    sequence: now,
    type: 'task.dispatch',
    sent_at: new Date(now).toISOString(),
    expires_at: new Date(now + 30 * 60_000).toISOString(),
    payload: {
      task_id: `tsk_ui_${now.toString(36)}`,
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

/** Build a `task.cancel` frame. */
export function buildCancelFrame(taskId, reason = '用户在控制台取消') {
  const now = Date.now();
  return {
    protocol_version: '1.0',
    message_id: `msg_ui_${now.toString(36)}_cancel`,
    sequence: now,
    type: 'task.cancel',
    sent_at: new Date(now).toISOString(),
    payload: { task_id: taskId, reason },
  };
}

/** Acknowledge one replace-all session catalogue from the Desktop. */
export function buildAgentSessionsSyncResult(syncFrame, ok = true, message = '') {
  const now = Date.now();
  return {
    protocol_version: '1.0',
    message_id: `msg_gateway_${now.toString(36)}_sync`,
    sequence: now,
    type: 'agent_sessions.sync.result',
    sent_at: new Date(now).toISOString(),
    payload: {
      reply_to_message_id: syncFrame.message_id,
      ok,
      ...(ok
        ? {}
        : {
            error: {
              code: 'INVALID_ARGUMENT',
              message,
            },
          }),
    },
  };
}

export function buildBindingResult(bindFrame, macAddress, ok = true) {
  const now = Date.now();
  return {
    protocol_version: '1.0',
    message_id: `msg_gateway_${now.toString(36)}_bind`,
    sequence: now,
    type: 'binding.result',
    sent_at: new Date(now).toISOString(),
    payload: ok
      ? {
          reply_to_message_id: bindFrame.message_id,
          ok: true,
          data: { device_id: macAddress, desktop_client_id: 'desktop-mock' },
        }
      : {
          reply_to_message_id: bindFrame.message_id,
          ok: false,
          error: { code: 'NOT_FOUND', message: '设备未在线' },
        },
  };
}

function validateDeviceBindFrame(frame) {
  if (!frame || typeof frame !== 'object') return false;
  if (frame.protocol_version !== '1.0' || frame.type !== 'device.bind') return false;
  if (typeof frame.message_id !== 'string' || frame.message_id.length === 0) return false;
  if (!Number.isInteger(frame.sequence) || frame.sequence < 0) return false;
  if (typeof frame.sent_at !== 'string' || !frame.payload || typeof frame.payload !== 'object') return false;
  const keys = Object.keys(frame.payload);
  return keys.length === 1 && keys[0] === 'mac_address' && typeof frame.payload.mac_address === 'string';
}

/**
 * Summarise a frame for the console list.
 *
 * The console shows a one-line description per frame; the full JSON stays
 * available in the detail pane.
 */
export function summarizeFrame(frame) {
  const payload = frame?.payload ?? {};
  switch (frame?.type) {
    case 'client.hello':
      return `desktop ${payload.desktop_version ?? '-'} · 协议 ${JSON.stringify(payload.protocol_versions ?? [])} · lastAcked ${payload.last_acked_sequence ?? 0}`;
    case 'client.heartbeat':
      return `client_time ${payload.client_time ?? '-'}`;
    case 'agent_sessions.sync':
      return `会话目录 ${Array.isArray(payload.items) ? payload.items.length : 0} 项`;
    case 'agent_sessions.sync.result':
      return `会话目录保存${payload.ok ? '成功' : '失败'}`;
    case 'binding.result':
      return payload.ok
        ? `设备配对成功 · ${payload.data?.device_id ?? '-'}`
        : `设备配对失败 · ${payload.error?.code ?? '-'} · ${payload.error?.message ?? ''}`;
    case 'task.ack':
      return `task ${payload.task_id ?? '-'} · disposition=${payload.disposition ?? '-'}${payload.error ? ` · ${payload.error.message ?? ''}` : ''}`;
    case 'task.started':
    case 'task.progress':
    case 'task.completed':
    case 'task.failed':
    case 'task.cancelled':
    case 'task.interrupted':
    case 'task.approval_required':
      return `task ${payload.task_id ?? '-'} · state=${payload.state ?? '-'} · ${String(payload.summary ?? '').slice(0, 60)}`;
    case 'project.upsert':
      return `project ${payload.project_id ?? '-'}`;
    case 'project.revoke':
      return `project ${payload.project_id ?? '-'}`;
    case 'messages.list':
      return `读取会话消息 · ${payload.agent_session_id ?? '-'}`;
    case 'message.content.get':
      return `读取消息正文 · ${payload.message_id ?? '-'}`;
    case 'query.result':
      return `${payload.ok ? '查询成功' : '查询失败'} · reply_to=${payload.reply_to_message_id ?? '-'}`;
    case 'message.available':
      return `新消息 · ${payload.agent_session_id ?? '-'} · ${payload.message_id ?? '-'}`;
    default:
      return '';
  }
}

/**
 * Build a console entry for a frame that is already in the history.
 *
 * The live SSE stream sends the full entry; a page reload asks for the
 * history instead, so both paths must produce the same shape.
 */
export function toHistoryEntry(frame, direction) {
  return {
    id: `hist_${direction}_${frame?.message_id ?? Math.random().toString(36)}`,
    direction,
    type: frame?.type ?? 'unknown',
    summary: summarizeFrame(frame),
    receivedAt: frame?.sent_at ?? new Date().toISOString(),
    frame,
  };
}

/**
 * Start the mock Gateway with a browser console.
 *
 * @param {object} options
 * @param {number} [options.port]
 * @param {string} [options.token]
 * @param {(event: object) => void} [options.onFrame] called for every inbound frame
 * @param {boolean} [options.verbose] print frames to the terminal
 */
export async function startMockGatewayConsole(options = {}) {
  const port = options.port ?? DEFAULT_PORT;
  const token = options.token ?? DEFAULT_TOKEN;
  const onFrame = options.onFrame ?? (() => {});
  const verbose = options.verbose ?? false;

  /** Every frame the Desktop sent, newest last. */
  const received = [];
  /** Frames the console sent to the Desktop, newest last. */
  const sent = [];
  /** Live SSE subscribers. */
  const subscribers = new Set();
  /** Currently connected Desktop sockets. */
  const sockets = new Set();
  /** Latest full session catalogue published by the connected Desktop. */
  let agentSessions = [];
  let connectionCount = 0;

  /** Push an event to every console subscriber. */
  const publish = (event) => {
    const line = `data: ${JSON.stringify(event)}\n\n`;
    for (const response of subscribers) {
      response.write(line);
    }
  };

  const record = (frame, direction) => {
    const entry = {
      id: `${Date.now().toString(36)}_${Math.floor(Math.random() * 1e6).toString(36)}`,
      direction,
      type: frame?.type ?? 'unknown',
      summary: summarizeFrame(frame),
      receivedAt: new Date().toISOString(),
      frame,
    };
    if (direction === 'inbound') {
      received.push(frame);
      if (received.length > MAX_HISTORY) {
        received.shift();
      }
    } else {
      sent.push(frame);
      if (sent.length > MAX_HISTORY) {
        sent.shift();
      }
    }
    publish({ kind: 'frame', entry });
    return entry;
  };

  const httpServer = createServer((request, response) => {
    const url = new URL(request.url ?? '/', `http://${request.headers.host ?? 'localhost'}`);

    // ---- 控制台页面 ----
    if (url.pathname === '/' || url.pathname === '/index.html') {
      const html = readFileSync(join(here, 'mock-gateway-console.html'), 'utf8');
      response.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
      response.end(html);
      return;
    }

    // ---- 实时事件流 ----
    if (url.pathname === '/events') {
      response.writeHead(200, {
        'content-type': 'text/event-stream; charset=utf-8',
        'cache-control': 'no-cache',
        connection: 'keep-alive',
      });
      response.write(': connected\n\n');
      subscribers.add(response);
      request.on('close', () => {
        subscribers.delete(response);
      });
      return;
    }

    // ---- 状态 ----
    if (url.pathname === '/api/state') {
      response.writeHead(200, { 'content-type': 'application/json; charset=utf-8' });
      response.end(
        JSON.stringify({
          port,
          connections: sockets.size,
          totalConnections: connectionCount,
          agentSessions,
          received: received
            .slice(-100)
            .reverse()
            .map((frame) => toHistoryEntry(frame, 'inbound')),
          sent: sent
            .slice(-100)
            .reverse()
            .map((frame) => toHistoryEntry(frame, 'outbound')),
        }),
      );
      return;
    }

    // ---- 下达指令 ----
    if (url.pathname === '/api/dispatch' && request.method === 'POST') {
      let body = '';
      request.on('data', (chunk) => {
        body += chunk;
      });
      request.on('end', () => {
        let payload;
        try {
          payload = body ? JSON.parse(body) : {};
        } catch {
          response.writeHead(400, { 'content-type': 'application/json; charset=utf-8' });
          response.end(JSON.stringify({ ok: false, message: '请求体不是合法 JSON' }));
          return;
        }

        if (sockets.size === 0) {
          response.writeHead(409, { 'content-type': 'application/json; charset=utf-8' });
          response.end(JSON.stringify({ ok: false, message: '当前没有 Desktop 连接' }));
          return;
        }

        const kind = payload.kind === 'cancel' ? 'cancel' : 'dispatch';
        let frame;
        if (kind === 'cancel') {
          frame = buildCancelFrame(payload.task_id ?? '', payload.reason ?? '用户在控制台取消');
        } else {
          const requested = payload.agent_session_id;
          const selected =
            typeof requested === 'string'
              ? agentSessions.find((item) => item.agent_session_id === requested)
              : null;
          if (typeof requested === 'string' && selected === undefined) {
            response.writeHead(404, { 'content-type': 'application/json; charset=utf-8' });
            response.end(JSON.stringify({ ok: false, message: '目标 Codex 会话不存在' }));
            return;
          }
          if (
            selected &&
            (payload.project_id !== selected.project_id ||
              (typeof payload.title === 'string' && payload.title !== selected.title))
          ) {
            response.writeHead(409, { 'content-type': 'application/json; charset=utf-8' });
            response.end(JSON.stringify({ ok: false, message: '项目与对话标题已变化，请重新选择' }));
            return;
          }
          const overrides = { ...(payload.payload ?? {}) };
          if (selected) {
            overrides.project_id = selected.project_id;
            overrides.agent = selected.agent;
            overrides.continuation = {
              agent_session_id: selected.agent_session_id,
              thread_id: selected.thread_id,
            };
          }
          frame = buildDispatchFrame(overrides);
        }

        for (const socket of sockets) {
          socket.send(JSON.stringify(frame));
        }
        record(frame, 'outbound');

        response.writeHead(200, { 'content-type': 'application/json; charset=utf-8' });
        response.end(JSON.stringify({ ok: true, messageId: frame.message_id, type: frame.type }));
      });
      return;
    }

    response.writeHead(404, { 'content-type': 'application/json; charset=utf-8' });
    response.end(JSON.stringify({ ok: false, message: '未知路径' }));
  });

  const wss = new WebSocketServer({ noServer: true });

  httpServer.on('upgrade', (request, socket, head) => {
    const url = new URL(request.url ?? '/', `http://${request.headers.host ?? 'localhost'}`);
    if (url.pathname !== CONNECT_PATH) {
      socket.destroy();
      return;
    }
    wss.handleUpgrade(request, socket, head, (ws) => {
      wss.emit('connection', ws, request);
    });
  });

  wss.on('connection', (socket, request) => {
    const protocols = request.headers['sec-websocket-protocol'] ?? '';
    const tokenOk = protocols.includes(`linxbox-token.${token}`);
    connectionCount += 1;

    if (verbose) {
      console.log(
        `[gateway] 连接建立，子协议: ${protocols}，token 校验: ${tokenOk ? '通过' : '失败'}`,
      );
    }
    publish({ kind: 'connection', connected: true, tokenOk });

    if (!tokenOk) {
      socket.close(4401, 'unauthorized');
      return;
    }

    sockets.add(socket);
    publish({ kind: 'status', connections: sockets.size, totalConnections: connectionCount });

    socket.on('message', (data) => {
      let frame;
      try {
        frame = JSON.parse(data.toString());
      } catch {
        publish({ kind: 'invalid', raw: String(data).slice(0, 500) });
        return;
      }
      record(frame, 'inbound');
      onFrame(frame);
      if (frame.type === 'agent_sessions.sync') {
        const items = frame?.payload?.items;
        const valid =
          Array.isArray(items) &&
          items.every(
            (item) =>
              item &&
              typeof item.agent_session_id === 'string' &&
              typeof item.thread_id === 'string' &&
              typeof item.project_id === 'string' &&
              typeof item.title === 'string',
          );
        const reply = buildAgentSessionsSyncResult(frame, valid, valid ? '' : '会话目录格式不合法');
        if (valid) {
          agentSessions = items.map((item) => ({ ...item }));
          publish({ kind: 'sessions', items: agentSessions });
        }
        socket.send(JSON.stringify(reply));
        record(reply, 'outbound');
      }
      if (frame.type === 'device.bind') {
        const mac = String(frame?.payload?.mac_address ?? '').replace(/-/g, ':').toUpperCase();
        const valid = validateDeviceBindFrame(frame) && /^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac);
        const reply = buildBindingResult(frame, mac, valid);
        socket.send(JSON.stringify(reply));
        record(reply, 'outbound');
        publish({ kind: 'binding', mac, ok: valid });
      }
      if (verbose) {
        console.log(`[gateway] 收到 ${frame.type} ${summarizeFrame(frame)}`);
      }
    });

    socket.on('close', () => {
      sockets.delete(socket);
      publish({ kind: 'status', connections: sockets.size, totalConnections: connectionCount });
      if (verbose) {
        console.log('[gateway] 连接关闭');
      }
    });
  });

  await new Promise((resolve, reject) => {
    httpServer.once('error', reject);
    httpServer.listen(port, '127.0.0.1', resolve);
  });

  // The requested port may be 0, which asks the OS for a free port. Report
  // the port the server actually bound so callers can reach it.
  const address = httpServer.address();
  const boundPort = typeof address === 'object' && address !== null ? address.port : port;

  return {
    port: boundPort,
    received,
    sent,
    get agentSessions() {
      return agentSessions;
    },
    close: () =>
      new Promise((resolve) => {
        for (const client of sockets) {
          client.terminate();
        }
        for (const subscriber of subscribers) {
          subscriber.end();
        }
        wss.close();
        httpServer.close(() => resolve());
      }),
  };
}

const isMain = process.argv[1] && process.argv[1].endsWith('mock-gateway-console.mjs');
if (isMain) {
  const args = process.argv.slice(2);
  const portIndex = args.indexOf('--port');
  const tokenIndex = args.indexOf('--token');
  const gateway = await startMockGatewayConsole({
    port: portIndex >= 0 ? Number(args[portIndex + 1]) : DEFAULT_PORT,
    token: tokenIndex >= 0 ? args[tokenIndex + 1] : DEFAULT_TOKEN,
    verbose: true,
  });
  console.log('');
  console.log(`[gateway] 控制台   http://127.0.0.1:${gateway.port}/`);
  console.log(`[gateway] WS 入口  ws://127.0.0.1:${gateway.port}${CONNECT_PATH}`);
  console.log('');
  console.log('在客户端里配置 Gateway 地址：');
  console.log(`  ws://127.0.0.1:${gateway.port}${CONNECT_PATH}`);
  console.log(`  access_token: ${DEFAULT_TOKEN}`);
}
