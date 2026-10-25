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
  const pill = headPill(state.connection);
  const element = qs('#head-pill');
  const text = qs('#head-pill-txt');
  element?.classList.toggle('is-on', pill.on);
  if (text) {
    text.textContent = pill.text;
  }
  const title = qs('#home-title');
  if (title) {
    title.textContent = homeHeadline(state.connection);
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
    sub.textContent = deviceSubtitle(state.connection);
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

/** Render every part of the window from the current state. */
function renderAll(): void {
  renderHeadPill();
  renderTasks();
  renderDevice();
  renderSettings();
  renderConnection();
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
function startPairing(): void {
  const input = qs<HTMLInputElement>('#mac-input');
  const mac = normalizeMacAddress(input?.value ?? '');
  if (!isValidMacAddress(mac)) {
    showToast('请输入正确的 MAC 地址，如 A4:C1:38:5B:2A:9E');
    input?.focus();
    return;
  }

  const note = qs('#pairing-note');
  if (note) {
    note.textContent = `已向 ${mac} 发起配对 · 等待设备确认`;
  }
  go('pairing');
  showToast('配对协议尚未接入，暂无法完成设备确认');
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

/** Save the Gateway connection and dial out to it. */
async function saveConnection(): Promise<void> {
  const url = qs<HTMLInputElement>('#conn-url')?.value.trim() ?? '';
  const token = qs<HTMLInputElement>('#conn-token')?.value.trim() ?? '';
  const name = qs<HTMLInputElement>('#conn-name')?.value.trim() ?? '';

  if (url === '' || token === '' || name === '') {
    showToast('请填写 Gateway 地址、access_token 和电脑名称');
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

  qsa<HTMLButtonElement>('[data-switch="startup"]').forEach((button) => {
    button.addEventListener('click', () => {
      // Launch at login is a system-level preference. It is not wired to a
      // channel yet, so the switch reports that it is not in effect.
      showToast('开机自启尚未接入系统设置');
    });
  });

  qsa<HTMLButtonElement>('[data-back="main"]').forEach((button) => {
    button.addEventListener('click', () => go('home'));
  });

  qs('#btn-start')?.addEventListener('click', startPairing);
  qs('#btn-cancel')?.addEventListener('click', cancelPairing);
  qs('#btn-unpair')?.addEventListener('click', unpair);
  qs('#btn-conn-save')?.addEventListener('click', () => {
    void saveConnection();
  });
  qs('#btn-conn-test')?.addEventListener('click', () => {
    void testConnection();
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

  const projects = await bridge.listProjects();
  if (projects && typeof projects === 'object' && 'ok' in projects) {
    const result = projects as { ok: boolean; data?: unknown };
    if (result.ok && Array.isArray(result.data)) {
      state.projects = result.data as ProjectFacts[];
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
  renderAll();
}

/** Boot the window. */
async function main(): Promise<void> {
  wireEvents();
  bridge?.onConnectionChanged((payload) => applyGatewayStatus(payload));
  await refreshFromBridge();
  startConnectionPolling();
}

void main();

export { clampBallPosition };
