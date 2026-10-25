/**
 * Renderer view model for the 灵矽 AI desktop client.
 *
 * Pure functions only: the renderer owns a display model and one-shot user
 * intent, never the protocol. The DTO shapes below mirror the narrow views
 * the main process is allowed to hand over — no token, no raw provider event,
 * no path outside an authorised project.
 *
 * The module deliberately imports nothing: the renderer is loaded as a plain
 * script in a sandboxed context and must not pull in Node or a bundler.
 */

/** The views the main window can show. */
export type AppView = 'pair' | 'pairing' | 'home' | 'settings';

/** The applications listed on the outer rail. */
export type RailApp = 'box' | 'settings';

/** The appearance themes the client offers. */
export type Theme = 'warm' | 'dark';

/** Connection facts as shown by the renderer. */
export interface ConnectionFacts {
  readonly configured: boolean;
  readonly connected: boolean;
  readonly gatewayUrl: string | null;
  readonly tokenHint: string | null;
  readonly computerName: string | null;
}

/** The live status of the Gateway connection. */
export interface GatewayStatusFacts {
  readonly configured: boolean;
  readonly connected: boolean;
  readonly lastError: string | null;
  readonly lastActivityAt: string | null;
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

/** Appearance preference. */
export interface AppearanceFacts {
  readonly theme: Theme;
}

/** Floating window facts. */
export interface FloatFacts {
  readonly visible: boolean;
  readonly position: { readonly x: number; readonly y: number } | null;
}

/** The connection badge shown in the app head. */
export interface HeadPill {
  readonly on: boolean;
  readonly text: string;
}

/** One row of the recent-task list. */
export interface TaskRow {
  readonly title: string;
  readonly detail: string;
  readonly status: string;
  readonly muted: boolean;
  readonly accent: boolean;
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

/**
 * Validate a device MAC address typed by the user.
 *
 * The address is only checked for shape here. Whether the device accepts the
 * pairing request is a protocol question the Desktop does not answer yet.
 */
export function isValidMacAddress(value: string): boolean {
  return /^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(value.trim().toUpperCase());
}

/** Normalise a MAC address for display: upper case, colon separated. */
export function normalizeMacAddress(value: string): string {
  return value.trim().toUpperCase();
}

/**
 * The connection badge in the app head.
 *
 * The client is "paired" once a Gateway connection exists and is live. Until
 * the pairing contract lands, the Desktop reports the connection fact it
 * actually has instead of claiming a device handshake succeeded.
 */
export function headPill(connection: ConnectionFacts): HeadPill {
  if (connection.connected) {
    return { on: true, text: '已配对 · 设备在线' };
  }
  if (connection.configured) {
    return { on: false, text: '已配置 · 等待连接' };
  }
  return { on: false, text: '未配对 · 等待设备' };
}

/** Label of a task state for the recent-task list. */
export function taskStateLabel(state: string): string {
  switch (state) {
    case 'dispatched':
      return '已下发';
    case 'accepted':
      return '已接受';
    case 'running':
      return '运行中';
    case 'approval_required':
      return '等你回复';
    case 'cancel_requested':
      return '正在取消';
    case 'succeeded':
      return '本轮结束';
    case 'failed':
      return '失败';
    case 'interrupted':
      return '已中断';
    case 'rejected':
      return '已拒绝';
    default:
      return '进行中';
  }
}

/**
 * Build the recent-task rows.
 *
 * Each row shows the task title, the project it belongs to and its state. The
 * running ones are highlighted so the user sees what the device is driving.
 */
export function taskRows(
  tasks: readonly TaskFacts[],
  projects: readonly ProjectFacts[],
): TaskRow[] {
  return tasks.map((task) => {
    const project = projects.find((item) => item.projectId === task.projectId);
    const active = isActiveTask(task.state);
    const done = task.state === 'succeeded';
    return {
      title: task.title,
      detail: `${project?.displayName ?? '未授权项目'} · ${task.stage}`,
      status: taskStateLabel(task.state),
      muted: done,
      accent: active,
    };
  });
}

/** Count of tasks still waiting to run. */
export function pendingTaskCount(tasks: readonly TaskFacts[]): number {
  return tasks.filter((task) => isActiveTask(task.state)).length;
}

/** Whether the app head and the footer are shown for a view. */
export function isGlobalView(view: AppView): boolean {
  return view === 'settings';
}

/** Which rail entry is active for a view. */
export function railAppForView(view: AppView): RailApp {
  return view === 'settings' ? 'settings' : 'box';
}

/**
 * Clamp a dragged ball position inside the viewport.
 *
 * The renderer clamps against its own window so the ball never leaves the
 * screen while the main process clamps against the display work area.
 */
export function clampBallPosition(
  position: { readonly x: number; readonly y: number },
  bounds: { readonly width: number; readonly height: number },
  ballSize: number,
): { readonly x: number; readonly y: number } {
  const maxX = Math.max(0, bounds.width - ballSize);
  const maxY = Math.max(0, bounds.height - ballSize);
  return {
    x: Math.min(Math.max(position.x, 0), maxX),
    y: Math.min(Math.max(position.y, 0), maxY),
  };
}

/**
 * The home headline.
 *
 * The headline follows the connection fact the client actually has, so the
 * page never claims a device is online while the badge says otherwise.
 */
export function homeHeadline(connection: ConnectionFacts): string {
  return connection.connected ? '已配对 · 设备在线' : '尚未配对 · 等待设备';
}

/**
 * The device panel subtitle.
 *
 * It names the machine that runs the client, not a claim about the device.
 */
export function deviceSubtitle(connection: ConnectionFacts): string {
  return connection.connected ? '已连接 · 这台 Mac' : '尚未连接 · 这台 Mac';
}

/** Bridge exposed by the preload script, when the renderer runs inside Electron. */
export interface LinxboxBridge {
  probeCodex(): Promise<unknown>;
  listCodexSessions(payload?: unknown): Promise<unknown>;
  listCodexProjects(payload?: unknown): Promise<unknown>;
  readCodexConversation(payload: unknown): Promise<unknown>;
  continueCodexSession(payload: unknown): Promise<unknown>;
  readCodexSession(payload: unknown): Promise<unknown>;
  startCodexTurn(payload: unknown): Promise<unknown>;
  readCodexTurn(payload?: unknown): Promise<unknown>;
  interruptCodexTurn(payload: unknown): Promise<unknown>;
  readConnection(): Promise<unknown>;
  saveConnection(payload: unknown): Promise<unknown>;
  testConnection(): Promise<unknown>;
  bindDevice(payload: unknown): Promise<unknown>;
  listProjects(): Promise<unknown>;
  grantProject(payload: unknown): Promise<unknown>;
  revokeProject(payload: unknown): Promise<unknown>;
  listTasks(payload?: unknown): Promise<unknown>;
  getTask(payload: unknown): Promise<unknown>;
  cancelTask(payload: unknown): Promise<unknown>;
  readDiagnostics(): Promise<unknown>;
  readAppearance(): Promise<unknown>;
  saveAppearance(payload: unknown): Promise<unknown>;
  readFloatWindow(): Promise<unknown>;
  setFloatWindowVisible(payload: unknown): Promise<unknown>;
  moveFloatWindow(payload: unknown): Promise<unknown>;
  focusMainWindow(): Promise<unknown>;
  /** Subscribe to the Gateway connection status changes. */
  onConnectionChanged(listener: (payload: unknown) => void): () => void;
  onTaskChanged(listener: (payload: unknown) => void): () => void;
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
