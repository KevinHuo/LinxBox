/**
 * Floating window renderer.
 *
 * The floating panel shows local device and task status and lets the user
 * reopen the main window. It holds no secret: every fact it shows comes from
 * the same allowlisted bridge the main window uses.
 *
 * Dragging the ball is reported to the main process, which owns the real
 * window position and clamps it into the display work area.
 */
import {
  getBridge,
  pendingTaskCount,
  taskRows,
  type ConnectionFacts,
  type ProjectFacts,
  type TaskFacts,
} from './view-model.js';

const bridge = getBridge();

const state = {
  open: false,
  paused: false,
  connection: {
    configured: false,
    connected: false,
    gatewayUrl: null,
    tokenHint: null,
    computerName: null,
  } as ConnectionFacts,
  projects: [] as ProjectFacts[],
  tasks: [] as TaskFacts[],
};

const qs = <T extends Element>(selector: string): T | null => document.querySelector<T>(selector);

let toastTimer: ReturnType<typeof setTimeout> | undefined;

function showToast(message: string): void {
  const toast = qs('#toast');
  if (!toast) {
    return;
  }
  toast.textContent = message;
  toast.classList.add('is-visible');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove('is-visible'), 2400);
}

/** Show or hide the expanded card. */
function setOpen(open: boolean): void {
  state.open = open;
  qs('#float-card')?.classList.toggle('is-open', open);
  qs('#float-ball')?.setAttribute('aria-expanded', open ? 'true' : 'false');
}

/** Render the device row, the last activity and the pending count. */
function renderStatus(): void {
  const device = qs('#fc-device');
  if (device) {
    device.textContent = state.connection.connected ? '灵矽设备 · 在线' : '灵矽设备 · 等待';
  }
  const dot = qs('.fc-row b i');
  dot?.classList.toggle('is-on', state.connection.connected);

  const rows = taskRows(state.tasks, state.projects);
  const last = qs('#fc-last');
  if (last) {
    last.textContent = state.paused ? '已暂停 · 不再提醒动态' : (rows[0]?.title ?? '暂无任务');
  }

  const pending = qs('#fc-pending');
  if (pending) {
    pending.textContent = `${pendingTaskCount(state.tasks)} 条`;
  }
}

/** Read the local facts the panel shows. */
async function refresh(): Promise<void> {
  if (!bridge) {
    renderStatus();
    return;
  }

  const connection = await bridge.readConnection();
  if (connection && typeof connection === 'object' && 'ok' in connection) {
    const result = connection as { ok: boolean; data?: unknown };
    if (result.ok && result.data && typeof result.data === 'object') {
      state.connection = result.data as ConnectionFacts;
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

  renderStatus();
}

/**
 * Drag the ball.
 *
 * The panel window itself is not movable: the ball position is reported to
 * the main process, which moves the real window and keeps it inside the
 * display work area.
 */
function wireDrag(): void {
  const ball = qs<HTMLElement>('#float-ball');
  if (!ball) {
    return;
  }

  /**
   * Pointer coordinates read from the drag events.
   *
   * The renderer bundle ships without the full DOM event library, so the
   * pointer payload is described by the fields the drag handler needs.
   */
  interface PointerPayload {
    readonly clientX: number;
    readonly clientY: number;
    readonly pointerId: number;
  }

  let moved = false;
  let startX = 0;
  let startY = 0;
  let pointerOffsetX = 0;
  let pointerOffsetY = 0;
  let pendingPosition: { x: number; y: number } | null = null;
  let framePending = false;

  const flushPosition = (): void => {
    framePending = false;
    const position = pendingPosition;
    pendingPosition = null;
    if (position !== null) {
      void reportPosition(position.x, position.y);
    }
  };

  const schedulePosition = (x: number, y: number): void => {
    pendingPosition = { x, y };
    if (!framePending) {
      framePending = true;
      window.requestAnimationFrame(flushPosition);
    }
  };

  ball.addEventListener('pointerdown', (raw: Event) => {
    const event = raw as unknown as PointerPayload;
    moved = false;
    startX = event.clientX;
    startY = event.clientY;
    const bounds = ball.getBoundingClientRect();
    pointerOffsetX = event.clientX - bounds.left;
    pointerOffsetY = event.clientY - bounds.top;
    raw.preventDefault();
    ball.setPointerCapture(event.pointerId);
  });

  ball.addEventListener('pointermove', (raw: Event) => {
    const event = raw as unknown as PointerPayload;
    if (!ball.hasPointerCapture(event.pointerId)) {
      return;
    }
    const dx = event.clientX - startX;
    const dy = event.clientY - startY;
    if (Math.abs(dx) + Math.abs(dy) > 6) {
      moved = true;
    }
    if (!moved) {
      return;
    }
    // Convert renderer-local coordinates into the screen coordinates expected
    // by the main process. Coalesce rapid pointer events into one IPC update
    // per animation frame so stale positions cannot make the window oscillate.
    schedulePosition(
      window.screenX + event.clientX - pointerOffsetX,
      window.screenY + event.clientY - pointerOffsetY,
    );
  });

  const finishDrag = (raw: Event): void => {
    const event = raw as unknown as PointerPayload;
    if (ball.hasPointerCapture(event.pointerId)) {
      ball.releasePointerCapture(event.pointerId);
    }
    if (!moved) {
      setOpen(!state.open);
    }
  };

  ball.addEventListener('pointerup', finishDrag);
  ball.addEventListener('pointercancel', finishDrag);
}

/** Report the ball position to the main process. */
async function reportPosition(x: number, y: number): Promise<void> {
  if (!bridge) {
    return;
  }
  await bridge.moveFloatWindow({ x, y });
}

/** Toggle the reminder pause. */
function togglePause(): void {
  state.paused = !state.paused;
  const button = qs('#btn-pause');
  if (button) {
    button.textContent = state.paused ? '恢复提醒' : '暂停提醒';
  }
  qs('#float-ball')?.classList.toggle('is-paused', state.paused);
  renderStatus();
  showToast(state.paused ? '已暂停提醒 · 设备动态不再弹出' : '已恢复提醒');
}

/** Wire the panel controls. */
function wireEvents(): void {
  qs('#btn-open-main')?.addEventListener('click', () => {
    setOpen(false);
    void bridge?.focusMainWindow();
  });

  qs('#btn-pause')?.addEventListener('click', togglePause);

  const appButton = qs('#fc-app');
  const appList = qs('#fc-app-list');
  appButton?.addEventListener('click', () => {
    const open = appList?.classList.toggle('is-open') ?? false;
    appButton.setAttribute('aria-expanded', open ? 'true' : 'false');
  });

  wireDrag();
}

/** Boot the panel. */
async function main(): Promise<void> {
  wireEvents();
  await refresh();
  window.setInterval(() => {
    void refresh();
  }, 15_000);
}

void main();
