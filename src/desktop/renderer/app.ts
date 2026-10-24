/**
 * Renderer entry point.
 *
 * The renderer holds a display model and one-shot user intent. It never sees
 * a token, a raw provider event or an unauthorised path. Every write goes
 * through the preload bridge, which forwards to one allowlisted main-process
 * channel.
 */
import {
  codexStatusLabel,
  connectionStatus,
  diagnosticsRows,
  filterTasks,
  formatRootPath,
  isCodexTurnRunning,
  projectLabel,
  projectStateLabel,
  selectTaskId,
  getBridge,
  type CodexProbeFacts,
  type CodexSessionDetailFacts,
  type CodexSessionFacts,
  type CodexTurnFacts,
  flattenSessionMessages,
  formatSessionTime,
  type ConnectionFacts,
  type DiagnosticsFacts,
  type LinxboxBridge,
  type ProjectFacts,
  type TaskFacts,
} from './view-model.js';
import { mockConnection, mockDiagnostics, mockProjects, mockTasks } from './fixtures.js';

const bridge: LinxboxBridge | null = getBridge();

const state = {
  connection: { ...mockConnection },
  projects: [...mockProjects],
  tasks: [...mockTasks],
  diagnostics: { ...mockDiagnostics },
  codexProbe: {
    available: false,
    binaryPath: null,
    version: null,
    userAgent: null,
    authMode: null,
    planType: null,
    error: null,
  } as CodexProbeFacts,
  codexTurn: {
    threadId: null,
    turnId: null,
    status: 'idle',
    progress: '',
    finalText: '',
    error: null,
    startedAt: null,
    completedAt: null,
  } as CodexTurnFacts,
  codexSessions: [] as CodexSessionFacts[],
  codexSessionDetail: null as CodexSessionDetailFacts | null,
  selectedThreadId: null as string | null,
  runningOnly: false,
  selectedTaskId: null as string | null,
};
const labels: Record<string, readonly [string, string]> = {
  overview: ['LINXBOX HOME', '你好，这里是 LinxBox'],
  lingxi: ['MCP WEBSOCKET', '连接 MCP WebSocket'],
  projects: ['LOCAL PROJECTS', '本地项目'],
  tasks: ['TASK HISTORY', '任务记录'],
  codex: ['LOCAL CODEX', 'Codex 会话'],
  diagnostics: ['SETTINGS & DIAGNOSTICS', '设置与诊断'],
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

function switchView(view: string): void {
  const label = labels[view];
  if (!label) {
    return;
  }
  qsa<HTMLButtonElement>('.nav-item').forEach((button) => {
    const selected = button.dataset['view'] === view;
    button.classList.toggle('is-active', selected);
    if (selected) {
      button.setAttribute('aria-current', 'page');
    } else {
      button.removeAttribute('aria-current');
    }
  });
  qsa<HTMLElement>('.view').forEach((panel) => {
    panel.classList.toggle('is-active', panel.dataset['viewPanel'] === view);
  });
  const kicker = qs('#view-kicker');
  const title = qs('#view-title');
  if (kicker) {
    kicker.textContent = label[0];
  }
  if (title) {
    title.textContent = label[1];
  }
  qs<HTMLElement>('#workspace')?.focus({ preventScroll: true });
}

function renderConnection(): void {
  const status = connectionStatus(state.connection);
  const pill = qs('#connection-pill');
  const label = qs('#connection-label');
  const hero = qs('#hero-lingxi-status');
  const preview = qs('#connection-preview');
  const previewTitle = qs('#preview-title');
  const previewCopy = qs('#preview-copy');
  const previewComputer = qs('#preview-computer');
  const previewNetwork = qs('#preview-network');
  const previewToken = qs('#preview-token');
  const connectButton = qs<HTMLButtonElement>('#connect-button');
  const disconnectButton = qs<HTMLButtonElement>('#disconnect-button');
  const gatewayInput = qs<HTMLInputElement>('#gateway-url');
  const tokenInput = qs<HTMLInputElement>('#access-token');
  const computerInput = qs<HTMLInputElement>('#computer-name');

  pill?.setAttribute('data-state', status.state);
  if (label) {
    label.textContent = status.label;
  }
  if (hero) {
    hero.textContent = status.state === 'online' ? '已连接' : '等待连接';
    hero.classList.toggle('status-good', status.state === 'online');
  }
  preview?.setAttribute('data-state', status.state);
  if (previewTitle) {
    previewTitle.textContent = status.state === 'online' ? 'LinxBox 已连接' : '等待连接';
  }
  if (previewCopy) {
    previewCopy.textContent =
      status.state === 'online'
        ? 'MCP WebSocket 已建立双向连接。'
        : '输入 MCP WebSocket 信息，然后测试 LinxBox 是否可以建立双向连接。';
  }
  if (previewComputer) {
    previewComputer.textContent = state.connection.computerName ?? '未命名 Mac';
  }
  if (previewNetwork) {
    previewNetwork.textContent = status.state === 'online' ? '已建立' : '未发起';
  }
  if (previewToken) {
    previewToken.textContent = state.connection.tokenHint ?? '未配置';
  }
  if (connectButton) {
    connectButton.textContent = state.connection.configured ? '重新测试' : '保存并测试';
  }
  if (disconnectButton) {
    disconnectButton.disabled = !state.connection.configured;
  }
  if (gatewayInput && !gatewayInput.value && state.connection.gatewayUrl) {
    gatewayInput.value = state.connection.gatewayUrl;
  }
  if (computerInput && !computerInput.value && state.connection.computerName) {
    computerInput.value = state.connection.computerName;
  }
  if (tokenInput) {
    tokenInput.value = '';
  }
}

function renderProjects(): void {
  const grid = qs('#project-grid');
  if (!grid) {
    return;
  }
  grid.innerHTML = state.projects
    .map(
      (project) => `
      <article class="project-card ${project.enabled ? 'is-enabled' : ''}">
        <div class="project-topline"><span>${escapeHtml(project.projectId)}</span><span>v${escapeHtml(project.grantVersion)}</span></div>
        <div class="folder-mark" aria-hidden="true"><i></i></div>
        <h3>${escapeHtml(project.displayName)}</h3>
        <p class="mock-path">${escapeHtml(formatRootPath(project.rootPath))}</p>
        <div class="project-footer">
          <span>${escapeHtml(projectStateLabel(project))}</span>
          <button type="button" class="switch ${project.enabled ? 'is-on' : ''}" data-toggle-project="${escapeHtml(project.projectId)}" aria-pressed="${project.enabled}"><i></i><span>${project.enabled ? '已启用' : '已停用'}</span></button>
        </div>
      </article>`,
    )
    .join('');
  const count = qs('#hero-project-count');
  if (count) {
    count.textContent = `${state.projects.filter((project) => project.enabled).length} 个`;
  }
}

function renderTasks(): void {
  const list = qs('#task-list');
  if (!list) {
    return;
  }
  const visible = filterTasks(state.tasks, state.runningOnly);
  state.selectedTaskId = selectTaskId(visible, state.selectedTaskId);
  list.innerHTML =
    visible
      .map(
        (task) => `
      <button class="task-row-large ${task.taskId === state.selectedTaskId ? 'is-selected' : ''}" type="button" data-task-id="${escapeHtml(task.taskId)}">
        <span class="task-status status-${escapeHtml(task.state)}" aria-hidden="true"></span>
        <span><strong>${escapeHtml(task.title)}</strong><small>${escapeHtml(projectLabel(state.projects, task.projectId))} · ${escapeHtml(task.updatedAt)}</small></span>
        <b>${escapeHtml(task.state)}</b>
      </button>`,
      )
      .join('') || '<p class="empty-state">没有符合条件的任务。</p>';
  renderTaskDetail();
}

function renderTaskDetail(): void {
  const detail = qs('#task-detail');
  if (!detail) {
    return;
  }
  const task = state.tasks.find((item) => item.taskId === state.selectedTaskId);
  detail.innerHTML = task
    ? `
    <p class="eyebrow">SELECTED TASK</p>
    <span class="task-detail-status status-text-${escapeHtml(task.state)}">${escapeHtml(task.state)}</span>
    <h3>${escapeHtml(task.title)}</h3>
    <p>${escapeHtml(task.stage)}</p>
    <dl>
      <div><dt>任务编号</dt><dd>${escapeHtml(task.taskId)}</dd></div>
      <div><dt>项目</dt><dd>${escapeHtml(projectLabel(state.projects, task.projectId))}</dd></div>
      <div><dt>状态</dt><dd>${escapeHtml(task.state)}</dd></div>
      <div><dt>时间</dt><dd>${escapeHtml(task.updatedAt)}</dd></div>
    </dl>
    <div class="local-note">完整 diff 与原始日志只留在这台机器上</div>`
    : '<p class="empty-state">选择一个任务查看详情。</p>';
}

function renderCodexPanel(): void {
  const set = (id: string, value: string): void => {
    const element = qs(`#${id}`);
    if (element) {
      element.textContent = value;
    }
  };
  set('codex-binary', state.codexProbe.binaryPath ?? '未探测');
  set('codex-version', state.codexProbe.version ?? '-');
  set(
    'codex-account',
    state.codexProbe.authMode
      ? `${state.codexProbe.authMode}${state.codexProbe.planType ? ` · ${state.codexProbe.planType}` : ''}`
      : '-',
  );
  set('codex-status', codexStatusLabel(state.codexTurn.status));

  const output = qs('#codex-output');
  if (output) {
    const parts: string[] = [];
    if (state.codexTurn.progress.length > 0) {
      parts.push(`<span class="codex-progress">${escapeHtml(state.codexTurn.progress)}</span>`);
    }
    if (state.codexTurn.finalText.length > 0) {
      parts.push(escapeHtml(state.codexTurn.finalText));
    }
    if (state.codexTurn.error !== null) {
      parts.push(`<span class="codex-error">${escapeHtml(state.codexTurn.error)}</span>`);
    }
    output.innerHTML = parts.join('\n\n');
  }

  const runButton = qs<HTMLButtonElement>('#codex-run');
  const interruptButton = qs<HTMLButtonElement>('#codex-interrupt');
  const running = isCodexTurnRunning(state.codexTurn);
  if (runButton) {
    runButton.disabled = running;
  }
  if (interruptButton) {
    interruptButton.disabled = !running;
  }

  const projectSelect = qs<HTMLSelectElement>('#codex-project');
  if (projectSelect) {
    const enabled = state.projects.filter((project) => project.enabled);
    const signature = enabled.map((project) => project.projectId).join(',');
    if (projectSelect.dataset['signature'] !== signature) {
      projectSelect.dataset['signature'] = signature;
      projectSelect.innerHTML = enabled
        .map(
          (project) =>
            `<option value="${escapeHtml(project.projectId)}">${escapeHtml(project.displayName)}</option>`,
        )
        .join('');
    }
  }
}

function renderCodexSessions(): void {
  const list = qs('#codex-session-list');
  if (!list) {
    return;
  }
  if (state.codexSessions.length === 0) {
    list.innerHTML = '<p class="codex-empty">还没有读到会话。点右上角"刷新会话"。</p>';
    return;
  }
  list.innerHTML = state.codexSessions
    .map(
      (session) => `
      <button class="codex-session-item ${session.threadId === state.selectedThreadId ? 'is-selected' : ''}" type="button" data-thread-id="${escapeHtml(session.threadId)}">
        <strong>${escapeHtml(session.title)}</strong>
        <small>${escapeHtml(formatSessionTime(session.updatedAt))} · ${escapeHtml(session.model ?? '-')}</small>
      </button>`,
    )
    .join('');
}

function renderCodexMessages(): void {
  const box = qs('#codex-messages');
  if (!box) {
    return;
  }
  const detail = state.codexSessionDetail;
  const meta = qs('#codex-transcript-meta');
  if (meta) {
    meta.textContent = detail ? `${detail.title} · ${detail.turns.length} 轮` : '未选择会话';
  }
  if (detail === null) {
    box.innerHTML = '<p class="codex-empty">左侧选一个会话，这里显示真实的聊天记录。</p>';
    return;
  }
  const messages = flattenSessionMessages(detail);
  if (messages.length === 0) {
    box.innerHTML = '<p class="codex-empty">这个会话还没有可读的消息。</p>';
    return;
  }
  box.innerHTML = messages
    .map(
      (message) => `
      <div class="codex-message is-${escapeHtml(message.role)}">
        <span>${message.role === 'user' ? '你' : 'Codex'}${message.phase === 'commentary' ? ' · 过程' : ''}</span>
        <p>${escapeHtml(message.text)}</p>
      </div>`,
    )
    .join('');
}

function renderDiagnostics(): void {
  const rows = diagnosticsRows(state.diagnostics);
  const set = (id: string, value: string): void => {
    const element = qs(`#${id}`);
    if (element) {
      element.textContent = value;
    }
  };
  set('diag-version', rows[0]?.value ?? '-');
  set('diag-protocol', rows[1]?.value ?? '-');
  set('diag-database', rows[2]?.value ?? '-');
  set('diag-connected', rows[3]?.value ?? '-');
  set('diag-last-error', rows[4]?.value ?? '-');
  const railVersion = qs('#rail-version');
  if (railVersion) {
    railVersion.textContent = state.diagnostics.desktopVersion;
  }
}

function renderAll(): void {
  renderConnection();
  renderProjects();
  renderTasks();
  renderDiagnostics();
  renderCodexPanel();
  renderCodexSessions();
  renderCodexMessages();
}

let codexPollTimer: ReturnType<typeof setInterval> | undefined;

function stopCodexPolling(): void {
  if (codexPollTimer !== undefined) {
    clearInterval(codexPollTimer);
    codexPollTimer = undefined;
  }
}

async function pollCodexTurn(): Promise<void> {
  if (bridge === null) {
    return;
  }
  const turn = (await bridge.readCodexTurn()) as CodexTurnFacts;
  state.codexTurn = turn;
  renderCodexPanel();
  if (!isCodexTurnRunning(turn)) {
    stopCodexPolling();
  }
}

function startCodexPolling(): void {
  stopCodexPolling();
  codexPollTimer = setInterval(() => {
    void pollCodexTurn();
  }, 1200);
}

async function listCodexSessions(): Promise<void> {
  if (bridge === null) {
    showToast('未接入桌面端桥接，仅本地预览');
    return;
  }
  const result = (await bridge.listCodexSessions({})) as {
    ok?: boolean;
    message?: string;
    data?: CodexSessionFacts[];
  };
  if (!result?.ok) {
    showToast(result?.message ?? '读取会话列表失败');
    return;
  }
  state.codexSessions = result.data ?? [];
  renderCodexSessions();
  const first = state.codexSessions[0];
  if (first !== undefined && state.selectedThreadId === null) {
    await readCodexSession(first.threadId);
  }
}

async function readCodexSession(threadId: string): Promise<void> {
  if (bridge === null) {
    return;
  }
  const result = (await bridge.readCodexSession({ threadId })) as {
    ok?: boolean;
    message?: string;
    data?: CodexSessionDetailFacts;
  };
  if (!result?.ok) {
    showToast(result?.message ?? '读取会话内容失败');
    return;
  }
  state.selectedThreadId = threadId;
  state.codexSessionDetail = result.data ?? null;
  renderCodexSessions();
  renderCodexMessages();
}

async function probeCodex(): Promise<void> {
  if (bridge === null) {
    showToast('未接入桌面端桥接，仅本地预览');
    return;
  }
  const probe = (await bridge.probeCodex()) as CodexProbeFacts;
  state.codexProbe = probe;
  renderCodexPanel();
  showToast(probe.available ? '本机 Codex 已就绪' : (probe.error ?? '未找到本机 Codex'));
}

async function runCodexTurn(): Promise<void> {
  if (bridge === null) {
    showToast('未接入桌面端桥接，仅本地预览');
    return;
  }
  const prompt = qs<HTMLTextAreaElement>('#codex-prompt')?.value.trim() ?? '';
  const projectId = qs<HTMLSelectElement>('#codex-project')?.value ?? '';
  if (!prompt) {
    showToast('请填写任务提示词');
    return;
  }
  if (!projectId) {
    showToast('请先在"本地项目"里授权一个项目');
    return;
  }
  const result = (await bridge.startCodexTurn({ prompt, projectId })) as {
    ok?: boolean;
    message?: string;
    data?: CodexTurnFacts;
  };
  if (!result?.ok) {
    showToast(result?.message ?? '启动失败');
    return;
  }
  state.codexTurn = result.data ?? state.codexTurn;
  renderCodexPanel();
  startCodexPolling();
}

async function interruptCodexTurn(): Promise<void> {
  if (bridge === null) {
    return;
  }
  const turnId = state.codexTurn.turnId;
  if (turnId === null) {
    return;
  }
  await bridge.interruptCodexTurn({ turnId });
  await pollCodexTurn();
}

async function refreshFromBridge(): Promise<void> {
  if (!bridge) {
    return;
  }
  const [connection, projects, tasks, diagnostics] = await Promise.all([
    bridge.readConnection(),
    bridge.listProjects(),
    bridge.listTasks(),
    bridge.readDiagnostics(),
  ]);
  state.connection = (connection as ConnectionFacts) ?? state.connection;
  state.projects = Array.isArray(projects) ? (projects as ProjectFacts[]) : state.projects;
  state.tasks = Array.isArray(tasks) ? (tasks as TaskFacts[]) : state.tasks;
  state.diagnostics = (diagnostics as DiagnosticsFacts) ?? state.diagnostics;
  renderAll();
}

function wireEvents(): void {
  qsa<HTMLButtonElement>('.nav-item').forEach((button) => {
    button.addEventListener('click', () => switchView(button.dataset['view'] ?? ''));
  });
  qsa<HTMLButtonElement>('[data-open-view]').forEach((button) => {
    button.addEventListener('click', () => switchView(button.dataset['openView'] ?? ''));
  });

  qs('#connection-form')?.addEventListener('submit', (event) => {
    event.preventDefault();
    const url = qs<HTMLInputElement>('#gateway-url')?.value.trim() ?? '';
    const token = qs<HTMLInputElement>('#access-token')?.value.trim() ?? '';
    const computer = qs<HTMLInputElement>('#computer-name')?.value.trim() ?? '';
    if (!url || !token || !computer) {
      showToast('请填写服务地址、访问令牌和电脑名称');
      return;
    }
    void saveConnection({ gatewayUrl: url, accessToken: token, computerName: computer });
  });

  qs('#disconnect-button')?.addEventListener('click', () => {
    showToast('连接配置由本地存储管理，清除功能尚未实现');
  });

  qs('#toggle-token')?.addEventListener('click', (event) => {
    const button = event.currentTarget;
    if (!(button instanceof HTMLElement)) {
      return;
    }
    const input = qs<HTMLInputElement>('#access-token');
    if (!input) {
      return;
    }
    const show = input.type === 'password';
    input.type = show ? 'text' : 'password';
    button.textContent = show ? '隐藏' : '显示';
  });

  qs('#computer-name')?.addEventListener('input', (event) => {
    const preview = qs('#preview-computer');
    if (preview) {
      preview.textContent = (event.target as HTMLInputElement).value || '未命名 Mac';
    }
  });

  qs('#add-project')?.addEventListener('click', () => {
    void grantProject();
  });

  qs('#project-grid')?.addEventListener('click', (event) => {
    const target = event.target as HTMLElement | null;
    const button = target?.closest('[data-toggle-project]');
    if (!(button instanceof HTMLElement)) {
      return;
    }
    void revokeProject(button.dataset['toggleProject'] ?? '');
  });

  qs('#filter-tasks')?.addEventListener('click', (event) => {
    const button = event.currentTarget;
    if (!(button instanceof HTMLElement)) {
      return;
    }
    state.runningOnly = !state.runningOnly;
    button.textContent = state.runningOnly ? '查看全部' : '仅看进行中';
    renderTasks();
  });

  qs('#task-list')?.addEventListener('click', (event) => {
    const target = event.target as HTMLElement | null;
    const row = target?.closest('[data-task-id]');
    if (!(row instanceof HTMLElement)) {
      return;
    }
    state.selectedTaskId = row.dataset['taskId'] ?? null;
    renderTasks();
  });

  qs('#refresh-diagnostics')?.addEventListener('click', () => {
    void refreshFromBridge();
  });

  qs('#codex-probe')?.addEventListener('click', () => {
    void probeCodex();
  });

  qs('#codex-sessions-refresh')?.addEventListener('click', () => {
    void listCodexSessions();
  });

  qs('#codex-session-list')?.addEventListener('click', (event) => {
    const target = event.target as HTMLElement | null;
    const item = target?.closest('[data-thread-id]');
    if (!(item instanceof HTMLElement)) {
      return;
    }
    const threadId = item.dataset['threadId'];
    if (threadId !== undefined) {
      void readCodexSession(threadId);
    }
  });

  qs('#codex-run')?.addEventListener('click', () => {
    void runCodexTurn();
  });

  qs('#codex-interrupt')?.addEventListener('click', () => {
    void interruptCodexTurn();
  });
}

async function saveConnection(input: {
  gatewayUrl: string;
  accessToken: string;
  computerName: string;
}): Promise<void> {
  if (!bridge) {
    showToast('未接入桌面端桥接，仅本地预览');
    return;
  }
  const result = (await bridge.saveConnection(input)) as {
    ok?: boolean;
    message?: string;
    data?: ConnectionFacts;
  };
  if (!result?.ok) {
    showToast(result?.message ?? '保存失败');
    return;
  }
  state.connection = result.data ?? state.connection;
  renderConnection();
  const tested = (await bridge.testConnection()) as { ok?: boolean; message?: string };
  showToast(tested?.ok ? '连接成功' : (tested?.message ?? '连接未建立'));
}

async function grantProject(): Promise<void> {
  if (!bridge) {
    showToast('未接入桌面端桥接，仅本地预览');
    return;
  }
  const result = (await bridge.grantProject({
    displayName: '新的本地项目',
    capabilities: ['read'],
  })) as {
    ok?: boolean;
    message?: string;
    data?: ProjectFacts;
  };
  if (!result?.ok) {
    showToast(result?.message ?? '未授权项目');
    return;
  }
  state.projects = [...state.projects, result.data as ProjectFacts];
  renderProjects();
}

async function revokeProject(projectId: string): Promise<void> {
  if (!bridge) {
    showToast('未接入桌面端桥接，仅本地预览');
    return;
  }
  const result = (await bridge.revokeProject({ projectId })) as {
    ok?: boolean;
    message?: string;
    data?: ProjectFacts[];
  };
  if (!result?.ok) {
    showToast(result?.message ?? '撤销失败');
    return;
  }
  state.projects = result.data ?? state.projects;
  renderProjects();
}

wireEvents();
renderAll();
void refreshFromBridge();
