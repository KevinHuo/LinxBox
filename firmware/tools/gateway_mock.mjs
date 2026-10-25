/** 本机联调 Gateway；只提供模拟数据，不执行代码或调用真实 Agent。 */
import https from 'node:https';
import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';
import { EventEmitter } from 'node:events';
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const MAX = 65536;
const utf8 = new TextDecoder('utf-8', { fatal: true });
const accept = key => createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');

/** 两端均用于本机测试。固件的 ESP WSS 实现独立测试，不依赖此适配器。 */
export class MockSocket extends EventEmitter {
  constructor(socket, client = false, head = Buffer.alloc(0)) {
    super();
    this.socket = socket;
    this.client = client;
    this.buffer = Buffer.alloc(0);
    this.parts = [];
    this.bytes = 0;
    socket.on('data', bytes => this.read(bytes));
    socket.on('error', () => this.emit('closed'));
    socket.on('close', () => this.emit('closed'));
    if (head.length) queueMicrotask(() => this.read(head));
  }
  frame(bytes, opcode = 1, fin = true) {
    bytes = Buffer.from(bytes);
    if (bytes.length > MAX) throw new RangeError('模拟帧超过 64 KiB');
    const extension = bytes.length < 126 ? 0 : bytes.length <= 65535 ? 2 : 8;
    const header = Buffer.alloc(2 + extension + (this.client ? 4 : 0));
    header[0] = (fin ? 128 : 0) | opcode;
    header[1] = (this.client ? 128 : 0) | (extension === 0 ? bytes.length : extension === 2 ? 126 : 127);
    if (extension === 2) header.writeUInt16BE(bytes.length, 2);
    if (extension === 8) header.writeBigUInt64BE(BigInt(bytes.length), 2);
    const body = Buffer.from(bytes);
    if (this.client) {
      const mask = randomBytes(4);
      mask.copy(header, header.length - 4);
      for (let i = 0; i < body.length; i++) body[i] ^= mask[i % 4];
    }
    this.socket.write(Buffer.concat([header, body]));
  }
  send(value) { this.frame(Buffer.from(JSON.stringify(value))); }
  close() { this.socket.destroy(); }
  read(bytes) {
    // A bounded frame plus transport header; reject before growing without bound.
    if (this.buffer.length + bytes.length > MAX * 2 + 28) return this.close();
    this.buffer = Buffer.concat([this.buffer, bytes]);
    while (this.buffer.length >= 2) {
      const b = this.buffer, opcode = b[0] & 15, fin = Boolean(b[0] & 128);
      const masked = Boolean(b[1] & 128);
      let length = b[1] & 127, start = 2;
      if (b[0] & 112 || masked === this.client) return this.close();
      if (length === 126) {
        if (b.length < 4) return;
        length = b.readUInt16BE(2); start = 4;
      } else if (length === 127) {
        if (b.length < 10) return;
        const large = b.readBigUInt64BE(2);
        if (large > BigInt(MAX)) return this.close();
        length = Number(large); start = 10;
      }
      if (length > MAX || (opcode >= 8 && (!fin || length > 125))) return this.close();
      const mask = masked ? 4 : 0;
      if (b.length < start + mask + length) return;
      const payload = Buffer.from(b.subarray(start + mask, start + mask + length));
      if (masked) for (let i = 0; i < payload.length; i++) payload[i] ^= b[start + i % 4];
      this.buffer = b.subarray(start + mask + length);
      if (opcode === 8) return this.close();
      if (opcode === 9) { this.frame(payload, 10); continue; }
      if (opcode === 10) continue;
      if (opcode !== 1 && opcode !== 0) return this.close();
      if ((opcode === 0 && !this.parts.length) || (opcode === 1 && this.parts.length)) return this.close();
      this.parts.push(payload); this.bytes += payload.length;
      if (this.bytes > MAX) return this.close();
      if (!fin) continue;
      const complete = Buffer.concat(this.parts);
      this.parts = []; this.bytes = 0;
      try { this.emit('message', utf8.decode(complete)); }
      catch { return this.close(); }
    }
  }
}

export function connectMockSocket(url, token, ca) {
  return new Promise((resolve, reject) => {
    const endpoint = new URL(url);
    if (endpoint.protocol === 'wss:') endpoint.protocol = 'https:';
    const key = randomBytes(16).toString('base64');
    const request = https.request(endpoint, { ca, headers: {
      Upgrade: 'websocket', Connection: 'Upgrade', 'Sec-WebSocket-Key': key,
      'Sec-WebSocket-Version': '13', Authorization: `Bearer ${token}`,
    }});
    request.on('error', reject);
    request.on('response', response => { response.resume(); reject(new Error(`握手失败 ${response.statusCode}`)); });
    request.on('upgrade', (response, socket, head) => {
      if (response.headers['sec-websocket-accept'] !== accept(key)) {
        socket.destroy(); reject(new Error('无效的 WebSocket 握手')); return;
      }
      resolve(new MockSocket(socket, true, head));
    });
    request.end();
  });
}

