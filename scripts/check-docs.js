#!/usr/bin/env node
/**
 * LinxBox 文档与契约一致性检查
 *
 * 用法：
 *   node scripts/check-docs.js [repoPath]
 *   LINXBOX_REPO=/path/to/repo node scripts/check-docs.js
 *
 * 省略 repoPath 时默认取本脚本所在的仓库根目录（<repo>/scripts/ → <repo>）。
 *
 * 检查项：
 *   1. 相对链接与锚点可达（含仓库内 GitHub 绝对链接）
 *   2. 全部 .md / .gitattributes 为合法 UTF-8
 *   3. 契约标识版本唯一、六个 MCP 工具名与章节一一对应
 *   4. 任务状态机在架构与领域契约之间不冲突
 *   5. docs/code-structure.md 的目录树与仓库实际目录一致
 *
 * 退出码：0 全部通过；1 存在失败项。
 */

'use strict';

const fs = require('fs');
const path = require('path');

const REPO = path.resolve(
  process.argv[2] || process.env.LINXBOX_REPO || path.resolve(__dirname, '..')
);

const errors = [];
const warnings = [];
const fail = (msg) => errors.push(msg);
const warn = (msg) => warnings.push(msg);

if (!fs.existsSync(REPO)) {
  console.error(`仓库路径不存在: ${REPO}`);
  process.exit(1);
}

function walk(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (entry.name === '.git' || entry.name === 'node_modules') continue;
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(full, out);
    else out.push(full);
  }
  return out;
}

const allFiles = walk(REPO);
const mdFiles = allFiles.filter((f) => f.endsWith('.md')).sort();
const read = (f) => fs.readFileSync(f, 'utf8');
const rel = (f) => path.relative(REPO, f);

// ---------- 1. 链接与锚点 ----------

function slug(heading) {
  return heading
    .trim()
    .toLowerCase()
    .replace(/[`*]/g, '')
    .replace(/[^\p{L}\p{N}\s_-]/gu, '')
    .replace(/\s+/g, '-');
}

function headingsOf(text) {
  const set = new Set();
  for (const line of text.split('\n')) {
    const m = /^#{1,6}\s+(.*)$/.exec(line);
    if (m) set.add(slug(m[1]));
  }
  return set;
}

const LINK = /\[[^\]]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g;
const GH_BLOB = /^https:\/\/github\.com\/KevinHuo\/LinxBox\/blob\/[^/]+\/(.+)$/;
let linkCount = 0;

for (const file of mdFiles) {
  const text = read(file);
  const heads = headingsOf(text);
  for (const m of text.matchAll(LINK)) {
    const target = m[1];
    if (/^(https?:|mailto:|tel:)/.test(target)) {
      const blob = GH_BLOB.exec(target);
      if (blob && !fs.existsSync(path.join(REPO, blob[1]))) {
        fail(`${rel(file)} -> ${target}：仓库内绝对链接指向不存在的文件`);
      }
      continue;
    }
    if (target.startsWith('#')) {
      linkCount += 1;
      const anchor = target.slice(1);
      if (anchor && !heads.has(anchor)) {
        fail(`${rel(file)} -> ${target}：锚点未命中`);
      }
      continue;
    }
    const [filePart, anchor] = target.split('#');
    if (!filePart) continue;
    const resolved = path.resolve(path.dirname(file), filePart);
    linkCount += 1;
    if (!fs.existsSync(resolved)) {
      fail(`${rel(file)} -> ${target}：目标文件不存在`);
      continue;
    }
    if (anchor && resolved.endsWith('.md')) {
      if (!headingsOf(read(resolved)).has(anchor)) {
        fail(`${rel(file)} -> ${target}：目标文件中的锚点未命中`);
      }
    }
  }
}

// ---------- 2. UTF-8 ----------

for (const file of allFiles) {
  if (!/\.(md|gitattributes)$/.test(file)) continue;
  const text = read(file);
  if (text.includes('�')) {
    fail(`${rel(file)}：包含非法 UTF-8 字节（解码出现替换字符）`);
  }
}

// ---------- 3. 契约标识与工具名 ----------

const VERSIONED = [
  { re: /linxbox\.mcp\/v\d+/g, expected: 'linxbox.mcp/v1' },
  { re: /device-presentation\/v\d+/g, expected: 'device-presentation/v1' },
  { re: /task-domain\/v\d+/g, expected: 'task-domain/v1' },
  { re: /task-result\/v\d+/g, expected: 'task-result-v1' },
];

for (const { re, expected } of VERSIONED) {
  for (const file of mdFiles) {
    const text = read(file);
    for (const m of text.matchAll(re)) {
      if (m[0] !== expected) {
        fail(`${rel(file)}：契约标识 ${m[0]} 与冻结值 ${expected} 不一致`);
      }
    }
  }
}

const TOOLS = [
  'list_projects',
  'list_agents',
  'start_task',
  'get_task_status',
  'continue_task',
  'stop_task',
];

const mcpPath = path.join(REPO, 'docs/contracts/mcp.md');
if (fs.existsSync(mcpPath)) {
  const text = read(mcpPath);
  const sections = new Set();
  for (const m of text.matchAll(/^## \d+\. `([a-z_]+)`\s*$/gm)) sections.add(m[1]);
  for (const tool of TOOLS) {
    if (!sections.has(tool)) fail(`docs/contracts/mcp.md：缺少工具章节 \`${tool}\``);
  }
  for (const section of sections) {
    if (!TOOLS.includes(section)) {
      fail(`docs/contracts/mcp.md：出现未登记的工具章节 \`${section}\`（新增工具需先改契约）`);
    }
  }
}

