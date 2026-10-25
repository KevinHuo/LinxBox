/** 真实 TLS socket → 同一 C controller → 真实 LVGL；不接私人账号。 */
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { spawn, execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';
import { createInterface } from 'node:readline';
import { randomBytes } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { connectMockSocket, createMockGateway } from '../tools/gateway_mock.mjs';

const temporary = mkdtempSync(join(tmpdir(), 'linx-gateway-tls-'));
const certPath = join(temporary, 'cert.pem'), keyPath = join(temporary, 'key.pem');
const opensslConfig = join(temporary, 'openssl.cnf');
writeFileSync(opensslConfig, '[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,digitalSignature,keyEncipherment\n');
execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
  '-keyout', keyPath, '-out', certPath, '-config', opensslConfig], { stdio: 'ignore' });
const cert = readFileSync(certPath), key = readFileSync(keyPath);
const token = randomBytes(24).toString('hex');

async function fixture(run) {
  const mock = createMockGateway({ cert, key, token });
  const url = await mock.start();
  try { await run(mock, url); } finally { await mock.stop(); }
}
function nextMessage(peer) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('等待模拟响应超时')), 5000);
    peer.once('message', text => { clearTimeout(timer); resolve(JSON.parse(text)); });
  });
}

test('认证、绑定、查询失败、幂等与 UTF-8 正文边界', async () => {
  await fixture(async (mock, url) => {
    await assert.rejects(connectMockSocket(url, 'wrong', cert), /401/);
    await assert.rejects(connectMockSocket(url, token, undefined)); // TLS 未信任必须失败。
    const peer = await connectMockSocket(url, token, cert);
    let seq = 0;
    const call = async (type, payload) => {
      const response = nextMessage(peer); peer.send({ id: `unit-${++seq}`, type, payload }); return (await response).payload;
    };
    assert.equal((await call('agent_sessions.list', {})).error.code, 'PERMISSION_DENIED');
    assert.equal((await call('device.hello', { mac_address: 'bad' })).error.code, 'INVALID_ARGUMENT');
    assert.equal((await call('device.hello', { mac_address: '34:00:00:00:00:01' })).data.desktop_client_id, null);
    const bound = nextMessage(peer); mock.bind(); assert.equal((await bound).type, 'device.bound');
    const first = await call('agent_sessions.list', {});
    assert.equal(first.data.items.length, 2);
    assert.equal((await call('agent_sessions.list', { cursor: first.data.next_cursor })).data.items[0].agent_session_id, '会话-3');
    assert.equal((await call('agent_sessions.list', { limit: 3 })).error.code, 'INVALID_ARGUMENT');
    const history = await call('messages.list', { agent_session_id: '会话-1' });
    assert.equal(history.data.items.length, 2);
    assert.deepEqual(history.data.items.map(x => x.role), ['assistant', 'user']);
    assert.equal((await call('messages.list', { agent_session_id: '会话-1', cursor: history.data.next_cursor })).data.items[0].message_id, '消息-3');
    assert.equal((await call('messages.list', { agent_session_id: '会话-1', limit: 3 })).error.code, 'INVALID_ARGUMENT');
    assert.equal((await call('agent_sessions.list', { cursor: 'invalid' })).error.code, 'CONFLICT');
    assert.equal((await call('agent_session.get', { agent_session_id: 'missing' })).error.code, 'NOT_FOUND');
    assert.equal((await call('messages.list', { agent_session_id: 'missing' })).error.code, 'NOT_FOUND');
    assert.equal((await call('message.content.get', { agent_session_id: '会话-1', message_id: 'missing' })).error.code, 'NOT_FOUND');
    assert.equal((await call('message.content.get', { agent_session_id: '会话-1', message_id: '消息-1', offset: 999999 })).error.code, 'INVALID_ARGUMENT');
    const chunk = (await call('message.content.get', { agent_session_id: '会话-1', message_id: '消息-1' })).data;
    assert(Buffer.byteLength(chunk.text) <= 1024); assert.equal(Buffer.byteLength(chunk.text), chunk.next_offset);
    assert.equal((await call('request.get', { request_id: 'missing' })).error.code, 'NOT_FOUND');
    assert.equal((await call('request.prepare', { request_id: 'r0', agent_session_id: 'missing' })).error.code, 'NOT_FOUND');
    assert.equal((await call('request.prepare', { request_id: 'r1', agent_session_id: '会话-1' })).data.status, 'awaiting_text');
    assert.equal((await call('request.prepare', { request_id: 'r1', agent_session_id: '会话-1' })).data.status, 'awaiting_text');
    assert.equal((await call('request.prepare', { request_id: 'r1', agent_session_id: '会话-2' })).error.code, 'CONFLICT');
    assert.equal((await call('request.prepare', { request_id: 'r2', agent_session_id: '会话-1' })).error.code, 'CONFLICT');
    assert.equal((await call('request.confirm', { request_id: 'r1' })).error.code, 'DEPENDENCY_UNAVAILABLE');
    assert.throws(() => mock.propose('r1', '   '));
    assert.equal((await call('request.propose', { request_id: 'r1', text: 'x' })).error.code, 'INVALID_ARGUMENT');
    const original = '原样上传："中文"\n路径\\file';
    const notification = nextMessage(peer);
    mock.propose('r1', original);
    assert.equal((await notification).payload.text, original);
    assert.equal((await call('request.get', { request_id: 'r1' })).data.text, original);
    assert.equal((await call('request.cancel', { request_id: 'r1' })).data.status, 'cancelled');
    assert.throws(() => mock.propose('r1', original));
    await call('request.prepare', { request_id: 'r3', agent_session_id: '会话-1' });
    const longDraft = '完整草稿边界。'.repeat(750);
    const longProposal = nextMessage(peer);
    mock.propose('r3', longDraft);
    assert.equal((await longProposal).type, 'request.proposed');
    const savedDraft = (await call('request.get', { request_id: 'r3' })).data;
    assert.equal(savedDraft.text, longDraft);
    assert(Buffer.byteLength(savedDraft.text) > 15000);
    assert.equal(savedDraft.status, 'awaiting_confirmation');
    assert.equal((await call('request.cancel', { request_id: 'r3' })).data.status, 'cancelled');
    assert.equal((await call('unknown', {})).error.code, 'INVALID_ARGUMENT');
    assert.throws(() => peer.frame(Buffer.alloc(65537)), RangeError);
    peer.close();
  });
});

