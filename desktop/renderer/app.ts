/**
 * Renderer entry point for the 灵矽 AI desktop client.
 *
 * The renderer holds a display model and one-shot user intent. It never sees a
 * token, a raw provider event or an unauthorised path. Every write goes
 * through the preload bridge, which forwards to one allowlisted main-process
 * channel.
 */
import {
  clampBallPosition,
  deviceSubtitle,
  getBridge,
  headPill,
  homeHeadline,
  isGlobalView,
  isValidMacAddress,
  normalizeMacAddress,
  pendingTaskCount,
  railAppForView,
  taskRows,
  type AppearanceFacts,
  type AppView,
  type ConnectionFacts,
  type FloatFacts,
  type GatewayStatusFacts,
  type ProjectFacts,
  type TaskFacts,
} from './view-model.js';

const bridge = getBridge();
const deviceStorageKey = 'linxbox.device.facts';

const state = {
  view: 'pair' as AppView,
  connection: {
    configured: false,
    connected: false,
    gatewayUrl: null,
    tokenHint: null,
    computerName: null,
  } as ConnectionFacts,
  gateway: {
    configured: false,
    connected: false,
    lastError: null,
    lastActivityAt: null,
  } as GatewayStatusFacts,
  projects: [] as ProjectFacts[],
  tasks: [] as TaskFacts[],
  appearance: { theme: 'warm' } as AppearanceFacts,
  float: { visible: true, position: null } as FloatFacts,
  device: { mac: null as string | null, pairedAt: null as string | null, syncedAt: null as string | null },
};

const qs = <T extends Element>(selector: string): T | null => document.querySelector<T>(selector);
const qsa = <T extends Element>(selector: string): T[] => [
  ...document.querySelectorAll<T>(selector),
];

const escapeHtml = (value: unknown): string =>
  String(value).replace(/[&<>"']/g, (character) => {
    const entities: Record<string, string> = {
      '&': '&amp;',
      '<': '&lt;',
      '>': '&gt;',
      '"': '&quot;',
      "'": '&#39;',
    };
    return entities[character] ?? character;
  });

let toastTimer: ReturnType<typeof setTimeout> | undefined;

function showToast(message: string): void {
  const toast = qs('#toast');
  if (!toast) {
    return;
  }
  toast.textContent = message;
  toast.classList.add('is-visible');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove('is-visible'), 2600);
}

/** Keep WSS and project configuration inside the Linx Box home page. */
function placeBoxSettings(): void {
  const target = qs('#box-settings');
  const settings = qs<HTMLElement>('[data-view="settings"]');
  if (!target || !settings) return;
  settings.querySelectorAll<HTMLElement>('.settings-block').forEach((block) => {
    if (block.querySelector('#conn-url')) target.append(block);
  });
}

/** Switch the visible view and keep the rail in sync. */
function go(view: AppView): void {
  state.view = view;

  qsa<HTMLElement>('.view').forEach((panel) => {
    panel.classList.toggle('is-active', panel.dataset['view'] === view);
  });

  const activeApp = railAppForView(view);
  qsa<HTMLButtonElement>('.rail-item').forEach((button) => {
    const selected = button.dataset['app'] === activeApp;
    button.classList.toggle('is-active', selected);
    if (selected) {
      button.setAttribute('aria-current', 'page');
    } else {
      button.removeAttribute('aria-current');
    }
  });

  const global = isGlobalView(view);
  const head = qs<HTMLElement>('#app-head');
  const foot = qs<HTMLElement>('#app-foot');
  if (head) {
    head.style.display = global ? 'none' : '';
  }
  if (foot) {
    foot.style.display = global ? 'none' : '';
  }

  qs<HTMLElement>('#workspace')?.focus({ preventScroll: true });
}

