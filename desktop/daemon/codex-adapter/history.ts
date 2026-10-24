/**
 * Codex session history.
 *
 * The app-server can list the sessions it knows about and read their
 * transcripts. These are **local reads**: they query the provider's own
 * thread store and never invoke a model, so they work even when the account
 * has no model quota left.
 *
 * The transcripts are the user's own conversations. They stay on the machine:
 * nothing here may be uploaded as-is.
 */

/** A session (thread) the provider knows about. */
export interface CodexSessionSummary {
  readonly threadId: string;
  readonly title: string;
  readonly cwd: string | null;
  readonly model: string | null;
  readonly createdAt: string | null;
  readonly updatedAt: string | null;
  readonly status: string;
}

/** One message inside a session. */
export interface CodexHistoryMessage {
  readonly id: string;
  readonly role: 'user' | 'assistant';
  readonly text: string;
  /** `commentary` is progress, `final_answer` is the answer. */
  readonly phase: string | null;
}

/** One turn inside a session. */
export interface CodexHistoryTurn {
  readonly turnId: string;
  readonly status: string;
  readonly startedAt: string | null;
  readonly messages: readonly CodexHistoryMessage[];
}

/** A session with its transcript. */
export interface CodexSessionDetail {
  readonly threadId: string;
  readonly title: string;
  readonly cwd: string | null;
  readonly turns: readonly CodexHistoryTurn[];
}

function asRecord(value: unknown): Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}

function asString(value: unknown): string | null {
  return typeof value === 'string' && value.length > 0 ? value : null;
}

/**
 * Read an array without letting `Array.isArray` widen the element type to
 * `any`.
 */
function asArray(value: unknown): unknown[] {
  return Array.isArray(value) ? (value as unknown[]) : [];
}

