#!/usr/bin/env node
/**
 * Codex 能力探针。
 *
 * 真连本机 Codex app-server，枚举它到底能交出什么、每种数据的真实结构长什么样，
 * 并把观测结果落成一份 JSON 报告。
 *
 * app-server 没有自描述的 method list，但���对一个不存在的方法发请求时，它会在错误
 * 信息里回"expected one of ..."，把**完整支持的方法集合**吐出来。探针用这个拿权威目录。
 *
 * 用法：
 *   pnpm build            # 探针复用构建产物
 *   node scripts/probe-codex.mjs [--out codex-capabilities.json] [--turn]
 *
 * `--turn` 会额外跑一个真实 turn（有额度时能拿到真实回答；没额度时会拿到真实的
 * provider 错误，这本身就是一份有用的数据）。
 */
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { spawn } from 'node:child_process';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = dirname(dirname(fileURLToPath(import.meta.url)));

const args = process.argv.slice(2);
const outIndex = args.indexOf('--out');
const outPath = resolve(outIndex >= 0 ? args[outIndex + 1] : 'codex-capabilities.json');
const runTurn = args.includes('--turn');

const { discoverCodexBinary, appServerArgs } = await import(
  join(root, 'dist', 'daemon', 'codex-adapter', 'discovery.js')
);
const { buildChildEnv } = await import(join(root, 'dist', 'daemon', 'codex-adapter', 'process.js'));

const binary = discoverCodexBinary();
if (binary === null) {
  console.error('未找到本机 Codex CLI');
  process.exit(1);
}
console.error(`provider: ${binary.path} (${binary.version ?? '版本未知'})`);

/* ------------------------------------------------------------------ */
/* 只读方法的样例数据                                                  */
/* ------------------------------------------------------------------ */

/**
 * 值得取样���方法。全部是只读查询，用来回答"能拿到哪些数据、什么格式"。
 * 写操作一律不在探针里执行。
 */
const READ_SAMPLES = [
  { method: 'config/read', params: { includeLayers: false } },
  { method: 'account/read', params: {} },
  { method: 'model/list', params: {} },
  { method: 'thread/list', params: { limit: 2 } },
  { method: 'project/list', params: {} },
  { method: 'skills/list', params: {} },
  { method: 'mcpServerStatus/list', params: {} },
  { method: 'permissionProfile/list', params: {} },
  { method: 'experimentalFeature/list', params: {} },
  { method: 'collaborationMode/list', params: {} },
  { method: 'account/rateLimits/read', params: {} },
  { method: 'account/usage/read', params: {} },
  { method: 'getAuthStatus', params: {} },
  { method: 'environment/info', params: {} },
  { method: 'server/diagnostics', params: {} },
];

/* ------------------------------------------------------------------ */
/* 会话                                                                */
/* ------------------------------------------------------------------ */

const workspace = mkdtempSync(join(tmpdir(), 'linxbox-probe-'));
const child = spawn(binary.path, appServerArgs(), {
  cwd: workspace,
  env: buildChildEnv(process.env),
  shell: false,
  stdio: ['pipe', 'pipe', 'pipe'],
});

let buffer = '';
let sequence = 0;
const pending = new Map();
/** 观测到的通知：method -> 样例 params */
const notifications = new Map();
/** 观测到的服务端请求 */
const serverRequests = [];

const send = (message) => child.stdin.write(`${JSON.stringify(message)}\n`);
const call = (method, params) =>
  new Promise((resolve) => {
    const id = ++sequence;
    pending.set(id, resolve);
    send({ jsonrpc: '2.0', id, method, params });
  });

child.stderr.setEncoding('utf8');
child.stderr.on('data', (chunk) => {
  for (const line of chunk.split('\n')) {
    if (line.trim().length > 0) {
      process.stderr.write(`[codex stderr] ${line}\n`);
    }
  }
});

child.stdout.setEncoding('utf8');
child.stdout.on('data', (chunk) => {
  buffer += chunk;
  let index = buffer.indexOf('\n');
  while (index !== -1) {
    const line = buffer.slice(0, index);
    buffer = buffer.slice(index + 1);
    handleLine(line);
    index = buffer.indexOf('\n');
  }
});

