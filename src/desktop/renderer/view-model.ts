/**
 * Renderer view model.
 *
 * Pure functions only: the renderer owns a display model and one-shot user
 * intent, never the protocol. The DTO shapes below mirror the narrow views
 * the main process is allowed to hand over — no token, no raw provider event,
 * no path outside an authorised project.
 *
 * The module deliberately imports nothing: the renderer is loaded as a plain
 * ES module in a sandboxed context and must not pull in Node or a bundler.
 */

/** Connection facts as shown by the renderer. */
export interface ConnectionFacts {
  readonly configured: boolean;
  readonly connected: boolean;
  readonly gatewayUrl: string | null;
  readonly tokenHint: string | null;
  readonly computerName: string | null;
}

/** A locally authorised project. */
export interface ProjectFacts {
  readonly projectId: string;
  readonly displayName: string;
  readonly rootPath: string;
  readonly capabilities: readonly string[];
  readonly grantVersion: number;
  readonly enabled: boolean;
}

/** A task row. */
export interface TaskFacts {
  readonly taskId: string;
  readonly title: string;
  readonly projectId: string;
  readonly state: string;
  readonly stage: string;
  readonly updatedAt: string;
  readonly needsAttention: boolean;
}

/** Local diagnostics. */
export interface DiagnosticsFacts {
  readonly desktopVersion: string;
  readonly protocolVersions: readonly string[];
  readonly databasePath: string | null;
  readonly connected: boolean;
  readonly lastError: string | null;
}

/** What the local Codex app-server reports about itself. */
export interface CodexProbeFacts {
  readonly available: boolean;
  readonly binaryPath: string | null;
  readonly version: string | null;
  readonly userAgent: string | null;
  readonly authMode: string | null;
  readonly planType: string | null;
  readonly error: string | null;
}

/** A session the local provider knows about. */
export interface CodexSessionFacts {
  readonly threadId: string;
  readonly title: string;
  readonly cwd: string | null;
  readonly model: string | null;
  readonly createdAt: string | null;
  readonly updatedAt: string | null;
  readonly status: string;
}

/** One message inside a session transcript. */
export interface CodexSessionMessageFacts {
  readonly id: string;
  readonly role: 'user' | 'assistant';
  readonly text: string;
  readonly phase: string | null;
}

/** A session with its transcript. */
export interface CodexSessionDetailFacts {
  readonly threadId: string;
  readonly title: string;
  readonly cwd: string | null;
  readonly turns: readonly {
    readonly turnId: string;
    readonly status: string;
    readonly startedAt: string | null;
    readonly messages: readonly CodexSessionMessageFacts[];
  }[];
}

/** The state of the turn the Desktop is driving against the local provider. */
export interface CodexTurnFacts {
  readonly threadId: string | null;
  readonly turnId: string | null;
  readonly status: 'idle' | 'running' | 'completed' | 'failed' | 'interrupted';
  readonly progress: string;
  readonly finalText: string;
  readonly error: string | null;
  readonly startedAt: string | null;
  readonly completedAt: string | null;
}

/** States that still represent work in flight. */
export const ACTIVE_TASK_STATES: readonly string[] = [
  'dispatched',
  'accepted',
  'running',
  'approval_required',
  'cancel_requested',
];

/** Whether a task state means the task is still running. */
export function isActiveTask(state: string): boolean {
  return ACTIVE_TASK_STATES.includes(state);
}

/** Filter the task list by the "running only" toggle. */
export function filterTasks(tasks: readonly TaskFacts[], runningOnly: boolean): TaskFacts[] {
  return runningOnly ? tasks.filter((task) => isActiveTask(task.state)) : [...tasks];
}

/**
 * Pick the task that should stay selected after the list changed.
 *
 * The selection falls back to the first row when the selected task is no
 * longer visible.
 */
export function selectTaskId(tasks: readonly TaskFacts[], current: string | null): string | null {
  if (current && tasks.some((task) => task.taskId === current)) {
    return current;
  }
  return tasks[0]?.taskId ?? null;
}

/** Display name of the project a task belongs to. */
export function projectLabel(projects: readonly ProjectFacts[], projectId: string): string {
  return projects.find((project) => project.projectId === projectId)?.displayName ?? projectId;
}