const archPath = path.join(REPO, 'docs/architecture.md');
if (fs.existsSync(archPath)) {
  const text = read(archPath);
  for (const tool of TOOLS) {
    if (!text.includes(tool)) fail(`docs/architecture.md：未列出工具 \`${tool}\``);
  }
}

// ---------- 4. 任务状态机 ----------

const STATES = [
  'requested', 'rejected', 'queued', 'waiting_for_client', 'expired',
  'dispatched', 'accepted', 'running', 'succeeded', 'failed', 'interrupted',
  'approval_required', 'cancel_requested', 'cancelled',
];
const STOP = new Set(['any', 'non', 'terminal', 'v']);

// 状态机块可能内嵌在架构里，也可能只以指针形式引用契约。
// 按哨兵词（状态名）定位代码块，避免抓到同文件里无关的图。
function stateBlock(file, sentinels) {
  const text = read(file);
  const blocks = [...text.matchAll(/```[a-z]*\n([\s\S]*?)```/g)].map((m) => m[1]);
  return blocks.find((b) => sentinels.every((s) => b.includes(s))) || null;
}

function statesIn(block) {
  const found = new Set();
  for (const m of block.matchAll(/[a-z][a-z_]*/g)) {
    const w = m[0];
    if (!STOP.has(w)) found.add(w);
  }
  return found;
}

const SENTINELS = ['requested', 'succeeded'];
const domainBlock = stateBlock(path.join(REPO, 'docs/contracts/domain.md'), SENTINELS);
const archBlock = stateBlock(path.join(REPO, 'docs/architecture.md'), SENTINELS);

if (!domainBlock) {
  fail('docs/contracts/domain.md：找不到任务状态机代码块');
} else {
  const domainStates = statesIn(domainBlock);
  for (const s of STATES) {
    if (!domainStates.has(s)) fail(`docs/contracts/domain.md 状态机缺少 \`${s}\``);
  }
  for (const s of domainStates) {
    if (!STATES.includes(s)) fail(`docs/contracts/domain.md 状态机出现未登记状态 \`${s}\``);
  }
}

// 架构可以内嵌状态机图，也可以只引用契约；内嵌时两者必须一致。
if (archBlock && domainBlock) {
  const archStates = statesIn(archBlock);
  const domainStates = statesIn(domainBlock);
  for (const s of STATES) {
    if (!archStates.has(s)) fail(`docs/architecture.md 状态机缺少 \`${s}\``);
  }
  for (const s of archStates) {
    if (!STATES.includes(s)) fail(`docs/architecture.md 状态机出现未登记状态 \`${s}\``);
  }
  for (const s of domainStates) {
    if (!archStates.has(s)) fail(`docs/architecture.md 状态机与领域契约不一致，缺少 \`${s}\``);
  }
}

// ---------- 5. 目录树一致性 ----------

const treePath = path.join(REPO, 'docs/code-structure.md');
if (fs.existsSync(treePath)) {
  const text = read(treePath);
  const fenceStart = text.indexOf('```text');
  const tree = fenceStart < 0 ? '' : text.slice(fenceStart + 7, text.indexOf('```', fenceStart + 7));
  const expected = new Set();
  const stack = [];
  for (const line of tree.split('\n')) {
    const m = /^([│ ]*)(├──|└──) (.+)$/.exec(line);
    if (!m) continue;
    const depth = Math.floor(m[1].length / 4);
    const name = m[3].split('#')[0].trim().replace(/\/$/, '');
    stack.length = depth;
    stack.push(name);
    expected.add(stack.join('/'));
  }

  const ROOTS = ['src', 'tests'];
  const actual = new Set();
  for (const root of ROOTS) {
    const full = path.join(REPO, root);
    if (!fs.existsSync(full)) continue;
    actual.add(root);
    const recurse = (dir, prefix) => {
      for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        if (entry.name === 'node_modules' || entry.name === '.git') continue;
        if (!entry.isDirectory()) continue;
        const next = prefix ? `${prefix}/${entry.name}` : entry.name;
        actual.add(next);
        recurse(path.join(dir, entry.name), next);
      }
    };
    recurse(full, root);
  }

  for (const dir of expected) {
    if (!ROOTS.includes(dir.split('/')[0])) continue;
    if (!actual.has(dir)) fail(`目录树声明了 ${dir}，但仓库中不存在`);
  }
  for (const dir of actual) {
    if (!expected.has(dir)) fail(`仓库存在 ${dir}，但 docs/code-structure.md 的目录树未声明`);
  }
}

// ---------- 输出 ----------

console.log(`仓库: ${REPO}`);
console.log(`Markdown 文件: ${mdFiles.length}，检查链接: ${linkCount}`);
if (warnings.length) {
  console.log(`\n警告 ${warnings.length} 项:`);
  for (const w of warnings) console.log(`  [WARN] ${w}`);
}
if (errors.length) {
  console.log(`\n失败 ${errors.length} 项:`);
  for (const e of errors) console.log(`  [FAIL] ${e}`);
  process.exit(1);
}
console.log('\n全部检查通过。');