test('实际 socket 分片、控制帧及错误帧关闭', async () => {
  await fixture(async (_mock, url) => {
    const peer = await connectMockSocket(url, token, cert);
    const hello = JSON.stringify({ id: 'fragment', type: 'device.hello', payload: { mac_address: '34:00:00:00:00:01' } });
    const response = nextMessage(peer);
    peer.frame(Buffer.from(hello.slice(0, 20)), 1, false);
    peer.frame(Buffer.from('ping'), 9); // 分片间控制包不进入文本。
    peer.frame(Buffer.from(hello.slice(20)), 0, true);
    assert.equal((await response).payload.ok, true);
    peer.frame(Buffer.from('pong'), 10);
    peer.close();
    const invalid = [
      p => p.frame(Buffer.from('binary'), 2),
      p => p.frame(Buffer.from('orphan'), 0),
      p => { p.frame(Buffer.from('first'), 1, false); p.frame(Buffer.from('second'), 1); },
      p => p.frame(Buffer.from('bad control'), 9, false),
      p => p.frame(Buffer.alloc(126), 9),
      p => p.frame(Buffer.from('closing'), 8),
      p => p.frame(Buffer.from('invalid json')),
      p => p.frame(Buffer.from([0xC0, 0xAF])),
      p => p.socket.write(Buffer.from([0x81, 0x00])), // 客户端无 mask。
      p => p.socket.write(Buffer.from([0xC1, 0x80])), // 保留位。
      p => p.socket.write(Buffer.from([0x81, 0xFF, 0, 0, 0, 0, 0, 1, 0, 1])),
    ];
    for (const send of invalid) {
      const bad = await connectMockSocket(url, token, cert);
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('错误帧没有关闭')), 5000);
        bad.once('closed', () => { clearTimeout(timer); resolve(); });
        send(bad);
      });
    }
  });
});

