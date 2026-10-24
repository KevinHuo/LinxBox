/**
 * Electron main process entry.
 *
 * This file is wiring only: it translates Electron events into calls on the
 * pure modules under `main/` and `shared/`. The policy it applies comes from
 * `main/security.ts` and the channel list from `shared/ipc.ts`.
 */
import { app, BrowserWindow, dialog, ipcMain, shell } from 'electron';
import { realpathSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  cancelTask,
  createDesktopState,
  findAuthorizedProject,
  getTask,
  grantProject,
  listProjects,
  listTasks,
  readConnectionView,
  readDiagnostics,
  revokeProject,
  saveConnection,
  testConnection,
  type DesktopState,
} from './desktop-state.js';
import {
  isNavigationAllowed,
  isPermissionRequestAllowed,
  isWindowOpenAllowed,
  buildBrowserWindowOptions,
  toExternalOpenUrl,
} from './security.js';
import {
  IPC_METHODS,
  channelFor,
  parseIpcRequest,
  type IpcMethod,
  type ProjectGrantInput,
  type TaskListInput,
} from '../shared/ipc.js';
import { createLogger } from '../shared/logger.js';
import { CodexSession } from './codex-session.js';

const logger = createLogger('desktop.main');

// `import.meta.url` points at dist/main/index.js, so the package root is two
// levels up. The same layout holds inside the packaged app.asar.
const packageRoot = fileURLToPath(new URL('../..', import.meta.url));
const preloadPath = join(packageRoot, 'dist-preload', 'preload', 'index.js');
const rendererPath = join(packageRoot, 'dist', 'renderer', 'index.html');

const state: DesktopState = createDesktopState({ desktopVersion: app.getVersion() });
const codexSession = new CodexSession();

let mainWindow: BrowserWindow | null = null;

/** Resolve the realpath of a directory chosen in the system dialog. */
function canonicalizeDirectory(candidate: string): string {
  return realpathSync(candidate);
}

function handleConnectionRead(): unknown {
  return readConnectionView(state);
}

function handleConnectionSave(payload: unknown): unknown {
  const parsed = parseIpcRequest('connection.save', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const input = parsed.data as { gatewayUrl: string; accessToken: string; computerName: string };
  return { ok: true, data: saveConnection(state, input) };
}

function handleConnectionTest(): unknown {
  return testConnection(state);
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

function handleCodexProbe(): unknown {
  return codexSession.probe();
}

function handleCodexTurnStatus(): unknown {
  return codexSession.status();
}

/**
 * Continue a session the user is looking at.
 *
 * The working directory comes from the provider's own record of the session.
 * The Desktop only runs it when that directory is covered by a project the
 * user has explicitly authorised.
 */
async function handleCodexSessionContinue(payload: unknown): Promise<unknown> {
  const parsed = parseIpcRequest('codex.session.continue', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { threadId, prompt } = parsed.data as { threadId: string; prompt: string };

  const cwd = await codexSession.sessionCwd(threadId);
  if (cwd === null) {
    return { ok: false, code: 'NOT_FOUND', message: '会话不存在或没有工作目录' };
  }
  if (findAuthorizedProject(state, cwd) === null) {
    return {
      ok: false,
      code: 'PERMISSION_DENIED',
      message: `这个会话在 ${cwd}，先授权这个目录才能继续`,
    };
  }

  try {
    const turn = await codexSession.continueSession(threadId, prompt);
    return { ok: true, data: turn };
  } catch (error) {
    const message = error instanceof Error ? error.message : '继续会话失败';
    logger.error('继续 Codex 会话失败', { message });
    return { ok: false, code: 'DEPENDENCY_UNAVAILABLE', message };
  }
}

/**
 * Authorise the directory a session belongs to.
 *
 * The directory still has to be confirmed in the macOS picker, so the
 * renderer can never widen the grant on its own.
 */
async function handleCodexSessionAuthorize(payload: unknown): Promise<unknown> {
  const parsed = parseIpcRequest('codex.session.authorize', payload);
  if (!parsed.ok) {
    return { ok: false, code: parsed.code, message: parsed.message };
  }
  const { threadId } = parsed.data as { threadId: string };

  const cwd = await codexSession.sessionCwd(threadId);
  if (cwd === null) {
    return { ok: false, code: 'NOT_FOUND', message: '会话不存在或没有工作目录' };
  }

  const picked = await dialog.showOpenDialog({
    title: '授权这个会话所在的目录',
    defaultPath: cwd,
    properties: ['openDirectory', 'createDirectory'],
  });
  const selected = picked.canceled ? undefined : picked.filePaths[0];
  if (!selected) {
    return { ok: false, code: 'NOT_FOUND', message: '未选择目录' };
  }

  let rootPath: string;
  try {
    rootPath = realpathSync(selected);
  } catch {
    return { ok: false, code: 'INVALID_ARGUMENT', message: '所选目录不可用' };
  }
  if (rootPath !== cwd) {
    return {
      ok: false,
      code: 'POLICY_DENIED',
      message: '所选目录与这个会话的工作目录不一致',
    };
  }

  const name =
    rootPath
      .split('/')
      .filter((part) => part.length > 0)
      .pop() ?? rootPath;
  const project = grantProject(
    state,
    { displayName: name, capabilities: ['read', 'write', 'test'] },
    rootPath,
  );
  return { ok: true, data: project };
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
  'codex.session.authorize': (payload) => handleCodexSessionAuthorize(payload),
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