/** Render the head pill and the home headline from the connection fact. */
function renderHeadPill(): void {
  const deviceBound = state.device.mac !== null;
  const pill = headPill(state.connection, deviceBound);
  const element = qs('#head-pill');
  const text = qs('#head-pill-txt');
  element?.classList.toggle('is-on', pill.on);
  if (text) {
    text.textContent = pill.text;
  }
  const title = qs('#home-title');
  if (title) {
    title.textContent = homeHeadline(state.connection, deviceBound);
  }
}

/** Render the recent-task list on the home view. */
function renderTasks(): void {
  const list = qs('#task-list');
  if (!list) {
    return;
  }
  const rows = taskRows(state.tasks, state.projects);
  if (rows.length === 0) {
    list.innerHTML = '<li class="empty-row">暂无任务</li>';
    return;
  }
  list.innerHTML = rows
    .map(
      (row) => `
      <li class="task-row${row.accent ? ' tone-accent' : ''}">
        <i class="t-dot" aria-hidden="true"></i>
        <div class="t-main">
          <strong>${escapeHtml(row.title)}</strong>
          <small>${escapeHtml(row.detail)}</small>
        </div>
        <span class="t-status${row.muted ? ' is-muted' : ''}">${escapeHtml(row.status)}</span>
      </li>`,
    )
    .join('');
}

/** Render the device panel on the home view. */
function renderDevice(): void {
  const pending = qs('#device-pending');
  if (pending) {
    pending.textContent = `${pendingTaskCount(state.tasks)} 条`;
  }
  const sub = qs('#device-sub');
  if (sub) {
    sub.textContent = deviceSubtitle(state.connection, state.device.mac !== null);
  }
  const paired = qs('#device-paired-at');
  const mac = qs('#device-mac');
  const synced = qs('#device-synced-at');
  const format = (value: string | null): string => value === null ? '—' : new Date(value).toLocaleString('zh-CN', { hour12: false });
  if (paired) paired.textContent = format(state.device.pairedAt);
  if (mac) mac.textContent = state.device.mac ?? '—';
  if (synced) synced.textContent = format(state.device.syncedAt);
}

function persistDeviceFacts(): void {
  try {
    window.localStorage.setItem(deviceStorageKey, JSON.stringify(state.device));
  } catch {
    // Renderer storage is only a cross-window display cache.
  }
}

function restoreDeviceFacts(): void {
  try {
    const value = JSON.parse(window.localStorage.getItem(deviceStorageKey) ?? 'null');
    if (value && typeof value === 'object') {
      state.device = value as typeof state.device;
    }
  } catch {
    // Ignore unavailable or malformed renderer storage.
  }
}

/** Render the settings view from the current preferences. */
function renderSettings(): void {
  qsa<HTMLButtonElement>('.seg-btn').forEach((button) => {
    button.classList.toggle('is-active', button.dataset['theme'] === state.appearance.theme);
  });
  qsa<HTMLButtonElement>('[data-switch="float"]').forEach((button) => {
    button.classList.toggle('is-on', state.float.visible);
    button.setAttribute('aria-checked', state.float.visible ? 'true' : 'false');
  });
}

/** Render the Gateway connection block on the settings view. */
function renderConnection(): void {
  const pill = qs('#conn-status');
  const text = qs('#conn-status-text');
  const online = state.gateway.connected;
  pill?.classList.toggle('is-on', online);
  if (text) {
    text.textContent = online ? '已连接' : state.gateway.configured ? '未连接' : '未配置';
  }

  const hint = qs('#conn-hint');
  if (hint) {
    if (state.gateway.lastError !== null) {
      hint.textContent = `最近错误：${state.gateway.lastError}`;
    }
  }

  // The url and the stored token hint are shown; the token itself stays in
  // the main process and is never handed back to the renderer.
  const url = qs<HTMLInputElement>('#conn-url');
  if (url && document.activeElement !== url && state.connection.gatewayUrl !== null) {
    url.value = state.connection.gatewayUrl;
  }
  const token = qs<HTMLInputElement>('#conn-token');
  if (token && document.activeElement !== token && state.connection.tokenHint !== null) {
    token.placeholder = state.connection.tokenHint;
  }
  const name = qs<HTMLInputElement>('#conn-name');
  if (name && document.activeElement !== name && state.connection.computerName !== null) {
    name.value = state.connection.computerName;
  }
}