export function createMockGateway({ key, cert, token }) {
  if (!token || /[\r\n]/.test(token)) throw new Error('需要有效的本机模拟令牌');
  let sequence = 0, version = 1, bound = false;
  const peers = new Set(), requests = new Map(), trace = [];
  const sessions = Array.from({ length: 7 }, (_, i) => ({
    agent_session_id: `会话-${i + 1}`, title: `服务端任务${i + 1}：部署校验`,
    project_name: '联网演示项目', agent: 'codex', status: 'idle', summary: '这份摘要来自模拟服务端。',
  }));
  const bodies = new Map();
  const messages = Array.from({ length: 18 }, (_, i) => {
    const message = { message_id: `消息-${i + 1}`, role: i % 2 ? 'user' : 'assistant', summary: `服务端历史 ${i + 1}` };
    bodies.set(message.message_id, (`## 服务端正文 ${i + 1}\n\n这段内容通过真实网络读取，覆盖部署策略、编译诊断与重连校验。\n\n`).repeat(50));
    return message;
  });
  const authorized = req => {
    const provided = Buffer.from(req.headers.authorization ?? '');
    const expected = Buffer.from(`Bearer ${token}`);
    return provided.length === expected.length && timingSafeEqual(provided, expected);
  };
  const send = (peer, type, payload, replyTo) => {
    const packet = { id: `mock-${++sequence}`, type, ...(replyTo ? { reply_to: replyTo } : {}), payload };
    trace.push({ direction: 'server', packet: structuredClone(packet) }); peer.send(packet);
  };
  const notify = (type, payload) => { for (const peer of peers) if (peer.mac) send(peer, type, payload); };
  const binding = peer => ({ device_id: peer.mac, desktop_client_id: bound ? 'mock-desktop' : null });
  const failure = (peer, packet, code, message) => send(peer, 'response', { ok: false, error: { code, message } }, packet.id);
  const success = (peer, packet, data) => send(peer, 'response', { ok: true, data }, packet.id);
  const page = (items, cursor, count, prefix) => {
    let offset = 0;
    if (cursor) {
      const match = new RegExp(`^${prefix}-${count}-(\\d+)-(\\d+)$`).exec(cursor);
      if (!match || Number(match[1]) !== version) throw new Error('CONFLICT');
      offset = Number(match[2]);
    }
    return { items: items.slice(offset, offset + count), next_cursor: offset + count < items.length ? `${prefix}-${count}-${version}-${offset + count}` : null };
  };
  const receive = (peer, bytes) => {
    let packet;
    try { packet = JSON.parse(bytes); } catch { peer.close(); return; }
    trace.push({ direction: 'device', packet });
    const p = packet.payload ?? {};
    if (packet.type === 'device.hello') {
      if (!/^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(p.mac_address)) return failure(peer, packet, 'INVALID_ARGUMENT', 'MAC 格式错误');
      peer.mac = p.mac_address; return success(peer, packet, binding(peer));
    }
    if (!peer.mac || !bound) return failure(peer, packet, 'PERMISSION_DENIED', '请先在电脑端绑定设备');
    const session = sessions.find(item => item.agent_session_id === p.agent_session_id);
    try {
      switch (packet.type) {
        case 'agent_sessions.list':
          if (Object.keys(p).some(key => key !== 'cursor'))
            return failure(peer, packet, 'INVALID_ARGUMENT', '目录参数无效');
          return success(peer, packet, page(sessions, p.cursor, 2, 'sessions'));
        case 'agent_session.get':
          return session ? success(peer, packet, session) : failure(peer, packet, 'NOT_FOUND', '会话已移除');
        case 'messages.list':
          if (Object.keys(p).some(key => !['agent_session_id', 'cursor'].includes(key)))
            return failure(peer, packet, 'INVALID_ARGUMENT', '历史参数无效');
          return session ? success(peer, packet, page(messages, p.cursor, 2, 'messages')) :
            failure(peer, packet, 'NOT_FOUND', '会话不存在');
        case 'message.content.get': {
          if (!session || !bodies.has(p.message_id)) return failure(peer, packet, 'NOT_FOUND', '消息不存在');
          const body = Buffer.from(bodies.get(p.message_id)), offset = p.offset ?? 0;
          if (!Number.isSafeInteger(offset) || offset < 0 || offset > body.length || (body[offset] & 192) === 128)
            return failure(peer, packet, 'INVALID_ARGUMENT', '正文位置错误');
          let end = Math.min(offset + 1024, body.length);
          while (end < body.length && (body[end] & 192) === 128) end--;
          return success(peer, packet, { text: body.subarray(offset, end).toString('utf8'), next_offset: end < body.length ? end : null });
        }
        case 'request.prepare': {
          const old = requests.get(p.request_id);
          if (old) return old.agent_session_id === p.agent_session_id ? success(peer, packet, old) : failure(peer, packet, 'CONFLICT', '目标不可改变');
          if (!session) return failure(peer, packet, 'NOT_FOUND', '会话不存在');
          const pending = [...requests.values()].some(r => !['completed', 'failed', 'cancelled'].includes(r.status));
          if (pending) return failure(peer, packet, 'CONFLICT', '已有待处理请求');
          const request = { request_id: p.request_id, agent_session_id: session.agent_session_id, target_title: session.title, text: null, status: 'awaiting_text', error: null };
          requests.set(request.request_id, request); return success(peer, packet, request);
        }
        case 'request.get': case 'request.confirm': case 'request.cancel': {
          const request = requests.get(p.request_id);
          if (!request) return failure(peer, packet, 'NOT_FOUND', '请求不存在');
          if (packet.type === 'request.confirm')
            return failure(peer, packet, 'DEPENDENCY_UNAVAILABLE', '任务调度尚未实现，请求未执行');
          if (packet.type === 'request.cancel') {
            if (!['awaiting_text', 'awaiting_confirmation', 'cancelled'].includes(request.status))
              return failure(peer, packet, 'CONFLICT', '请求已确认，不能取消执行');
            request.status = 'cancelled';
          }
          return success(peer, packet, request);
        }
        default: return failure(peer, packet, 'INVALID_ARGUMENT', '未知操作');
      }
    } catch (error) {
      return failure(peer, packet, error.message === 'CONFLICT' ? 'CONFLICT' : 'INTERNAL', '分页已失效，请从首批刷新');
    }
  };
  const server = https.createServer({ key, cert }, (req, res) => {
    // The mock has no public control interface; mutate through its local test API.
    res.writeHead(req.url === '/healthz' ? 200 : 404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ mock: true }));
  });
  server.on('upgrade', (req, socket, head) => {
    const key = req.headers['sec-websocket-key'];
    if (req.url !== '/v1/devices/connect' || !authorized(req) || !key || req.headers['sec-websocket-version'] !== '13') {
      socket.end('HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n'); return;
    }
    socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept(key)}\r\n\r\n`);
    const peer = new MockSocket(socket, false, head); peers.add(peer);
    peer.on('message', bytes => receive(peer, bytes));
    peer.on('closed', () => peers.delete(peer));
  });
  return {
    server, trace, sessions, requests,
    async start() { await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); return `wss://localhost:${server.address().port}/v1/devices/connect`; },
    async stop() { for (const peer of peers) peer.close(); await new Promise(resolve => server.close(resolve)); },
    bind() { bound = true; for (const peer of peers) if (peer.mac) send(peer, 'device.bound', binding(peer)); },
    update() { version++; sessions[0].title = '刷新后的服务端任务'; sessions.reverse(); notify('agent_sessions.updated', {}); notify('agent_sessions.updated', {}); },
    available(sessionId, messageId = '新回复-一') {
      if (!bodies.has(messageId)) { messages.unshift({ message_id: messageId, role: 'assistant', summary: '新回复来自服务端通知' }); bodies.set(messageId, '这是新收到的最终回复。\n请保留正在阅读的旧正文。'); }
      notify('message.available', { agent_session_id: sessionId, message_id: messageId });
    },
    propose(requestId, text) {
      const r = requests.get(requestId);
      if (!r || r.status !== 'awaiting_text' || !text.trim() || Buffer.byteLength(text) > 16384 || [...text].length > 16000)
        throw new Error('模拟草稿状态或长度错误');
      r.text = text; r.status = 'awaiting_confirmation'; notify('request.proposed', r);
    },
    disconnect() { for (const peer of peers) peer.close(); },
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [certPath, keyPath] = process.argv.slice(2);
  if (!certPath || !keyPath || !process.env.LINX_GATEWAY_MOCK_TOKEN) {
    console.error('用法：LINX_GATEWAY_MOCK_TOKEN=… node tools/gateway_mock.mjs 证书.pem 私钥.pem');
    process.exitCode = 1;
  } else {
    const mock = createMockGateway({ cert: readFileSync(certPath), key: readFileSync(keyPath), token: process.env.LINX_GATEWAY_MOCK_TOKEN });
    const url = await mock.start();
    for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, () => { void mock.stop(); });
    mock.bind();
    console.log(`模拟 Gateway：${url}（仅监听本机；无真实任务执行）`);
  }
}