/** Connection indicator state and label. */
export function connectionStatus(connection: ConnectionFacts): {
  readonly state: 'online' | 'offline';
  readonly label: string;
} {
  if (connection.connected) {
    return { state: 'online', label: 'MCP 已连接' };
  }
  if (connection.configured) {
    return { state: 'offline', label: 'MCP 已配置，未连接' };
  }
  return { state: 'offline', label: 'MCP 未连接' };
}

/**
 * Shorten a project root for display.
 *
 * The user home prefix collapses to `~` so the row stays readable, and only
 * the last two path segments remain visible.
 */
export function formatRootPath(rootPath: string): string {
  const withoutHome = rootPath.replace(/^\/Users\/[^/]+/, '~');
  const segments = withoutHome.split('/').filter((segment) => segment.length > 0);
  const isHome = withoutHome.startsWith('~/');
  if (segments.length <= 2) {
    return withoutHome;
  }
  const head = isHome ? '~' : '…';
  return `${head}/${segments.slice(-2).join('/')}`;
}

/** Label for a project's enable state. */
export function projectStateLabel(project: ProjectFacts): string {
  return project.enabled ? '可供灵矽使用' : '当前已停用';
}

/** Flatten a session transcript into a chronological message list. */
export function flattenSessionMessages(
  session: CodexSessionDetailFacts,
): CodexSessionMessageFacts[] {
  const messages: CodexSessionMessageFacts[] = [];
  for (const turn of session.turns) {
    for (const message of turn.messages) {
      messages.push(message);
    }
  }
  return messages;
}

/** Format a session timestamp for display; falls back to the raw value. */
export function formatSessionTime(value: string | null): string {
  if (value === null) {
    return '-';
  }
  const parsed = Date.parse(value);
  if (Number.isNaN(parsed)) {
    return value;
  }
  return new Date(parsed).toLocaleString('zh-CN', {
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  });
}

/** Label for a Codex turn status. */
export function codexStatusLabel(status: CodexTurnFacts['status']): string {
  switch (status) {
    case 'running':
      return '执行中';
    case 'completed':
      return '已完成';
    case 'failed':
      return '失败';
    case 'interrupted':
      return '已中断';
    case 'idle':
      return '空闲';
  }
}

/** Whether a Codex turn is still in flight. */
export function isCodexTurnRunning(turn: CodexTurnFacts): boolean {
  return turn.status === 'running';
}

/** Diagnostics rows rendered on the settings page. */ export function diagnosticsRows(
  diagnostics: DiagnosticsFacts,
): { label: string; value: string }[] {
  return [
    { label: '客户端版本', value: diagnostics.desktopVersion },
    { label: '协议版本', value: diagnostics.protocolVersions.join(', ') || '-' },
    { label: '数据目录', value: diagnostics.databasePath ?? '尚未创建' },
    { label: '连接状态', value: diagnostics.connected ? '已连接' : '未连接' },
    { label: '最近错误', value: diagnostics.lastError ?? '无' },
  ];
}

/** Bridge exposed by the preload script, when the renderer runs inside Electron. */
export interface LinxboxBridge {
  probeCodex(): Promise<unknown>;
  listCodexSessions(payload?: unknown): Promise<unknown>;
  continueCodexSession(payload: unknown): Promise<unknown>;
  authorizeCodexSession(payload: unknown): Promise<unknown>;
  readCodexSession(payload: unknown): Promise<unknown>;
  startCodexTurn(payload: unknown): Promise<unknown>;
  readCodexTurn(payload?: unknown): Promise<unknown>;
  interruptCodexTurn(payload: unknown): Promise<unknown>;
  readConnection(): Promise<unknown>;
  saveConnection(payload: unknown): Promise<unknown>;
  testConnection(): Promise<unknown>;
  listProjects(): Promise<unknown>;
  grantProject(payload: unknown): Promise<unknown>;
  revokeProject(payload: unknown): Promise<unknown>;
  listTasks(payload?: unknown): Promise<unknown>;
  getTask(payload: unknown): Promise<unknown>;
  cancelTask(payload: unknown): Promise<unknown>;
  readDiagnostics(): Promise<unknown>;
}

/**
 * Read the preload bridge.
 *
 * Returns `null` when the page is opened outside Electron, which lets the
 * same renderer code run against local mock data during design review.
 */
export function getBridge(): LinxboxBridge | null {
  if (typeof window === 'undefined') {
    return null;
  }
  const candidate = (window as { linxbox?: unknown }).linxbox;
  return candidate && typeof candidate === 'object' ? (candidate as LinxboxBridge) : null;
}