function handleLine(line) {
  if (line.trim().length === 0) {
    return;
  }
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    return;
  }
  if (message.id !== undefined && pending.has(message.id)) {
    pending.get(message.id)(message);
    pending.delete(message.id);
    return;
  }
  if (message.method === undefined) {
    return;
  }
  if (message.id !== undefined) {
    serverRequests.push({ method: message.method, params: message.params });
    send({ jsonrpc: '2.0', id: message.id, error: { code: -32601, message: 'probe declined' } });
    return;
  }
  if (!notifications.has(message.method)) {
    notifications.set(message.method, message.params);
  } else {
    const existing = notifications.get(message.method);
    if (Array.isArray(existing)) {
      existing.push(message.params);
    } else {
      notifications.set(message.method, [existing, message.params]);
    }
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/* ------------------------------------------------------------------ */
/* 执行                                                                */
/* ------------------------------------------------------------------ */

const init = await call('initialize', {
  clientInfo: { name: 'linxbox_probe', title: 'LinxBox Probe', version: '0.1.0' },
  capabilities: { experimentalApi: true },
});
send({ jsonrpc: '2.0', method: 'initialized', params: {} });
await sleep(600);

/**
 * 拿权威方法目录：对一个不存在的方法发请求，服务端会在错误信息里列出全部支持的方法。
 */
const catalogueResponse = await call('__linxbox_probe_unknown_method__', {});
const catalogueText = String(catalogueResponse.error?.message ?? '');
const catalogueMatch = catalogueText.match(/expected one of (.*)$/);
const supportedMethods = catalogueMatch
  ? [...catalogueText.matchAll(/`([^`]+)`/g)].map((m) => m[1])
  : [];

const samples = {};
for (const sample of READ_SAMPLES) {
  const response = await call(sample.method, sample.params);
  samples[sample.method] = response.error
    ? { error: String(response.error.message ?? '').slice(0, 300) }
    : { result: response.result };
}

let turnReport = null;
if (runTurn) {
  const thread = await call('thread/start', { cwd: workspace });
  const threadId = thread.result?.thread?.id ?? null;
  const turn = threadId
    ? await call('turn/start', {
        threadId,
        input: [
          { type: 'text', text: '只回答一句话：当前工作目录里能看到什么？不要执行任何命令。' },
        ],
        clientUserMessageId: 'probe-1',
      })
    : null;
  const turnId = turn?.result?.turn?.id ?? null;
  const deadline = Date.now() + 90_000;
  while (Date.now() < deadline && !notifications.has('turn/completed')) {
    await sleep(500);
  }
  const agentMessages = [];
  const completed = notifications.get('item/completed');
  const list = Array.isArray(completed) ? completed : completed ? [completed] : [];
  for (const item of list) {
    if (item?.item?.type === 'agentMessage') {
      agentMessages.push({ phase: item.item.phase ?? null, text: item.item.text ?? '' });
    }
  }
  turnReport = {
    threadId,
    turnId,
    turnStartResult: turn?.result ?? null,
    agentMessages,
    terminal: notifications.get('turn/completed') ?? null,
  };
}

/* ------------------------------------------------------------------ */
/* 归类                                                                */
/* ------------------------------------------------------------------ */

const CATEGORY_RULES = [
  ['会话与轮次', /^(thread\/|turn\/|review\/|getConversationSummary|gitDiffToRemote)/],
  ['文件与进程', /^(fs\/|process\/|command\/|windowsSandbox\/|fuzzyFileSearch)/],
  ['账号与额度', /^(account\/|getAuthStatus|userVerification\/)/],
  [
    '模型与配置',
    /^(model|config|permissionProfile|experimentalFeature|collaborationMode|externalAgentConfig|environment\/)/,
  ],
  ['扩展与插件', /^(plugin\/|marketplace\/|skills\/|app\/|mcpServer|hooks\/|remoteControl\/)/],
  ['项目与记忆', /^(project\/|memory\/|rollout\/|threadSection)/],
];

function categorise(method) {
  for (const [label, pattern] of CATEGORY_RULES) {
    if (pattern.test(method)) {
      return label;
    }
  }
  return '其他';
}

const categories = {};
for (const method of supportedMethods) {
  const label = categorise(method);
  categories[label] = categories[label] ?? [];
  categories[label].push(method);
}

const report = {
  generatedAt: new Date().toISOString(),
  provider: {
    binaryPath: binary.path,
    version: binary.version,
    userAgent: init.result?.userAgent ?? null,
    platformFamily: init.result?.platformFamily ?? null,
    platformOs: init.result?.platformOs ?? null,
    codexHome: init.result?.codexHome ?? null,
  },
  supportedMethodCount: supportedMethods.length,
  supportedMethods,
  methodCategories: Object.fromEntries(
    Object.entries(categories).map(([label, methods]) => [label, methods.length]),
  ),
  readSamples: samples,
  notificationCatalogue: Object.fromEntries(notifications),
  serverRequestCatalogue: serverRequests,
  turn: turnReport,
};

writeFileSync(outPath, `${JSON.stringify(report, null, 2)}\n`);

console.log('');
console.log('=== provider ===');
console.log(JSON.stringify(report.provider, null, 2));
console.log('');
console.log(`=== 权威方法目录：${supportedMethods.length} 个 ===`);
for (const [label, count] of Object.entries(report.methodCategories)) {
  console.log(`  ${label}: ${count}`);
}
console.log('');
console.log('=== 关键方法 ===');
for (const key of [
  'turn/start',
  'turn/interrupt',
  'thread/start',
  'thread/resume',
  'thread/turns/list',
  'thread/items/list',
  'thread/list',
  'thread/search',
  'project/list',
  'fs/readFile',
  'fs/readDirectory',
  'process/spawn',
  'account/read',
  'model/list',
]) {
  console.log(`  ${supportedMethods.includes(key) ? '✓' : '✗'} ${key}`);
}
console.log('');
console.log(`=== 只读样例（${Object.keys(samples).length} 个）===`);
for (const [method, sample] of Object.entries(samples)) {
  const status = 'error' in sample ? `error: ${sample.error.slice(0, 60)}` : 'ok';
  console.log(`  ${method}: ${status}`);
}
console.log('');
console.log(`=== 观测到的通知（${notifications.size} 种）===`);
for (const method of notifications.keys()) {
  console.log(`  ${method}`);
}
console.log('');
console.log(`报告已写入: ${outPath}`);

child.kill();
rmSync(workspace, { recursive: true, force: true });
process.exit(0);
