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
