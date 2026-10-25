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
import { getBridge, parseDeviceFacts, type ConnectionFacts } from './view-model.js';

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
    activeProfile: 'online',
    profiles: {
      local: {
        gatewayUrl: 'ws://127.0.0.1:8787/v1/clients/connect',
        tokenHint: 'demo…oken',
      },
      online: {
        gatewayUrl: 'ws://124.220.162.10:8787/v1/clients/connect',
        tokenHint: null,
      },
    },
  } as ConnectionFacts,
  device: {
    mac: null as string | null,
    pairedAt: null as string | null,
    syncedAt: null as string | null,
  },
};

const qs = <T extends Element>(selector: string): T | null => document.querySelector<T>(selector);

/** Show or hide the expanded card. */
function setOpen(open: boolean): void {
  state.open = open;
  qs('#float-card')?.classList.toggle('is-open', open);
  qs('#float-ball')?.setAttribute('aria-expanded', open ? 'true' : 'false');
}

const deviceStorageKey = 'linxbox.device.facts';

function formatDeviceDate(value: string | null): string {
  return value === null ? '—' : new Date(value).toLocaleString('zh-CN', { hour12: false });
}

function readDeviceFacts(): void {
  try {
    const value = parseDeviceFacts(
      JSON.parse(window.localStorage.getItem(deviceStorageKey) ?? 'null'),
    );
    if (value !== null) state.device = value;
  } catch {
    // Ignore unavailable or malformed renderer storage.
  }
}

/** Render the device rows shared with the main window. */
function renderStatus(): void {
  const deviceBound = state.device.mac !== null;
  const deviceOnline = state.connection.connected && deviceBound;
  const device = qs('#fc-device');
  if (device) {
    device.textContent = deviceOnline ? '灵矽设备 · 在线' : '等待设备绑定';
  }
  const dot = qs('.fc-row b i');
  dot?.classList.toggle('is-on', deviceOnline);

  qs('#fc-paired-at')!.textContent = formatDeviceDate(state.device.pairedAt);
  qs('#fc-device-mac')!.textContent = state.device.mac ?? '—';
  qs('#fc-synced-at')!.textContent = formatDeviceDate(state.device.syncedAt);
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

/** Quit the whole Electron application, including the floating window. */
function quitApp(): void {
  void bridge?.quitApp();
}

/** Wire the panel controls. */
function wireEvents(): void {
  qs('#btn-open-main')?.addEventListener('click', () => {
    setOpen(false);
    void bridge?.focusMainWindow();
  });

  qs('#btn-quit')?.addEventListener('click', quitApp);

  const appButton = qs('#fc-app');
  const appList = qs('#fc-app-list');
  appButton?.addEventListener('click', () => {
    const open = appList?.classList.toggle('is-open') ?? false;
    appButton.setAttribute('aria-expanded', open ? 'true' : 'false');
  });

  appList?.addEventListener('click', (event) => {
    const target = event.target as HTMLElement | null;
    if (!target?.closest('[data-app]')) return;
    appList.classList.remove('is-open');
    appButton?.setAttribute('aria-expanded', 'false');
  });

  wireDrag();
  window.addEventListener('storage', (event) => {
    if (event.key === deviceStorageKey) {
      readDeviceFacts();
      renderStatus();
    }
  });
}

/** Boot the panel. */
async function main(): Promise<void> {
  wireEvents();
  readDeviceFacts();
  await refresh();
  window.setInterval(() => {
    void refresh();
  }, 15_000);
}

void main();
