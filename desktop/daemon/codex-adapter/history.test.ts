import { describe, expect, it } from 'vitest';

import {
  findProjectForPath,
  normalizeDetailItem,
  normalizeMessage,
  normalizeProject,
  normalizeProjectList,
  normalizeTurnDetails,
  normalizeSessionDetail,
  normalizeSessionList,
  normalizeSessionSummary,
  normalizeTurn,
  normalizeTurnList,
} from './history.js';

/** Shape taken from a real `thread/list` response. */
const rawThread = {
  id: '01a16d9d-5e5f-7290-8f73-c8bc049d0737',
  environments: null,
  sessionId: '01a16d9d-5e5f-7290-8f73-c8bc049d0737',
  preview: '只回答一句话：当前工作目录里能看到什么？',
  ephemeral: false,
  modelProvider: 'openai',
  model: 'gpt-5.6-sol',
  createdAt: 1792840394,
  updatedAt: 1792840398,
  status: { type: 'notLoaded' },
  cwd: '/Users/demo/Projects/app',
  cliVersion: '0.164.0-alpha.11.1',
};

const rawUserItem = {
  type: 'userMessage',
  id: '01a16d9d-7080-7320-a050-150b34c79c83',
  clientId: 'linxbox-mvmavq5b',
  content: [{ type: 'text', text: '修复登录页空值错误', text_elements: [] }],
};

const rawAgentItem = {
  type: 'agentMessage',
  id: '01a16d9d-7081-7320-a050-150b34c79c84',
  phase: 'final_answer',
  text: '已定位表单提交前的空值处理，正在修改。',
};

describe('normalizeSessionSummary', () => {
  it('maps a real thread object', () => {
    const summary = normalizeSessionSummary(rawThread);
    expect(summary?.threadId).toBe('01a16d9d-5e5f-7290-8f73-c8bc049d0737');
    expect(summary?.title).toBe('只回答一句话：当前工作目录里能看到什么？');
    expect(summary?.cwd).toBe('/Users/demo/Projects/app');
    expect(summary?.model).toBe('gpt-5.6-sol');
    expect(summary?.status).toBe('notLoaded');
    expect(summary?.createdAt).toBe('2026-10-24T11:13:14.000Z');
  });

  it('returns null for a thread without an id', () => {
    expect(normalizeSessionSummary({ preview: 'x' })).toBeNull();
    expect(normalizeSessionSummary(null)).toBeNull();
    expect(normalizeSessionSummary('nope')).toBeNull();
  });

  it('falls back through title candidates', () => {
    expect(normalizeSessionSummary({ id: 't1', name: '命名会话' })?.title).toBe('命名会话');
    expect(normalizeSessionSummary({ id: 't1', cwd: '/a/b' })?.title).toBe('/a/b');
    expect(normalizeSessionSummary({ id: 't1' })?.title).toBe('(未命名会话)');
  });

  it('reads the cwd out of the first environment', () => {
    const summary = normalizeSessionSummary({
      id: 't1',
      environments: [{ environmentId: 'local', cwd: '/tmp/work' }],
    });
    expect(summary?.cwd).toBe('/tmp/work');
  });

  it('treats an unknown status as unknown', () => {
    expect(normalizeSessionSummary({ id: 't1', status: 'weird' })?.status).toBe('unknown');
  });
});

describe('normalizeSessionList', () => {
  it('maps the data array of a thread list', () => {
    const list = normalizeSessionList({ data: [rawThread, { preview: 'no id' }] });
    expect(list).toHaveLength(1);
    expect(list[0]?.threadId).toBe('01a16d9d-5e5f-7290-8f73-c8bc049d0737');
  });

  it('returns an empty list for an unexpected payload', () => {
    expect(normalizeSessionList(null)).toEqual([]);
    expect(normalizeSessionList({ data: 'not-an-array' })).toEqual([]);
  });
});

describe('normalizeMessage', () => {
  it('maps a user message', () => {
    const message = normalizeMessage(rawUserItem);
    expect(message?.role).toBe('user');
    expect(message?.text).toBe('修复登录页空值错误');
    expect(message?.phase).toBeNull();
  });

  it('maps an agent message and keeps its phase', () => {
    const message = normalizeMessage(rawAgentItem);
    expect(message?.role).toBe('assistant');
    expect(message?.text).toBe('已定位表单提交前的空值处理，正在修改。');
    expect(message?.phase).toBe('final_answer');
  });

  it('joins several text parts', () => {
    const message = normalizeMessage({
      type: 'userMessage',
      content: [
        { type: 'text', text: '第一段' },
        { type: 'text', text: '第二段' },
      ],
    });
    expect(message?.text).toBe('第一段\n第二段');
  });

  it('ignores items that carry no readable text', () => {
    expect(normalizeMessage({ type: 'commandExecution', text: 'ls' })).toBeNull();
    expect(normalizeMessage({ type: 'userMessage', content: [] })).toBeNull();
    expect(normalizeMessage({ type: 'userMessage', content: [{ type: 'image' }] })).toBeNull();
    expect(normalizeMessage({ type: 'agentMessage' })).toBeNull();
    expect(normalizeMessage(null)).toBeNull();
  });
});

