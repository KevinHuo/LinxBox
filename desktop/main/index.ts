/**
 * Electron main process entry.
 *
 * This file is wiring only: it translates Electron events into calls on the
 * pure modules under `main/` and `shared/`. The policy it applies comes from
 * `main/security.ts` and the channel list from `shared/ipc.ts`.
 */
import { app, BrowserWindow, dialog, ipcMain, screen, shell } from 'electron';
import { realpathSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  cancelTask,
  createDesktopState,
  getTask,
  grantProject,
  listProjects,
  listTasks,
  readConnectionView,
  readDiagnostics,
  revokeProject,
  saveConnection,
  type DesktopState,
} from './desktop-state.js';
import {
  isNavigationAllowed,
  isPermissionRequestAllowed,
  isWindowOpenAllowed,
  buildBrowserWindowOptions,
  buildFloatWindowOptions,
  FLOAT_PANEL_HEIGHT,
  FLOAT_PANEL_WIDTH,
  toExternalOpenUrl,
} from './security.js';
import {
  IPC_EVENT_CHANNELS,
  IPC_METHODS,
  channelFor,
  parseIpcRequest,
  type IpcMethod,
  type ProjectGrantInput,
  type TaskListInput,
} from '../shared/ipc.js';
import { createLogger } from '../shared/logger.js';
import { CodexSession } from './codex-session.js';
import { GatewayConnectionManager } from './gateway-connection.js';
import {
  clampFloatPosition,
  createAppearanceState,
  createFloatState,
  defaultFloatPosition,
  readAppearanceView,
  readFloatView,
  saveAppearance,
  FLOAT_BALL_SIZE,
  type AppearanceState,
  type FloatState,
} from './preferences.js';

const logger = createLogger('desktop.main');

// `import.meta.url` points at dist/main/index.js, so the package root is two
// levels up. The same layout holds inside the packaged app.asar.
const packageRoot = fileURLToPath(new URL('../..', import.meta.url));
const preloadPath = join(packageRoot, 'dist-preload', 'preload', 'index.js');
const rendererPath = join(packageRoot, 'dist', 'renderer', 'index.html');
const floatRendererPath = join(packageRoot, 'dist', 'renderer', 'float.html');

const state: DesktopState = createDesktopState({ desktopVersion: app.getVersion() });
const codexSession = new CodexSession();
const appearance: AppearanceState = createAppearanceState();
const floatState: FloatState = createFloatState();

/**
 * The outbound Gateway connection.
 *
 * The manager owns the socket, the reconnect timers and the dispatcher. The
 * renderer never sees a token: it only reads the connection status.
 */
const gateway = new GatewayConnectionManager({
  session: codexSession,
  desktopVersion: app.getVersion(),
  resolveProject: (projectId) => {
    const project = state.projects.get(projectId);
    if (project === undefined) {
      return null;
    }
    return { rootPath: project.rootPath, enabled: project.enabled };
  },
  onStatusChange: (status) => {
    state.connection = {
      configured: status.configured,
      connected: status.connected,
      gatewayUrl: state.connection.gatewayUrl,
      accessToken: state.connection.accessToken,
      computerName: state.connection.computerName,
    };
    if (status.lastError !== null) {
      state.lastError = status.lastError;
    }
    broadcastConnectionStatus(status);
  },
});

/**
 * Tell the renderer that the Gateway connection status changed.
 *
 * The connection state is an event, not a value the renderer has to poll for:
 * the socket can come up or drop between two reads.
 */
function broadcastConnectionStatus(status: {
  readonly configured: boolean;
  readonly connected: boolean;
  readonly lastError: string | null;
  readonly lastActivityAt: string | null;
}): void {
  const payload = { ...status };
  for (const window of BrowserWindow.getAllWindows()) {
    if (!window.isDestroyed()) {
      window.webContents.send(IPC_EVENT_CHANNELS.connectionChanged, payload);
    }
  }
}

let mainWindow: BrowserWindow | null = null;
let floatWindow: BrowserWindow | null = null;

/** Resolve the realpath of a directory chosen in the system dialog. */
function canonicalizeDirectory(candidate: string): string {
  return realpathSync(candidate);
}

