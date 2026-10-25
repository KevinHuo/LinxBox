/**
 * Local appearance and floating-window preferences.
 *
 * Both are presentation facts of this Mac: which theme the client renders in
 * and where the floating ball sits. Neither changes what the Desktop executes,
 * and neither leaves the machine.
 *
 * The module is deliberately free of Electron imports so the policy can be
 * unit tested. `main/index.ts` wires the returned values into Electron.
 */
import {
  appearanceThemeSchema,
  type AppearanceTheme,
  type AppearanceView,
  type FloatWindowView,
} from '../shared/ipc.js';

/** The appearance the client starts with when nothing was stored yet. */
export const DEFAULT_APPEARANCE: AppearanceTheme = 'warm';

/** The floating ball starts visible, as the prototype shows it on first run. */
export const DEFAULT_FLOAT_VISIBLE = true;

/** Diameter of the floating ball, in points. */
export const FLOAT_BALL_SIZE = 62;

/** Gap between the ball and the screen edge, in points. */
export const FLOAT_EDGE_MARGIN = 8;

/** How far the ball may be dragged past the work area before it is clamped. */
export const FLOAT_CLAMP_SLACK = 64;

/** Everything the Desktop knows about the floating window. */
export interface FloatState {
  visible: boolean;
  position: { readonly x: number; readonly y: number } | null;
}

/** Everything the Desktop knows about the appearance. */
export interface AppearanceState {
  theme: AppearanceTheme;
}

/** The screen area the floating ball may live in. */
export interface WorkArea {
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly height: number;
}

/** The initial appearance, before anything was stored. */
export function createAppearanceState(): AppearanceState {
  return { theme: DEFAULT_APPEARANCE };
}

/** The initial floating-window state, before anything was stored. */
export function createFloatState(): FloatState {
  return { visible: DEFAULT_FLOAT_VISIBLE, position: null };
}

/** The floating-window facts for the renderer. */
export function readFloatView(state: FloatState): FloatWindowView {
  return { visible: state.visible, position: state.position };
}

/** The appearance facts for the renderer. */
export function readAppearanceView(state: AppearanceState): AppearanceView {
  return { theme: state.theme };
}

/**
 * Apply an appearance choice.
 *
 * An unknown theme is rejected instead of silently falling back, so a broken
 * payload cannot quietly reset the user's preference.
 */
export function saveAppearance(
  state: AppearanceState,
  input: { readonly theme: string },
): { readonly ok: boolean; readonly code?: string; readonly message?: string } {
  const parsed = appearanceThemeSchema.safeParse(input.theme);
  if (!parsed.success) {
    return { ok: false, code: 'INVALID_ARGUMENT', message: '外观主题不合法' };
  }
  state.theme = parsed.data;
  return { ok: true };
}

/**
 * Clamp a dragged ball position into the display work area.
 *
 * The ball is kept fully inside the work area so it can never be dropped
 * where the user cannot reach it. A work area smaller than the ball keeps the
 * ball at the work area origin.
 */
export function clampFloatPosition(
  position: { readonly x: number; readonly y: number },
  workArea: WorkArea,
): { readonly x: number; readonly y: number } {
  const maxX = workArea.x + workArea.width - FLOAT_BALL_SIZE - FLOAT_EDGE_MARGIN;
  const maxY = workArea.y + workArea.height - FLOAT_BALL_SIZE - FLOAT_EDGE_MARGIN;
  const minX = workArea.x + FLOAT_EDGE_MARGIN;
  const minY = workArea.y + FLOAT_EDGE_MARGIN;

  if (maxX < minX) {
    return { x: minX, y: minY };
  }
  if (maxY < minY) {
    return { x: minX, y: minY };
  }
  return {
    x: clamp(position.x, minX, maxX),
    y: clamp(position.y, minY, maxY),
  };
}

/**
 * Default position for the ball: the bottom-right corner of the work area.
 *
 * The prototype shows the ball in that corner, so the first run starts where
 * the design puts it.
 */
export function defaultFloatPosition(workArea: WorkArea): {
  readonly x: number;
  readonly y: number;
} {
  return {
    x: workArea.x + workArea.width - FLOAT_BALL_SIZE - FLOAT_EDGE_MARGIN,
    y: workArea.y + workArea.height - FLOAT_BALL_SIZE - FLOAT_EDGE_MARGIN,
  };
}

/** Clamp a number into an inclusive range. */
function clamp(value: number, min: number, max: number): number {
  if (value < min) {
    return min;
  }
  if (value > max) {
    return max;
  }
  return value;
}