describe('normalizeTurn', () => {
  it('keeps only readable messages', () => {
    const turn = normalizeTurn({
      id: 'tn_1',
      status: 'completed',
      startedAt: 1792840394,
      items: [rawUserItem, rawAgentItem, { type: 'commandExecution' }],
    });
    expect(turn.turnId).toBe('tn_1');
    expect(turn.status).toBe('completed');
    expect(turn.messages).toHaveLength(2);
    expect(turn.startedAt).toBe('2026-10-24T11:13:14.000Z');
  });

  it('handles a turn without items', () => {
    const turn = normalizeTurn({ startedAt: Number.POSITIVE_INFINITY });
    expect(turn.turnId).toBe('');
    expect(turn.status).toBe('unknown');
    expect(turn.startedAt).toBeNull();
    expect(turn.messages).toEqual([]);
  });
});

describe('normalizeSessionDetail', () => {
  it('maps a thread with its turns', () => {
    const detail = normalizeSessionDetail({
      thread: {
        ...rawThread,
        turns: [
          {
            id: 'tn_1',
            status: 'completed',
            startedAt: 1792840394,
            items: [rawUserItem, rawAgentItem],
          },
        ],
      },
      readState: { revision: 'x' },
    });
    expect(detail?.threadId).toBe('01a16d9d-5e5f-7290-8f73-c8bc049d0737');
    expect(detail?.turns).toHaveLength(1);
    expect(detail?.turns[0]?.messages).toHaveLength(2);
  });

  it('returns null without a thread id', () => {
    expect(normalizeSessionDetail({ thread: {} })).toBeNull();
    expect(normalizeSessionDetail(null)).toBeNull();
  });
});

describe('normalizeTurnList', () => {
  it('maps the data array of a turn list', () => {
    const turns = normalizeTurnList({ data: [{ id: 'tn_1', status: 'failed', items: [] }] });
    expect(turns).toHaveLength(1);
    expect(turns[0]?.status).toBe('failed');
  });

  it('returns an empty list for an unexpected payload', () => {
    expect(normalizeTurnList({})).toEqual([]);
  });
});

describe('normalizeProject', () => {
  const raw = {
    id: '01a16cde-31fb-7913-bf09-1fc71850316e',
    name: 'LinxBox-MVP',
    roots: [{ path: '/Users/tongjie/Desktop/LinxBox-MVP' }],
    metadata: {},
    position: 0,
    createdAt: 1792827863,
  };

  it('maps a real project object', () => {
    const project = normalizeProject(raw);
    expect(project?.projectId).toBe('01a16cde-31fb-7913-bf09-1fc71850316e');
    expect(project?.name).toBe('LinxBox-MVP');
    expect(project?.roots).toEqual(['/Users/tongjie/Desktop/LinxBox-MVP']);
    expect(project?.position).toBe(0);
  });

  it('returns null without an id', () => {
    expect(normalizeProject({ name: 'no id' })).toBeNull();
  });

  it('falls back to a placeholder name and no roots', () => {
    const project = normalizeProject({ id: 'p1', roots: [{}], position: Number.NaN });
    expect(project?.name).toBe('(未命名项目)');
    expect(project?.roots).toEqual([]);
    expect(project?.position).toBe(0);
  });
});

describe('normalizeProjectList', () => {
  it('maps and sorts by position', () => {
    const list = normalizeProjectList({
      data: [
        { id: 'p2', name: 'second', roots: [], position: 2 },
        { id: 'p1', name: 'first', roots: [], position: 1 },
      ],
    });
    expect(list.map((project) => project.name)).toEqual(['first', 'second']);
  });

  it('returns an empty list for an unexpected payload', () => {
    expect(normalizeProjectList(null)).toEqual([]);
  });

  it('drops list entries that have no project id', () => {
    expect(normalizeProjectList({ data: [{ name: 'invalid' }, { id: 'valid' }] })).toEqual([
      { projectId: 'valid', name: '(未命名项目)', roots: [], position: 0 },
    ]);
  });
});

describe('findProjectForPath', () => {
  const projects = [{ projectId: 'p1', name: 'app', roots: ['/Users/demo/app'], position: 0 }];

  it('matches the project root', () => {
    expect(findProjectForPath(projects, '/Users/demo/app')?.projectId).toBe('p1');
  });

  it('matches a directory inside the project', () => {
    expect(findProjectForPath(projects, '/Users/demo/app/src')?.projectId).toBe('p1');
  });

  it('matches inside a project root that already ends with a slash', () => {
    const withSlash = [{ projectId: 'p1', name: 'app', roots: ['/Users/demo/app/'], position: 0 }];
    expect(findProjectForPath(withSlash, '/Users/demo/app/src')?.projectId).toBe('p1');
  });

  it('does not match a sibling with a shared prefix', () => {
    expect(findProjectForPath(projects, '/Users/demo/app-other')).toBeNull();
  });

  it('returns null for an unknown or missing directory', () => {
    expect(findProjectForPath(projects, '/Users/demo/other')).toBeNull();
    expect(findProjectForPath(projects, null)).toBeNull();
  });
});