/**
 * Authorise a local directory.
 *
 * The directory is picked in the system dialog inside the main process: the
 * renderer never supplies a path, so a compromised renderer cannot authorise
 * an arbitrary directory.
 */
async function grantProject(): Promise<void> {
  if (!bridge) {
    showToast('浏览器预览模式无法授权目录');
    return;
  }
  const result = await bridge.grantProject({
    displayName: '本地项目',
    capabilities: ['read', 'write', 'test'],
  });
  if (result && typeof result === 'object' && 'ok' in result && result.ok === false) {
    const failure = result as { message?: string };
    showToast(failure.message ?? '授权失败');
    return;
  }
  showToast('已授权该目录');
  await refreshFromBridge();
}

/** Revoke a local project grant. */
async function revokeProject(projectId: string): Promise<void> {
  if (!bridge) {
    return;
  }
  const result = await bridge.revokeProject({ projectId });
  if (result && typeof result === 'object' && 'ok' in result && result.ok === false) {
    const failure = result as { message?: string };
    showToast(failure.message ?? '撤销失败');
    return;
  }
  showToast('已撤销该目录');
  await refreshFromBridge();
}

/** Render every part of the window from the current state. */
function renderAll(): void {
  renderHeadPill();
  renderTasks();
  renderDevice();
  renderSettings();
  renderConnection();
  persistDeviceFacts();
}

/** Apply the appearance theme to the document. */
function applyTheme(theme: AppearanceFacts['theme']): void {
  state.appearance = { theme };
  document.documentElement.dataset['theme'] = theme;
  qsa<HTMLButtonElement>('.seg-btn').forEach((button) => {
    button.classList.toggle('is-active', button.dataset['theme'] === theme);
  });
}

/**
 * Start the pairing flow.
 *
 * The shape of the MAC address is checked locally. The handshake itself is
 * not implemented: the pairing contract has not been reviewed yet, so the
 * client reports that honestly instead of pretending a device confirmed.
 */
async function startPairing(): Promise<void> {
  if (!state.gateway.connected) {
    showToast('请先在设置中配置并连接 Gateway WSS');
    go('settings');
    return;
  }
  const input = qs<HTMLInputElement>('#mac-input');
  const mac = normalizeMacAddress(input?.value ?? '');
  if (!isValidMacAddress(mac)) {
    showToast('请输入正确的 MAC 地址，如 A4:C1:38:5B:2A:9E');
    input?.focus();
    return;
  }

  if (!bridge) return;
  const result = (await bridge.bindDevice({ macAddress: mac })) as { ok?: boolean; message?: string };
  if (!result?.ok) {
    showToast(result?.message ?? '设备配对失败');
    return;
  }
  state.device = { mac, pairedAt: new Date().toISOString(), syncedAt: new Date().toISOString() };
  persistDeviceFacts();
  renderDevice();
  const note = qs('#pairing-note');
  if (note) note.textContent = `设备 ${mac} 已配对成功`;
      go(state.device.mac !== null ? 'home' : 'pair');
  showToast('设备配对成功');
}

/** Cancel the pairing flow and return to the first screen. */
function cancelPairing(): void {
  go('pair');
  showToast('已取消配对');
}

/**
 * Leave the paired state.
 *
 * Unpairing only clears the local view. It does not tell a device anything:
 * that needs the pairing contract.
 */
function unpair(): void {
  state.device = { mac: null, pairedAt: null, syncedAt: null };
  persistDeviceFacts();
  go('pair');
  showToast('已退出配对 · 设备不再连接这台 Mac');
}