test('模拟服务 CLI 参数与正常退出', async () => {
  const script = fileURLToPath(new URL('../tools/gateway_mock.mjs', import.meta.url));
  await new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [script], { env: { ...process.env, LINX_GATEWAY_MOCK_TOKEN: '' } });
    child.once('error', reject); child.once('exit', code => { try { assert.equal(code, 1); resolve(); } catch (e) { reject(e); } });
  });
  await new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [script, certPath, keyPath], { env: { ...process.env, LINX_GATEWAY_MOCK_TOKEN: token } });
    child.stdout.once('data', () => child.kill('SIGTERM'));
    child.once('error', reject); child.once('exit', code => { try { assert.equal(code, 0); resolve(); } catch (e) { reject(e); } });
  });
});

test('目录、正文和推送刷新不改变阅读，单次 OK 只结束原语音', { timeout: 60000 }, async () => {
  const binary = process.env.LINX_GATEWAY_HOST_BINARY;
  assert(binary, '必须提供真实 LVGL host，不允许跳过');
  const output = resolve(process.env.LINX_GATEWAY_TEST_OUTPUT ?? join(temporary, 'evidence'));
  const screens = join(output, 'screens'); mkdirSync(screens, { recursive: true });
  await fixture(async (mock, url) => {
    const child = spawn(resolve(binary), [screens], { stdio: ['pipe', 'pipe', 'pipe'] });
    let failure, stderr = '', generation = 0, peer, clock = 1000, closing = false;
    const snapshots = [];
    child.stderr.on('data', data => { stderr += data; });
    child.on('error', error => { failure = error; });
    child.stdin.on('error', error => { if (!closing) failure = error; });
    child.on('exit', code => { if (code !== 0) failure = new Error(`host退出 ${code}: ${stderr}`); });
    const input = value => { if (failure) throw failure; child.stdin.write(JSON.stringify(value) + '\n'); };
    createInterface({ input: child.stdout }).on('line', line => {
      try {
        const event = JSON.parse(line);
        if (event.event === 'send' && event.generation === generation) peer.send(event.data);
        else if (event.event === 'screen') snapshots.push(event);
      } catch (error) { failure = error; }
    });
    const connect = async () => {
      const current = ++generation;
      peer = await connectMockSocket(url, token, cert);
      peer.on('message', text => input({ event: 'frame', generation: current, data: JSON.parse(text) }));
      let closed = false;
      peer.on('closed', () => { if (!closed && !closing) { closed = true; input({ event: 'disconnected', generation: current }); } });
      input({ event: 'connected', generation: current, mac_address: '34:00:00:00:00:01' });
      input({ event: 'tick', now_ms: clock });
    };
    const snapshot = async (name = '') => {
      const before = snapshots.length;
      input({ event: 'snapshot', name });
      const end = Date.now() + 5000;
      while (snapshots.length === before) {
        if (failure) throw failure;
        assert(Date.now() < end, `snapshot超时: ${stderr}`); await delay(2);
      }
      return snapshots.at(-1);
    };
    const wait = async predicate => {
      const end = Date.now() + 10000;
      while (true) {
        const state = await snapshot();
        if (predicate(state)) return state;
        assert(Date.now() < end, `状态未收敛: ${JSON.stringify(state)}`); await delay(5);
      }
    };
    const key = async key => { input({ event: 'key', key }); return snapshot(); };
    const calls = type => mock.trace.filter(x => x.direction === 'device' && x.packet.type === type);
    const voice = (capture, state) => input({ event: 'voice', generation: capture.voice_generation, state, error: 0 });
    try {
      await connect();
      await wait(s => !s.bound && s.connected);
      await snapshot('01-unbound');
      mock.bind();
      await wait(s => s.bound && s.labels.join(' ').includes('服务端任务1'));
      await snapshot('02-sessions');
      await key('down'); await key('down'); await key('ok');
      await wait(s => s.sessions_depth === 1 && !s.loading && s.labels.join(' ').includes('服务端任务3'));
      await snapshot('02b-session-page');
      await key('down'); await key('down'); await key('ok');
      await wait(s => s.sessions_depth === 0 && !s.loading);
      await key('ok');
      await wait(s => s.page_name === 'overview' && !s.loading && s.session_id === '会话-1');
      await snapshot('03-overview');
      await key('down'); await key('down'); await key('ok');
      await wait(s => s.page_name === 'history' && !s.loading);
      await snapshot('03b-history');
      await key('down'); await key('down'); await key('ok');
      await wait(s => s.history_depth === 1 && !s.loading);
      await key('down'); await key('down'); await key('ok');
      await wait(s => s.history_depth === 0 && !s.loading);
      await key('back'); await key('up'); await key('up'); await key('ok');
      await wait(s => s.page_name === 'reader' && !s.loading && s.message_id === '消息-1');
      await key('down');
      const reading = await snapshot('04-reader');
      mock.update(); mock.available('会话-1');
      await wait(s => s.new_reply && calls('agent_sessions.list').length >= 2 && !s.loading);
      const notified = await snapshot('05-new-reply');
      for (const field of ['message_id', 'offset', 'read_page', 'content_hash']) assert.equal(notified[field], reading[field], field);
      let state = notified;
      for (let i = 0; state.offset === 0 && i < 40; i++) state = await key('down');
      state = await wait(s => s.offset > 0 && !s.loading); await snapshot('06-next-block');
      for (let i = 0; state.offset !== 0 && i < 40; i++) state = await key('up');
      await wait(s => s.offset === 0 && !s.loading);
      assert.equal((await snapshot()).content_hash, reading.content_hash);
      await key('back'); await wait(s => s.page_name === 'overview');
      await key('down'); await key('ok');
      const capture = await wait(s => s.voice_starts === 1 && s.request_status === 1);
      assert.equal(capture.voice_request_id, capture.request_id);
      assert.equal(capture.voice_session_id, capture.session_id);
      voice(capture, 2);
      clock += 1000; input({ event: 'tick', now_ms: clock });
      assert((await snapshot('07-recording')).labels.includes('00:01'));
      await key('ok'); await wait(s => s.voice_finishes === 1);
      clock += 1000; input({ event: 'tick', now_ms: clock });
      voice(capture, 3); await snapshot('08-waiting');
      // 模拟平台在 MCP 收到原文，不从设备上传。这里只保存，不模拟发送成功。
      mock.propose(capture.request_id, '请保留原来的语音交互。');
      await wait(s => s.request_status === 2);
      assert.equal((await snapshot()).page_name, 'voice');
      await key('ok'); assert.equal((await snapshot()).voice_starts, 1);
      assert.equal(calls('request.propose').length, 0);
      assert.equal(calls('request.confirm').length, 0);
      voice(capture, 4); await snapshot('09-agent-speaking');
      assert.equal((await snapshot()).page_name, 'voice');
      voice(capture, 5); await wait(s => s.page_name === 'overview');
      await snapshot('10-returned');
      // 取消残留待确认草稿，绝不靠一次自动 confirm 绕过正式服务。
      await wait(s => mock.requests.get(capture.request_id).status === 'cancelled');
      mock.disconnect(); await wait(s => !s.connected); await snapshot('11-disconnected');
      await connect(); await wait(s => s.bound && !s.loading); await snapshot('12-reconnected');
      assert.equal(calls('request.propose').length, 0);
      assert.equal(calls('request.confirm').length, 0);
      assert(mock.trace.filter(x => x.direction === 'device').every(x => !('limit' in x.packet.payload)));
      writeFileSync(join(output, 'trace.json'), JSON.stringify(mock.trace, null, 2));
    } finally {
      closing = true; peer?.close(); child.stdin.end();
      await new Promise(resolve => child.once('exit', resolve));
    }
  });
});

process.on('exit', () => rmSync(temporary, { recursive: true, force: true }));