function asNumber(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

/** Epoch seconds to an ISO timestamp. */
function toIso(seconds: number | null): string | null {
  return seconds === null ? null : new Date(seconds * 1000).toISOString();
}

/** Read the status of a session; the provider uses a tagged union. */
function readStatus(status: unknown): string {
  const record = asRecord(status);
  return asString(record['type']) ?? 'unknown';
}

/**
 * Map a raw thread object into a session summary.
 *
 * Returns `null` for a thread without an id: such a record cannot be read
 * back and is therefore useless.
 */
export function normalizeSessionSummary(raw: unknown): CodexSessionSummary | null {
  const thread = asRecord(raw);
  const threadId = asString(thread['id']);
  if (threadId === null) {
    return null;
  }
  const environments = asArray(thread['environments']);
  const first = environments[0];
  const environment = asRecord(first);
  const title =
    asString(thread['name']) ??
    asString(thread['preview']) ??
    asString(thread['cwd']) ??
    '(未命名会话)';
  return {
    threadId,
    title,
    cwd: asString(thread['cwd']) ?? asString(environment['cwd']),
    model: asString(thread['model']),
    createdAt: toIso(asNumber(thread['createdAt'])),
    updatedAt: toIso(asNumber(thread['updatedAt'])),
    status: readStatus(thread['status']),
  };
}

/** Map the raw thread list into session summaries, dropping unusable records. */
export function normalizeSessionList(raw: unknown): CodexSessionSummary[] {
  const result = asRecord(raw);
  const data = asArray(result['data']);
  const summaries: CodexSessionSummary[] = [];
  for (const item of data) {
    const summary = normalizeSessionSummary(item);
    if (summary !== null) {
      summaries.push(summary);
    }
  }
  return summaries;
}

/**
 * Map one provider item into a message.
 *
 * Only user and agent messages carry text. Command output, reasoning and file
 * change items are not part of the readable transcript.
 */
export function normalizeMessage(item: unknown): CodexHistoryMessage | null {
  const record = asRecord(item);
  const type = asString(record['type']);
  if (type === 'userMessage') {
    const content = asArray(record['content']);
    const parts: string[] = [];
    for (const part of content) {
      const text = asString(asRecord(part)['text']);
      if (text !== null) {
        parts.push(text);
      }
    }
    const text = parts.join('\n');
    if (text.length === 0) {
      return null;
    }
    return {
      id: asString(record['id']) ?? '',
      role: 'user',
      text,
      phase: null,
    };
  }
  if (type === 'agentMessage') {
    const text = asString(record['text']);
    if (text === null) {
      return null;
    }
    return {
      id: asString(record['id']) ?? '',
      role: 'assistant',
      text,
      phase: asString(record['phase']),
    };
  }
  return null;
}

/** Map a raw turn into a turn with its readable messages. */
export function normalizeTurn(raw: unknown): CodexHistoryTurn {
  const turn = asRecord(raw);
  const items = asArray(turn['items']);
  const messages: CodexHistoryMessage[] = [];
  for (const item of items) {
    const message = normalizeMessage(item);
    if (message !== null) {
      messages.push(message);
    }
  }
  return {
    turnId: asString(turn['id']) ?? '',
    status: asString(turn['status']) ?? 'unknown',
    startedAt: toIso(asNumber(turn['startedAt'])),
    messages,
  };
}

/** Map a raw thread with its turns into a session detail. */
export function normalizeSessionDetail(raw: unknown): CodexSessionDetail | null {
  const result = asRecord(raw);
  const thread = asRecord(result['thread']);
  const threadId = asString(thread['id']);
  if (threadId === null) {
    return null;
  }
  const summary = normalizeSessionSummary(thread);
  const turns = asArray(thread['turns']);
  return {
    threadId,
    title: summary?.title ?? '(未命名会话)',
    cwd: summary?.cwd ?? null,
    turns: turns.map((turn) => normalizeTurn(turn)),
  };
}

/** Map a raw turn list into turns with their readable messages. */
export function normalizeTurnList(raw: unknown): CodexHistoryTurn[] {
  const result = asRecord(raw);
  const data = asArray(result['data']);
  return data.map((turn) => normalizeTurn(turn));
}

/* ------------------------------------------------------------------ */
/* 项目                                                                */
/* ------------------------------------------------------------------ */

/** Codex 自己记录的项目。 */
export interface CodexProjectSummary {
  readonly projectId: string;
  readonly name: string;
  /** 项目的根目录。 */
  readonly roots: readonly string[];
  readonly position: number;
}

/** Map a raw project object into a project summary. */
export function normalizeProject(raw: unknown): CodexProjectSummary | null {
  const project = asRecord(raw);
  const projectId = asString(project['id']);
  if (projectId === null) {
    return null;
  }
  const rawRoots = asArray(project['roots']);
  const roots: string[] = [];
  for (const root of rawRoots) {
    const path = asString(asRecord(root)['path']);
    if (path !== null) {
      roots.push(path);
    }
  }
  return {
    projectId,
    name: asString(project['name']) ?? '(未命名项目)',
    roots,
    position: asNumber(project['position']) ?? 0,
  };
}

/** Map a raw project list into project summaries. */
export function normalizeProjectList(raw: unknown): CodexProjectSummary[] {
  const result = asRecord(raw);
  const data = asArray(result['data']);
  const projects: CodexProjectSummary[] = [];
  for (const item of data) {
    const project = normalizeProject(item);
    if (project !== null) {
      projects.push(project);
    }
  }
  return projects.sort((left, right) => left.position - right.position);
}

/**
 * Find the project a session belongs to.
 *
 * A session belongs to a project when its working directory is the project
 * root or lives inside it. A session outside every project belongs to none.
 */
export function findProjectForPath(
  projects: readonly CodexProjectSummary[],
  directory: string | null,
): CodexProjectSummary | null {
  if (directory === null) {
    return null;
  }
  for (const project of projects) {
    for (const root of project.roots) {
      if (directory === root) {
        return project;
      }
      const prefix = root.endsWith('/') ? root : `${root}/`;
      if (directory.startsWith(prefix)) {
        return project;
      }
    }
  }
  return null;
}

/* ------------------------------------------------------------------ */
/* 对话详情                                                            */
/* ------------------------------------------------------------------ */

/** The kind of a detail entry inside a conversation. */
export const DETAIL_KINDS = [
  'user',
  'agent',
  'reasoning',
  'command',
  'file-change',
  'mcp',
  'web-search',
  'image',
  'image-generation',
  'compaction',
  'other',
] as const;

export type CodexDetailKind = (typeof DETAIL_KINDS)[number];

/** One entry inside a conversation. */
export interface CodexDetailItem {
  readonly kind: CodexDetailKind;
  readonly id: string;
  /** One-line summary. */
  readonly title: string;
  /** The detail body. May be long. */
  readonly body: string;
  /** Extra facts such as the working directory or the exit code. */
  readonly meta: string | null;
}

const MAX_BODY = 20000;

/** Cap a body so one huge diff cannot blow up the renderer. */
function cap(text: string): string {
  return text.length > MAX_BODY ? `${text.slice(0, MAX_BODY)}\n…(已截断)` : text;
}

function stringify(value: unknown): string {
  if (value === null || value === undefined) {
    return '';
  }
  if (typeof value === 'string') {
    return value;
  }
  try {
    return JSON.stringify(value, null, 2) ?? '';
  } catch {
    return '[无法序列化的值]';
  }
}

/**
 * Map one provider item into a detail entry.
 *
 * Every item type the provider emits is mapped to a kind with a readable
 * title. Unknown types are kept as raw JSON so nothing is silently dropped.
 */
export function normalizeDetailItem(item: unknown): CodexDetailItem {
  const record = asRecord(item);
  const type = asString(record['type']) ?? 'unknown';
  const id = asString(record['id']) ?? '';

  switch (type) {
    case 'userMessage': {
      const content = asArray(record['content']);
      const parts: string[] = [];
      for (const part of content) {
        const text = asString(asRecord(part)['text']);
        if (text !== null) {
          parts.push(text);
        }
      }
      return { kind: 'user', id, title: '你', body: cap(parts.join('\n')), meta: null };
    }

    case 'agentMessage': {
      const phase = asString(record['phase']);
      return {
        kind: 'agent',
        id,
        title: phase === 'commentary' ? 'Codex · 过程' : 'Codex',
        body: cap(asString(record['text']) ?? ''),
        meta: null,
      };
    }

    case 'reasoning': {
      const summary = asArray(record['summary']);
      const content = asArray(record['content']);
      const parts = [...summary, ...content]
        .map((part) => stringify(part))
        .filter((part) => part.length > 0);
      return {
        kind: 'reasoning',
        id,
        title: '推理',
        body: parts.length > 0 ? cap(parts.join('\n')) : '(没有可读的推理文本)',
        meta: null,
      };
    }

    case 'commandExecution': {
      const status = asString(record['status']) ?? 'unknown';
      const exitCode = asNumber(record['exitCode']);
      const meta = [asString(record['cwd']), exitCode === null ? null : `exit ${exitCode}`]
        .filter((part): part is string => part !== null)
        .join(' · ');
      return {
        kind: 'command',
        id,
        title: `命令 · ${status}`,
        body: cap(asString(record['command']) ?? ''),
        meta: meta.length > 0 ? meta : null,
      };
    }

    case 'fileChange': {
      const changes = asArray(record['changes']);
      const parts: string[] = [];
      for (const change of changes) {
        const entry = asRecord(change);
        const path = asString(entry['path']) ?? '(未知文件)';
        const kind = asString(asRecord(entry['kind'])['type']) ?? 'modify';
        const diff = asString(entry['diff']) ?? '';
        parts.push(`### ${path} (${kind})\n${diff}`);
      }
      const status = asString(record['status']);
      return {
        kind: 'file-change',
        id,
        title: `文件改动 · ${changes.length} 个${status === null ? '' : ` · ${status}`}`,
        body: cap(parts.join('\n\n')),
        meta: null,
      };
    }

    case 'mcpToolCall': {
      const server = asString(record['server']) ?? 'unknown';
      const tool = asString(record['tool']) ?? 'unknown';
      const status = asString(record['status']) ?? 'unknown';
      const result = record['result'];
      const error = record['error'];
      return {
        kind: 'mcp',
        id,
        title: `MCP · ${server}/${tool} · ${status}`,
        body: cap(stringify(record['arguments'])),
        meta:
          error !== null && error !== undefined ? cap(stringify(error)) : cap(stringify(result)),
      };
    }

    case 'webSearch': {
      const action = asRecord(record['action']);
      return {
        kind: 'web-search',
        id,
        title: '搜索',
        body: cap(asString(record['query']) ?? ''),
        meta: asString(action['url']) ?? asString(action['type']),
      };
    }

    case 'imageView': {
      return {
        kind: 'image',
        id,
        title: '图片',
        body: asString(record['path']) ?? '',
        meta: null,
      };
    }

    case 'imageGeneration': {
      return {
        kind: 'image-generation',
        id,
        title: `图片生成 · ${asString(record['status']) ?? 'unknown'}`,
        body: cap(asString(record['revisedPrompt']) ?? ''),
        meta: asString(record['savedPath']),
      };
    }

    case 'contextCompaction': {
      return { kind: 'compaction', id, title: '上下文压缩', body: '', meta: null };
    }

    default: {
      return {
        kind: 'other',
        id,
        title: type,
        body: cap(stringify(record)),
        meta: null,
      };
    }
  }
}

/** Map a raw turn into its detail entries. */
export function normalizeTurnDetails(raw: unknown): CodexDetailItem[] {
  const turn = asRecord(raw);
  const items = asArray(turn['items']);
  const details: CodexDetailItem[] = [];
  for (const item of items) {
    details.push(normalizeDetailItem(item));
  }
  return details;
}

/** A conversation with its full detail entries. */
export interface CodexConversation {
  readonly threadId: string;
  readonly title: string;
  readonly cwd: string | null;
  readonly updatedAt: string | null;
  readonly turns: readonly {
    readonly turnId: string;
    readonly status: string;
    readonly startedAt: string | null;
    readonly details: readonly CodexDetailItem[];
  }[];
}
