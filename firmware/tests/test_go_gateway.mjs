/** 正式 Go Gateway + 真实 C/LVGL 客户端；Desktop 与灵矽来源由本机模拟。 */
import assert from 'node:assert/strict';
import { test } from 'node:test';
import https from 'node:https';
import net from 'node:net';
import { spawn, execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { randomBytes } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';
import { connectMockSocket } from '../tools/gateway_mock.mjs';

function request(url, ca, headers = {}, body) {
  return new Promise((resolve, reject) => {
    const req = https.request(url, { ca, headers, method: body ? 'POST' : 'GET' }, res => {
      let text = ''; res.on('data', bytes => { text += bytes; });
      res.on('end', () => resolve({ status: res.statusCode, text }));
    });
    req.on('error', reject); req.end(body);
  });
}

async function until(predicate, label) {
  const end = Date.now() + 10000;
  while (Date.now() < end) {
    const result = await predicate();
    if (result) return result;
    await delay(5);
  }
  throw new Error(`等待超时：${label}`);
}

// 仅模拟平台逐轮来源头，不代表真实灵矽平台已经支持此透传。
async function mcpSession(base, ca, token) {
  const events = [], headers = { Authorization: `Bearer ${token}` };
  let buffer = '', seq = 0;
  const stream = https.get(`${base}/mcp`, { ca, headers }, response => {
    assert.equal(response.statusCode, 200);
    response.on('data', bytes => {
      buffer += bytes;
      let end;
      while ((end = buffer.indexOf('\n\n')) >= 0) {
        const block = buffer.slice(0, end); buffer = buffer.slice(end + 2);
        const type = /^event: *(.*)$/m.exec(block)?.[1];
        const data = /^data: *(.*)$/m.exec(block)?.[1];
        if (data) events.push({ type, data });
      }
    });
  });
  stream.on('error', () => {});
  const endpoint = await until(() => events.find(e => e.type === 'endpoint')?.data, 'MCP endpoint');
  const call = async (method, params, context = {}) => {
    const id = ++seq;
    const posted = await request(new URL(endpoint, base), ca,
      { ...headers, 'Content-Type': 'application/json', ...context }, JSON.stringify({ jsonrpc: '2.0', id, method, params }));
    assert.equal(posted.status, 202);
    return until(() => events.filter(e => e.type === 'message').map(e => JSON.parse(e.data)).find(e => e.id === id), method);
  };
  await call('initialize', { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: '本机固件联调', version: '1' } });
  const initialized = await request(new URL(endpoint, base), ca, { ...headers, 'Content-Type': 'application/json' },
    JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }));
  assert.equal(initialized.status, 202);
  return { call, close() { stream.destroy(); } };
}