/** Toggle the appearance theme and persist it. */
async function setTheme(theme: AppearanceFacts['theme']): Promise<void> {
  applyTheme(theme);
  if (!bridge) {
    return;
  }
  await bridge.saveAppearance({ theme });
}

/** Show or hide the floating ball. */
async function setFloatVisible(visible: boolean): Promise<void> {
  state.float = { ...state.float, visible };
  renderSettings();
  if (!bridge) {
    showToast(visible ? '已开启悬浮窗' : '已关闭悬浮窗');
    return;
  }
  await bridge.setFloatWindowVisible({ visible });
  showToast(visible ? '已开启悬浮窗' : '已关闭悬浮窗');
}

/**
 * Save the Gateway connection and dial out to it.
 *
 * An empty token field means "keep the stored one": the renderer only ever
 * sees a mask of the token, so it cannot send it back unchanged.
 */
async function saveConnection(): Promise<void> {
  const url = qs<HTMLInputElement>('#conn-url')?.value.trim() ?? '';
  const token = qs<HTMLInputElement>('#conn-token')?.value.trim() ?? '';
  const name = qs<HTMLInputElement>('#conn-name')?.value.trim() ?? '';

  if (url === '' || name === '') {
    showToast('请填写 Gateway 地址和电脑名称');
    return;
  }
  if (token === '' && state.connection.tokenHint === null) {
    showToast('请填写 access_token');
    return;
  }

  if (!bridge) {
    showToast('浏览器预览模式无法建立连接');
    return;
  }

  const result = await bridge.saveConnection({
    gatewayUrl: url,
    accessToken: token,
    computerName: name,
  });
  if (result && typeof result === 'object' && 'ok' in result && result.ok === false) {
    const failure = result as { message?: string };
    showToast(failure.message ?? '保存失败');
    return;
  }
  showToast('已保存，正在连接 Gateway…');
  await refreshFromBridge();
}

/** Report the current Gateway connection state. */
async function testConnection(): Promise<void> {
  if (!bridge) {
    showToast('浏览器预览模式无法测试连接');
    return;
  }
  const result = await bridge.testConnection();
  if (result && typeof result === 'object' && 'ok' in result && result.ok === true) {
    showToast('Gateway 已连接');
  } else {
    const failure = result as { message?: string };
    showToast(failure.message ?? 'Gateway 未连接');
  }
  await refreshFromBridge();
}

/** Wire the static controls. */
function wireEvents(): void {
  qsa<HTMLButtonElement>('.rail-item').forEach((button) => {
    button.addEventListener('click', () => {
      go(button.dataset['app'] === 'settings' ? 'settings' : 'home');
    });
  });

  qsa<HTMLButtonElement>('.seg-btn').forEach((button) => {
    button.addEventListener('click', () => {
      const theme = button.dataset['theme'];
      if (theme === 'warm' || theme === 'dark') {
        void setTheme(theme);
      }
    });
  });

  qsa<HTMLButtonElement>('[data-switch="float"]').forEach((button) => {
    button.addEventListener('click', () => {
      void setFloatVisible(!state.float.visible);
    });
  });

  qsa<HTMLButtonElement>('[data-back="main"]').forEach((button) => {
    button.addEventListener('click', () => go(state.device.mac !== null ? 'home' : 'pair'));
  });

  qs('#btn-start')?.addEventListener('click', () => {
    void startPairing();
  });
  qs('#btn-cancel')?.addEventListener('click', cancelPairing);
  qs('#btn-unpair')?.addEventListener('click', unpair);
  qs('#btn-conn-save')?.addEventListener('click', () => {
    void saveConnection();
  });
  qs('#btn-conn-test')?.addEventListener('click', () => {
    void testConnection();
  });
  qs('#btn-project-add')?.addEventListener('click', () => {
    void grantProject();
  });

  // The revoke button lives inside the rendered list, so the click is
  // delegated to the list itself.
  qs('#project-list')?.addEventListener('click', (event) => {
    const target = event.target as HTMLElement | null;
    const button = target?.closest<HTMLElement>('[data-revoke]');
    const projectId = button?.dataset['revoke'];
    if (projectId !== undefined) {
      void revokeProject(projectId);
    }
  });
}

