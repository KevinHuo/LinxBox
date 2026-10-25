/**
 * Electron security policy for the LinxBox Desktop window.
 *
 * This module is deliberately free of Electron imports so the policy can be
 * unit tested. `main/index.ts` only wires the returned values into Electron.
 */
import type { BrowserWindowConstructorOptions } from 'electron';

/** Protocol version of the local renderer document. */
export const RENDERER_PROTOCOL = 'file';

/** Origin of the bundled renderer document. */
export const RENDERER_ORIGIN = `${RENDERER_PROTOCOL}://`;

/**
 * Size of the floating panel.
 *
 * The panel is wider than the ball so the expanded card fits inside the same
 * window: the ball sits in its bottom-right corner. The height covers the
 * card content (head, application selector, three status rows and two action
 * buttons) plus the ball below it.
 */
export const FLOAT_PANEL_WIDTH = 320;
export const FLOAT_PANEL_HEIGHT = 430;

/** Schemes the Desktop is allowed to hand to the system browser. */
export const EXTERNAL_SCHEMES = ['https:'] as const;

export interface BrowserPolicyOptions {
  /** Absolute path of the compiled preload script. */
  readonly preloadPath: string;
}

/**
 * Build the `BrowserWindow` options for the Desktop shell.
 *
 * The renderer gets no Node integration, runs sandboxed, and is isolated from
 * the preload context. There is no remote module and no experimental web
 * platform feature is enabled.
 */
export function buildBrowserWindowOptions(
  options: BrowserPolicyOptions,
): BrowserWindowConstructorOptions {
  return {
    width: 1180,
    height: 820,
    minWidth: 960,
    minHeight: 640,
    show: false,
    backgroundColor: '#f7f2e8',
    title: '灵矽 AI · 桌面客户端',
    webPreferences: {
      preload: options.preloadPath,
      sandbox: true,
      contextIsolation: true,
      nodeIntegration: false,
      nodeIntegrationInWorker: false,
      nodeIntegrationInSubFrames: false,
      webSecurity: true,
      allowRunningInsecureContent: false,
      experimentalFeatures: false,
      webviewTag: false,
      spellcheck: false,
    },
  };
}

/**
 * Build the `BrowserWindow` options for the floating ball.
 *
 * The floating window is a frameless, transparent, always-on-top panel that
 * holds the ball and its expanded card. It never takes keyboard focus away
 * from the app the user is working in, and it is skipped in the window list
 * and the Dock.
 *
 * The renderer inside it is sandboxed exactly like the main window: the ball
 * may show local status, never read a token or reach the filesystem.
 */
export function buildFloatWindowOptions(
  options: BrowserPolicyOptions,
): BrowserWindowConstructorOptions {
  return {
    width: FLOAT_PANEL_WIDTH,
    height: FLOAT_PANEL_HEIGHT,
    show: false,
    frame: false,
    transparent: true,
    resizable: false,
    movable: false,
    minimizable: false,
    maximizable: false,
    fullscreenable: false,
    skipTaskbar: true,
    alwaysOnTop: true,
    hasShadow: false,
    backgroundColor: '#00000000',
    title: '灵矽 AI 悬浮窗',
    webPreferences: {
      preload: options.preloadPath,
      sandbox: true,
      contextIsolation: true,
      nodeIntegration: false,
      nodeIntegrationInWorker: false,
      nodeIntegrationInSubFrames: false,
      webSecurity: true,
      allowRunningInsecureContent: false,
      experimentalFeatures: false,
      webviewTag: false,
      spellcheck: false,
    },
  };
}

/**
 * Decide whether the window may navigate to a target URL.
 *
 * The renderer is a local document. Any other navigation is refused so a
 * malicious task output cannot turn the Desktop window into a browser.
 */
export function isNavigationAllowed(targetUrl: string, currentUrl: string): boolean {
  if (!currentUrl.startsWith(RENDERER_ORIGIN)) {
    return false;
  }
  if (!targetUrl.startsWith(RENDERER_ORIGIN)) {
    return false;
  }
  return sameLocalDocument(targetUrl, currentUrl);
}

/**
 * Decide whether a target may be opened in a new window.
 *
 * The Desktop never opens its own windows. External links are handed to the
 * system browser instead, and only over `https`.
 */
export function isWindowOpenAllowed(_targetUrl: string): boolean {
  return false;
}

/**
 * Convert a target URL into a URL the system browser may open.
 *
 * Returns `null` for anything that is not an `https` URL, which keeps
 * `file:`, `javascript:` and custom schemes out of the system browser.
 */
export function toExternalOpenUrl(targetUrl: string): string | null {
  let parsed: URL;
  try {
    parsed = new URL(targetUrl);
  } catch {
    return null;
  }
  const allowed = (EXTERNAL_SCHEMES as readonly string[]).includes(parsed.protocol);
  return allowed ? parsed.toString() : null;
}

/**
 * Decide whether a permission request may be granted.
 *
 * The Desktop needs no camera, microphone, geolocation or notification
 * permission: the renderer only displays local facts.
 */
export function isPermissionRequestAllowed(): boolean {
  return false;
}

/** Compare the path and query of two local renderer documents. */
function sameLocalDocument(targetUrl: string, currentUrl: string): boolean {
  const target = new URL(targetUrl);
  const current = new URL(currentUrl);
  return target.pathname === current.pathname && target.search === current.search;
}