test('正式 Go WSS 登记绑定、MCP 原文、单次确认与执行回执', { timeout: 60000 }, async () => {
  const binary = process.env.LINX_GATEWAY_GO_BINARY;
  const hostBinary = process.env.LINX_GATEWAY_HOST_BINARY;
  assert(binary && hostBinary, '必须提供正式 Go Gateway 和 C/LVGL host');
  const directory = mkdtempSync(join(tmpdir(), 'linx-go-firmware-'));
  let gateway, host, desktop, firmware, mcp;
  const trace = [], screens = [];
  let failure, stderr = '', dtSequence = 0;
  try {
    const key = join(directory, 'key.pem'), certPath = join(directory, 'cert.pem');
    writeFileSync(join(directory, 'openssl.cnf'), '[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n');
    execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-keyout', key,
      '-out', certPath, '-config', join(directory, 'openssl.cnf')], { stdio: 'ignore' });
    const cert = readFileSync(certPath);
    const reservation = net.createServer();
    await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
    const port = reservation.address().port;
    await new Promise(resolve => reservation.close(resolve));
    const base = `https://localhost:${port}`, ws = `wss://localhost:${port}`;
    const mac = '34:00:00:00:00:01', devToken = randomBytes(24).toString('hex');
    const dtToken = randomBytes(24).toString('hex'), mcpToken = randomBytes(24).toString('hex');
    const config = join(directory, 'config.json');
    writeFileSync(config, JSON.stringify({ listen: { addr: `127.0.0.1:${port}`, tls_cert_file: certPath, tls_key_file: key },
      database: { path: join(directory, 'gateway.db') }, credentials: {
        devices: [{ token: devToken, device_id: mac, subject: 'test' }],
        desktops: [{ token: dtToken, client_id: 'test-desktop', subject: 'test' }],
        mcp: [{ token: mcpToken, device_id: mac, agent_id: 'test-agent' }] },
      pairing: { rules: [{ subject: 'test', device_ids: [mac] }] }, limits: { session_page_size: 2 } }), { mode: 0o600 });
    gateway = spawn(resolve(binary), ['--config', config], { stdio: ['ignore', 'ignore', 'pipe'] });
    gateway.stderr.on('data', bytes => { stderr += bytes; });
    await until(async () => { try { return (await request(`${base}/healthz`, cert)).status === 200; } catch { return false; } }, 'Go 服务就绪');
    firmware = await connectMockSocket(`${ws}/v1/devices/connect`, devToken, cert);
    desktop = await connectMockSocket(`${ws}/v1/clients/connect`, dtToken, cert);
    const desktopMessages = [];
    const dtSend = (type, payload) => desktop.send({ protocol_version: '1.0', message_id: `dt-${++dtSequence}`,
      sequence: dtSequence, type, sent_at: new Date().toISOString(), payload });
    desktop.on('message', text => {
      const msg = JSON.parse(text); desktopMessages.push(msg);
      if (msg.type === 'messages.list') dtSend('query.result', { reply_to_message_id: msg.message_id, ok: true,
        data: { items: [{ message_id: 'answer-1', role: 'assistant', summary: '真实 Go 路由的模拟回复' },
          { message_id: 'user-1', role: 'user', summary: '真实 Go 路由的模拟指令' }], next_cursor: null } });
      if (msg.type === 'message.content.get') dtSend('query.result', { reply_to_message_id: msg.message_id,
        ok: true, data: { text: '来自 Desktop 模拟端，经正式 Go Gateway 转发。', next_offset: null } });
    });
    host = spawn(resolve(hostBinary), [], { stdio: ['pipe', 'pipe', 'pipe'] });
    host.stderr.on('data', bytes => { stderr += bytes; });
    host.on('exit', code => { if (code) failure = new Error(stderr); });
    const input = data => { assert(!failure, String(failure)); host.stdin.write(JSON.stringify(data) + '\n'); };
    firmware.on('message', text => { const packet = JSON.parse(text); trace.push({ direction: 'server', packet }); input({ event: 'frame', generation: 1, data: packet }); });
    createInterface({ input: host.stdout }).on('line', line => {
      try {
        const event = JSON.parse(line);
        if (event.event === 'screen') screens.push(event);
        if (event.event === 'send') { trace.push({ direction: 'device', packet: event.data }); firmware.send(event.data); }
      } catch (error) { failure = error; }
    });
    const snapshot = async () => { const count = screens.length; input({ event: 'snapshot' }); return until(() => screens.length > count && screens.at(-1), '界面快照'); };
    const state = predicate => until(async () => { const view = await snapshot(); return predicate(view) && view; }, '界面状态');
    const press = key => input({ event: 'key', key });
    input({ event: 'connected', generation: 1, mac_address: mac }); input({ event: 'tick', now_ms: 1000 });
    const hello = await until(() => trace.find(t => t.direction === 'server' && t.packet.type === 'response'), '设备 hello 登记');
    assert.deepEqual(hello.packet.payload, { ok: true, data: { device_id: mac, desktop_client_id: null } });
    dtSend('device.bind', { mac_address: mac });
    const binding = await until(() => desktopMessages.find(m => m.type === 'binding.result'), '设备绑定');
    assert.equal(binding.payload.ok, true, JSON.stringify(binding.payload.error));
    dtSend('agent_sessions.sync', { items: Array.from({ length: 3 }, (_, i) => ({ agent_session_id: `s-${i}`,
      title: `正式服务任务${i}`, project_name: '服务端工程', agent: 'codex', status: 'idle', summary: '', thread_id: `t-${i}`, project_id: 'p-1' })) });
    await until(() => desktopMessages.some(m => m.type === 'agent_sessions.sync.result' && m.payload.ok), '目录同步');
    await state(s => s.bound && s.labels.join('').includes('正式服务任务'));
    press('ok'); await state(s => s.page_name === 'overview' && !s.loading);
    press('ok'); await state(s => s.page_name === 'reader' && s.labels.join('').includes('Desktop'));
    press('back'); await state(s => s.page_name === 'overview'); press('down'); press('ok');
    const recording = await state(s => s.voice_starts === 1 && s.request_status === 1);
    assert.equal(recording.voice_request_id, recording.request_id);
    assert.equal(recording.voice_session_id, recording.session_id);
    input({ event: 'voice', generation: recording.voice_generation, state: 2, error: 0 }); press('ok');
    await state(s => s.voice_finishes === 1);
    input({ event: 'voice', generation: recording.voice_generation, state: 3, error: 0 });
    mcp = await mcpSession(base, cert, mcpToken);
    const context = { 'X-Linx-Device-Id': mac, 'X-Linx-Agent-Id': 'test-agent',
      'X-Linx-Session-Id': 'simulated-lingxi', 'X-Linx-Request-Id': recording.request_id };
    const original = '  保留空格、中文和标点。\n路径\\file 🙂';
    assert(!desktopMessages.some(p => p.type === 'task.dispatch'));
    const proposed = await mcp.call('tools/call', { name: 'propose_task_reply', arguments: { text: original } }, context);
    assert.equal(Boolean(proposed.result.isError), false);
    const result = JSON.parse(proposed.result.content[0].text);
    assert.equal(result.data.text, original); assert.equal(result.data.status, 'awaiting_confirmation');
    const dispatch = await until(() => desktopMessages.find(m => m.type === 'task.dispatch'), '确认后的任务投递');
    assert.equal(dispatch.payload.prompt, original);
    assert.equal(dispatch.payload.continuation.agent_session_id, recording.session_id);
    assert.equal(dispatch.payload.continuation.thread_id, 't-0');
    assert.equal(dispatch.payload.project_id, 'p-1');
    dtSend('task.ack', { task_id: dispatch.payload.task_id, message_id: dispatch.message_id,
      sequence: dispatch.sequence, disposition: 'accepted' });
    await state(s => s.request_status === 3 && s.confirm_sent && s.page_name === 'voice');
    const invalid = await mcp.call('tools/call', { name: 'propose_task_reply', arguments: { text: original } },
      { ...context, 'X-Linx-Request-Id': 'wrong-turn' });
    assert.equal(invalid.result.isError, true);
    assert.equal(JSON.parse(invalid.result.content[0].text).error.code, 'NOT_FOUND');
    input({ event: 'voice', generation: recording.voice_generation, state: 4, error: 0 });
    assert.equal((await snapshot()).page_name, 'voice');
    input({ event: 'voice', generation: recording.voice_generation, state: 5, error: 0 });
    await state(s => s.page_name === 'overview' && s.request_status === 3 && s.confirm_sent && !s.request_error_code);
    const taskEvent = (sequence, state, result = null) => ({ task_id: dispatch.payload.task_id,
      event_id: `evt-${sequence}`, event_sequence: sequence, occurred_at: new Date().toISOString(),
      state, stage: state === 'running' ? 'preparing' : 'finished', summary: '本机模拟执行回执',
      thread_id: 't-0', turn_id: 'turn-1', result, error: null });
    dtSend('task.started', taskEvent(1, 'running'));
    await state(s => s.request_status === 4 && s.labels.join('').includes('电脑正在执行'));
    dtSend('task.completed', taskEvent(2, 'succeeded', { contract_version: 'task-result-v1',
      status: 'succeeded', summary: '本机模拟完成', changes: [], verification: [], risks: [],
      needs_user_action: null, voice_summary: '本机模拟完成' }));
    await state(s => s.request_status === 5 && s.labels.join('').includes('执行完成') && !s.request_error_code);
    const outgoing = trace.filter(t => t.direction === 'device').map(t => t.packet);
    assert(!outgoing.some(p => p.type === 'request.propose'));
    const confirmations = outgoing.filter(p => p.type === 'request.confirm');
    assert.equal(confirmations.length, 1);
    assert.deepEqual(confirmations[0].payload, { request_id: recording.request_id });
    assert(!outgoing.some(p => p.type === 'request.cancel'));
    assert(outgoing.every(p => !('limit' in p.payload)));
    assert.equal(desktopMessages.filter(p => p.type === 'task.dispatch').length, 1);
    if (process.env.LINX_GATEWAY_TEST_OUTPUT) writeFileSync(join(process.env.LINX_GATEWAY_TEST_OUTPUT, 'go-trace.json'), JSON.stringify(trace, null, 2));
  } finally {
    mcp?.close(); firmware?.close(); desktop?.close();
    if (host) { host.stdin.end(); await new Promise(resolve => host.once('exit', resolve)); }
    if (gateway) { gateway.kill('SIGTERM'); await new Promise(resolve => gateway.once('exit', resolve)); }
    rmSync(directory, { recursive: true, force: true });
  }
});