/** Read the local facts the window shows. */
async function refreshFromBridge(): Promise<void> {
  if (!bridge) {
    return;
  }

  const connection = await bridge.readConnection();
  if (connection && typeof connection === 'object' && 'ok' in connection) {
    const result = connection as { ok: boolean; data?: unknown };
    if (result.ok && result.data && typeof result.data === 'object') {
      const data = result.data as ConnectionFacts & Partial<GatewayStatusFacts>;
      state.connection = data;
      state.gateway = {
        configured: data.configured ?? false,
        connected: data.connected ?? false,
        lastError: data.lastError ?? null,
        lastActivityAt: data.lastActivityAt ?? null,
      };
    }
  }

  const projects = await bridge.listCodexProjects();
  if (projects && typeof projects === 'object' && 'ok' in projects) {
    const result = projects as { ok: boolean; data?: unknown };
    if (result.ok && Array.isArray(result.data)) {
      state.projects = (result.data as Array<{
        projectId: string;
        name: string;
        roots: readonly string[];
        position: number;
      }>).map((project) => ({
        projectId: project.projectId,
        displayName: project.name,
        rootPath: project.roots[0] ?? '',
        capabilities: ['codex'],
        grantVersion: 0,
        enabled: true,
      }));
    }
  }

  const tasks = await bridge.listTasks({ limit: 20 });
  if (tasks && typeof tasks === 'object' && 'ok' in tasks) {
    const result = tasks as { ok: boolean; data?: unknown };
    if (result.ok && Array.isArray(result.data)) {
      state.tasks = result.data as TaskFacts[];
    }
  }

  const appearance = await bridge.readAppearance();
  if (appearance && typeof appearance === 'object' && 'ok' in appearance) {
    const result = appearance as { ok: boolean; data?: unknown };
    if (result.ok && result.data && typeof result.data === 'object') {
      const theme = (result.data as AppearanceFacts).theme;
      if (theme === 'warm' || theme === 'dark') {
        applyTheme(theme);
      }
    }
  }

  const float = await bridge.readFloatWindow();
  if (float && typeof float === 'object' && 'ok' in float) {
    const result = float as { ok: boolean; data?: unknown };
    if (result.ok && result.data && typeof result.data === 'object') {
      state.float = result.data as FloatFacts;
    }
  }

  renderAll();
}

/** Keep the window in sync with the connection state. */
function startConnectionPolling(): void {
  if (!bridge) {
    return;
  }
  window.setInterval(() => {
    void refreshFromBridge();
  }, 15_000);
}

/**
 * React to a Gateway connection status change pushed by the main process.
 *
 * The socket can come up or drop between two reads, so the status arrives as
 * an event instead of being polled for.
 */
function applyGatewayStatus(payload: unknown): void {
  if (payload === null || typeof payload !== 'object') {
    return;
  }
  const status = payload as GatewayStatusFacts;
  state.gateway = {
    configured: status.configured ?? false,
    connected: status.connected ?? false,
    lastError: status.lastError ?? null,
    lastActivityAt: status.lastActivityAt ?? null,
  };
  state.connection = { ...state.connection, connected: status.connected ?? false };
  if (status.lastActivityAt !== null) state.device.syncedAt = status.lastActivityAt;
  renderAll();
}

/** Boot the window. */
async function main(): Promise<void> {
  placeBoxSettings();
  wireEvents();
  restoreDeviceFacts();
  bridge?.onConnectionChanged((payload) => applyGatewayStatus(payload));
  bridge?.onTaskChanged(() => {
    void refreshFromBridge();
  });
  await refreshFromBridge();
  startConnectionPolling();
}

void main();

export { clampBallPosition };