describe('normalizeDetailItem', () => {
  it('maps a user message', () => {
    const detail = normalizeDetailItem({
      type: 'userMessage',
      id: 'm1',
      content: [{ type: 'text', text: '你好' }],
    });
    expect(detail.kind).toBe('user');
    expect(detail.title).toBe('你');
    expect(detail.body).toBe('你好');
  });

  it('ignores non-text user content parts', () => {
    const detail = normalizeDetailItem({
      type: 'userMessage',
      content: [
        { type: 'image', imageUrl: 'local' },
        { type: 'text', text: '文字' },
      ],
    });
    expect(detail.body).toBe('文字');
  });

  it('marks commentary as progress', () => {
    const detail = normalizeDetailItem({
      type: 'agentMessage',
      id: 'm2',
      phase: 'commentary',
      text: '先看看',
    });
    expect(detail.kind).toBe('agent');
    expect(detail.title).toContain('过程');
  });

  it('maps a command execution with its working directory and exit code', () => {
    const detail = normalizeDetailItem({
      type: 'commandExecution',
      id: 'm3',
      command: 'ls -la',
      cwd: '/tmp/work',
      status: 'completed',
      exitCode: 0,
    });
    expect(detail.kind).toBe('command');
    expect(detail.title).toContain('completed');
    expect(detail.body).toBe('ls -la');
    expect(detail.meta).toBe('/tmp/work · exit 0');
  });

  it('maps a file change with the diff', () => {
    const detail = normalizeDetailItem({
      type: 'fileChange',
      id: 'm4',
      status: 'completed',
      changes: [{ path: '/a/b.ts', kind: { type: 'add' }, diff: '+ line' }],
    });
    expect(detail.kind).toBe('file-change');
    expect(detail.body).toContain('/a/b.ts');
    expect(detail.body).toContain('+ line');
  });

  it('maps an MCP tool call', () => {
    const detail = normalizeDetailItem({
      type: 'mcpToolCall',
      id: 'm5',
      server: 'codex_app',
      tool: 'open_in_codex',
      status: 'completed',
      arguments: { target: 'x' },
      result: { content: [{ type: 'text', text: 'ok' }] },
    });
    expect(detail.kind).toBe('mcp');
    expect(detail.title).toContain('codex_app/open_in_codex');
    expect(detail.body).toContain('target');
  });

  it('keeps an explicit marker when an MCP result cannot be serialized', () => {
    const circular: Record<string, unknown> = {};
    circular['self'] = circular;
    const detail = normalizeDetailItem({
      type: 'mcpToolCall',
      server: 'local',
      tool: 'broken',
      result: circular,
    });
    expect(detail.meta).toBe('[无法序列化的值]');
  });

  it('maps a web search', () => {
    const detail = normalizeDetailItem({
      type: 'webSearch',
      id: 'm6',
      query: 'https://example.com',
      action: { type: 'openPage', url: 'https://example.com' },
    });
    expect(detail.kind).toBe('web-search');
    expect(detail.meta).toBe('https://example.com');
  });

  it('maps reasoning, images, generation and compaction', () => {
    expect(normalizeDetailItem({ type: 'reasoning', id: 'r' }).kind).toBe('reasoning');
    expect(normalizeDetailItem({ type: 'imageView', id: 'i', path: '/a.png' }).body).toBe('/a.png');
    expect(normalizeDetailItem({ type: 'imageGeneration', id: 'g' }).kind).toBe('image-generation');
    expect(normalizeDetailItem({ type: 'contextCompaction', id: 'c' }).kind).toBe('compaction');
  });

  it('keeps an unknown type as raw json', () => {
    const detail = normalizeDetailItem({ type: 'brandNew', id: 'u', extra: 1 });
    expect(detail.kind).toBe('other');
    expect(detail.title).toBe('brandNew');
    expect(detail.body).toContain('extra');
  });

  it('keeps an empty unknown item without inventing provider data', () => {
    const detail = normalizeDetailItem(null);
    expect(detail).toEqual({ kind: 'other', id: '', title: 'unknown', body: '{}', meta: null });
    expect(normalizeDetailItem({ type: 'reasoning', summary: [null] }).body).toBe(
      '(没有可读的推理文本)',
    );
  });

  it('truncates an oversized body', () => {
    const detail = normalizeDetailItem({
      type: 'commandExecution',
      id: 'big',
      command: 'x'.repeat(50000),
      status: 'completed',
    });
    expect(detail.body.length).toBeLessThan(21000);
  });
});

describe('normalizeTurnDetails', () => {
  it('maps every item of a turn', () => {
    const details = normalizeTurnDetails({
      id: 'tn_1',
      items: [
        { type: 'userMessage', content: [{ type: 'text', text: 'a' }] },
        { type: 'agentMessage', text: 'b' },
        { type: 'commandExecution', command: 'ls' },
      ],
    });
    expect(details.map((detail) => detail.kind)).toEqual(['user', 'agent', 'command']);
  });

  it('handles a turn without items', () => {
    expect(normalizeTurnDetails({ id: 'tn_2' })).toEqual([]);
  });
});