function handleConnectionRead(): unknown {
  return { ok: true, data: { ...readConnectionView(state), ...gateway.status() } };
}

/**
 * Store a connection configuration and dial out to the Gateway.
 *
 * The token never leaves the main process: it is handed to the Gateway client,
 * which sends it as a WebSocket sub-protocol.
 */
function handleConnectionSave(payload: unknown): unknown {
  const parsed = parseIpcRequest('connection.save', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const input = parsed.data as { gatewayUrl: string; accessToken: string; computerName: string };
  const view = saveConnection(state, input);

  gateway.connect({
    gatewayUrl: input.gatewayUrl,
    accessToken: input.accessToken,
    computerName: input.computerName,
  });

  return { ok: true, data: { ...view, ...gateway.status() } };
}

/**
 * Report the current connection state.
 *
 * The connection is established when it is configured, so this reports what
 * the Gateway client actually knows rather than pretending a handshake
 * succeeded.
 */
function handleConnectionTest(): unknown {
  const status = gateway.status();
  if (!status.configured) {
    return { ok: false, code: 'NOT_FOUND', message: '尚未配置 Gateway 连接' };
  }
  if (status.connected) {
    return { ok: true, data: status, message: 'Gateway 已连接' };
  }
  return {
    ok: false,
    code: 'DEPENDENCY_UNAVAILABLE',
    message: status.lastError ?? 'Gateway 尚未连接',
    data: status,
  };
}

function handleProjectsList(): unknown {
  return listProjects(state);
}

async function handleProjectsGrant(payload: unknown): Promise<unknown> {
  const parsed = parseIpcRequest('projects.grant', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const input = parsed.data as ProjectGrantInput;

  const picked = await dialog.showOpenDialog({
    title: '选择允许 LinxBox 使用的本地项目',
    properties: ['openDirectory', 'createDirectory'],
  });
  const selected = picked.canceled ? undefined : picked.filePaths[0];
  if (!selected) {
    return { ok: false, code: 'NOT_FOUND', message: '未选择目录' };
  }

  let rootPath: string;
  try {
    rootPath = canonicalizeDirectory(selected);
  } catch (error) {
    logger.warn('无法解析所选目录', { reason: error instanceof Error ? error.message : 'unknown' });
    return { ok: false, code: 'INVALID_ARGUMENT', message: '所选目录不可用' };
  }

  return { ok: true, data: grantProject(state, input, rootPath) };
}

function handleProjectsRevoke(payload: unknown): unknown {
  const parsed = parseIpcRequest('projects.revoke', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { projectId } = parsed.data as { projectId: string };
  const revoked = revokeProject(state, projectId);
  return revoked
    ? { ok: true, data: listProjects(state) }
    : { ok: false, code: 'NOT_FOUND', message: '项目不存在' };
}

function handleTasksList(payload: unknown): unknown {
  const parsed = parseIpcRequest('tasks.list', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { limit } = parsed.data as TaskListInput;
  return listTasks(state, limit);
}

function handleTasksGet(payload: unknown): unknown {
  const parsed = parseIpcRequest('tasks.get', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { taskId } = parsed.data as { taskId: string };
  const task = getTask(state, taskId);
  return task ? { ok: true, data: task } : { ok: false, code: 'NOT_FOUND', message: '任务不存在' };
}

function handleTasksCancel(payload: unknown): unknown {
  const parsed = parseIpcRequest('tasks.cancel', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { taskId } = parsed.data as { taskId: string };
  const cancelled = cancelTask(state, taskId);
  return cancelled
    ? { ok: true, data: getTask(state, taskId) }
    : { ok: false, code: 'NOT_FOUND', message: '任务不存在' };
}

function handleDiagnosticsRead(): unknown {
  return readDiagnostics(state);
}

function handleAppearanceRead(): unknown {
  return { ok: true, data: readAppearanceView(appearance) };
}

function handleAppearanceSave(payload: unknown): unknown {
  const parsed = parseIpcRequest('appearance.save', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const result = saveAppearance(appearance, parsed.data as { theme: string });
  if (!result.ok) {
    return {
      ok: false,
      code: result.code ?? 'INVALID_ARGUMENT',
      message: result.message ?? '保存失败',
    };
  }
  return { ok: true, data: readAppearanceView(appearance) };
}

function handleFloatRead(): unknown {
  return { ok: true, data: readFloatView(floatState) };
}

/**
 * Show or hide the floating ball.
 *
 * The state is kept even while the window is hidden, so the settings page can
 * show the current choice and the ball can come back where the user left it.
 */
function handleFloatSetVisible(payload: unknown): unknown {
  const parsed = parseIpcRequest('window.float.setVisible', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { visible } = parsed.data as { visible: boolean };
  floatState.visible = visible;

  if (visible) {
    const window = ensureFloatWindow();
    if (floatState.position === null) {
      const workArea = screen.getPrimaryDisplay().workArea;
      floatState.position = defaultFloatPosition(workArea);
    }
    applyFloatPosition();
    window.show();
  } else if (floatWindow !== null && !floatWindow.isDestroyed()) {
    floatWindow.hide();
  }

  return { ok: true, data: readFloatView(floatState) };
}

/**
 * Move the floating ball.
 *
 * The reported position is clamped into the display work area, so a drag can
 * never leave the ball outside the reachable screen.
 */
function handleFloatMove(payload: unknown): unknown {
  const parsed = parseIpcRequest('window.float.move', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { x, y } = parsed.data as { x: number; y: number };

  if (floatWindow !== null && !floatWindow.isDestroyed()) {
    const workArea = screen.getDisplayMatching(floatWindow.getBounds()).workArea;
    floatState.position = clampFloatPosition({ x, y }, workArea);
    applyFloatPosition();
  }

  return { ok: true, data: readFloatView(floatState) };
}

/**
 * Apply the stored ball position to the panel window.
 *
 * The panel is larger than the ball: the ball sits in its bottom-right
 * corner, so the window origin is offset by the difference between the two
 * sizes.
 */
function applyFloatPosition(): void {
  if (floatWindow === null || floatWindow.isDestroyed() || floatState.position === null) {
    return;
  }
  const { x, y } = floatState.position;
  floatWindow.setPosition(
    Math.round(x - (FLOAT_PANEL_WIDTH - FLOAT_BALL_SIZE)),
    Math.round(y - (FLOAT_PANEL_HEIGHT - FLOAT_BALL_SIZE)),
  );
}

function handleCodexProbe(): unknown {
  return codexSession.probe();
}

function handleCodexTurnStatus(): unknown {
  return codexSession.status();
}

/**
 * Continue a session the user is looking at.
 *
 * This is a local user action against an existing Codex session. Codex owns
 * the session's working directory and enforces its own permissions, so the
 * Desktop must not require a second project grant here. Remote Gateway tasks
 * continue to use the separate project-authorisation boundary.
 */
async function handleCodexSessionContinue(payload: unknown): Promise<unknown> {
  const parsed = parseIpcRequest('codex.session.continue', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { threadId, prompt } = parsed.data as { threadId: string; prompt: string };

  try {
    const turn = await codexSession.continueSession(threadId, prompt);
    return { ok: true, data: turn };
  } catch (error) {
    const message = error instanceof Error ? error.message : '继续会话失败';
    logger.error('继续 Codex 会话失败', { message });
    return { ok: false, code: 'DEPENDENCY_UNAVAILABLE', message };
  }
}

async function handleCodexProjectsList(): Promise<unknown> {
  try {
    const projects = await codexSession.listProjects();
    return { ok: true, data: projects };
  } catch (error) {
    const message = error instanceof Error ? error.message : '读取项目列表失败';
    logger.error('读取 Codex 项目列表失败', { message });
    return { ok: false, code: 'DEPENDENCY_UNAVAILABLE', message };
  }
}

async function handleCodexConversationRead(payload: unknown): Promise<unknown> {
  const parsed = parseIpcRequest('codex.conversation.read', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { threadId } = parsed.data as { threadId: string };
  try {
    const conversation = await codexSession.readConversation(threadId);
    if (conversation === null) {
      return { ok: false, code: 'NOT_FOUND', message: '会话不存在' };
    }
    return { ok: true, data: conversation };
  } catch (error) {
    const message = error instanceof Error ? error.message : '读取对话详情失败';
    logger.error('读取 Codex 对话详情失败', { message });
    return { ok: false, code: 'DEPENDENCY_UNAVAILABLE', message };
  }
}

async function handleCodexSessionsList(payload: unknown): Promise<unknown> {
  const parsed = parseIpcRequest('codex.sessions.list', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { limit } = parsed.data as { limit?: number };
  try {
    const sessions = await codexSession.listSessions(limit ?? 30);
    return { ok: true, data: sessions };
  } catch (error) {
    const message = error instanceof Error ? error.message : '读取本机会话失败';
    logger.error('读取 Codex 会话列表失败', { message });
    return { ok: false, code: 'DEPENDENCY_UNAVAILABLE', message };
  }
}

async function handleCodexSessionsRead(payload: unknown): Promise<unknown> {
  const parsed = parseIpcRequest('codex.sessions.read', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { threadId } = parsed.data as { threadId: string };
  try {
    const session = await codexSession.readSession(threadId);
    if (session === null) {
      return { ok: false, code: 'NOT_FOUND', message: '会话不存在' };
    }
    return { ok: true, data: session };
  } catch (error) {
    const message = error instanceof Error ? error.message : '读取会话内容失败';
    logger.error('读取 Codex 会话内容失败', { message });
    return { ok: false, code: 'DEPENDENCY_UNAVAILABLE', message };
  }
}

async function handleCodexTurnStart(payload: unknown): Promise<unknown> {
  const parsed = parseIpcRequest('codex.turn.start', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { prompt, projectId } = parsed.data as { prompt: string; projectId: string };

  const project = state.projects.get(projectId);
  if (!project) {
    return { ok: false, code: 'NOT_FOUND', message: '项目不存在，请先授权本地项目' };
  }
  if (!project.enabled) {
    return { ok: false, code: 'PERMISSION_DENIED', message: '该项目已被停用' };
  }

  try {
    const turn = await codexSession.startTurn(prompt, project.rootPath);
    return { ok: true, data: turn };
  } catch (error) {
    const message = error instanceof Error ? error.message : '本机 Codex 调用失败';
    logger.error('启动 Codex turn 失败', { message });
    return { ok: false, code: 'DEPENDENCY_UNAVAILABLE', message };
  }
}

async function handleCodexTurnInterrupt(payload: unknown): Promise<unknown> {
  const parsed = parseIpcRequest('codex.turn.interrupt', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { turnId } = parsed.data as { turnId: string };
  try {
    await codexSession.interrupt(turnId);
    return { ok: true, data: codexSession.status() };
  } catch (error) {
    const message = error instanceof Error ? error.message : '中断失败';
    return { ok: false, code: 'DEPENDENCY_UNAVAILABLE', message };
  }
}

const HANDLERS: Readonly<Record<IpcMethod, (payload: unknown) => unknown>> = {
  'codex.probe': () => handleCodexProbe(),
  'codex.session.continue': (payload) => handleCodexSessionContinue(payload),
  'codex.projects.list': () => handleCodexProjectsList(),
  'codex.conversation.read': (payload) => handleCodexConversationRead(payload),
  'codex.sessions.list': (payload) => handleCodexSessionsList(payload),
  'codex.sessions.read': (payload) => handleCodexSessionsRead(payload),
  'codex.turn.start': (payload) => handleCodexTurnStart(payload),
  'codex.turn.status': () => handleCodexTurnStatus(),
  'codex.turn.interrupt': (payload) => handleCodexTurnInterrupt(payload),
  'connection.read': () => handleConnectionRead(),
  'connection.save': (payload) => handleConnectionSave(payload),
  'connection.test': () => handleConnectionTest(),
  'projects.list': () => handleProjectsList(),
  'projects.grant': (payload) => handleProjectsGrant(payload),
  'projects.revoke': (payload) => handleProjectsRevoke(payload),
  'tasks.list': (payload) => handleTasksList(payload),
  'tasks.get': (payload) => handleTasksGet(payload),
  'tasks.cancel': (payload) => handleTasksCancel(payload),
  'diagnostics.read': () => handleDiagnosticsRead(),
  'appearance.read': () => handleAppearanceRead(),
  'appearance.save': (payload) => handleAppearanceSave(payload),
  'window.float.read': () => handleFloatRead(),
  'window.float.setVisible': (payload) => handleFloatSetVisible(payload),
  'window.float.move': (payload) => handleFloatMove(payload),
};

/** Register one handler per allowlisted channel. */
function registerIpcHandlers(): void {
  for (const method of IPC_METHODS) {
    ipcMain.handle(channelFor(method), (_event, payload: unknown) => {
      try {
        return HANDLERS[method](payload);
      } catch (error) {
        const message = error instanceof Error ? error.message : 'unknown error';
        logger.error('IPC 处理失败', { method, message });
        state.lastError = message;
        return { ok: false, code: 'INTERNAL', message: '桌面端内部错误' };
      }
    });
  }
}

function createMainWindow(): BrowserWindow {
  const window = new BrowserWindow(buildBrowserWindowOptions({ preloadPath }));
  window.once('ready-to-show', () => {
    logger.info('主窗口已就绪', { version: state.desktopVersion });
    window.show();
  });

  window.webContents.on('did-fail-load', (_event, code, description, url) => {
    logger.error('渲染进程加载失败', { code, description, url });
  });

  window.webContents.on('did-finish-load', () => {
    logger.info('渲染进程加载完成', { url: window.webContents.getURL() });
  });

  window.webContents.on('render-process-gone', (_event, details) => {
    logger.error('渲染进程退出', { reason: details.reason });
  });

  window.webContents.on('will-navigate', (event, url) => {
    if (!isNavigationAllowed(url, window.webContents.getURL())) {
      event.preventDefault();
      logger.warn('拒绝窗口导航', { url });
    }
  });

  window.webContents.setWindowOpenHandler(({ url }) => {
    if (!isWindowOpenAllowed(url)) {
      const external = toExternalOpenUrl(url);
      if (external) {
        void shell.openExternal(external);
      }
      return { action: 'deny' };
    }
    return { action: 'deny' };
  });

  window.webContents.session.setPermissionRequestHandler((_contents, _permission, callback) => {
    callback(isPermissionRequestAllowed());
  });

  void window.loadFile(rendererPath);
  return window;
}

/**
 * Create the floating panel if it does not exist yet.
 *
 * The panel holds the ball in its bottom-right corner and the expanded card
 * above it. The window itself is transparent and frameless, so only the
 * painted content is visible on the desktop.
 */
function ensureFloatWindow(): BrowserWindow {
  if (floatWindow !== null && !floatWindow.isDestroyed()) {
    return floatWindow;
  }

  const window = new BrowserWindow(buildFloatWindowOptions({ preloadPath }));

  window.setAlwaysOnTop(true, 'floating');
  window.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });

  window.webContents.on('did-fail-load', (_event, code, description, url) => {
    logger.error('悬浮窗加载失败', { code, description, url });
  });

  window.webContents.on('will-navigate', (event, url) => {
    if (!isNavigationAllowed(url, window.webContents.getURL())) {
      event.preventDefault();
      logger.warn('拒绝悬浮窗导航', { url });
    }
  });

  window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  window.webContents.session.setPermissionRequestHandler((_contents, _permission, callback) => {
    callback(isPermissionRequestAllowed());
  });

  void window.loadFile(floatRendererPath);
  floatWindow = window;
  return window;
}

void app.whenReady().then(() => {
  registerIpcHandlers();
  mainWindow = createMainWindow();

  app.on('activate', () => {
    if (mainWindow !== null && !mainWindow.isDestroyed()) {
      mainWindow.focus();
      return;
    }
    mainWindow = createMainWindow();
  });
});

app.on('window-all-closed', () => {
  codexSession.close();
  if (process.platform !== 'darwin') {
    app.quit();
  }
});
