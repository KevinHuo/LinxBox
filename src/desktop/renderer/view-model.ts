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

/** Diagnostics rows rendered on the settings page. */
export function diagnosticsRows(diagnostics: DiagnosticsFacts): { label: string; value: string }[] {
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
