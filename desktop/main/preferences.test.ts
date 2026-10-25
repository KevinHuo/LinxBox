import { describe, expect, it } from 'vitest';

import {
  clampFloatPosition,
  createAppearanceState,
  createFloatState,
  defaultFloatPosition,
  FLOAT_BALL_SIZE,
  FLOAT_EDGE_MARGIN,
  readAppearanceView,
  readFloatView,
  saveAppearance,
} from './preferences.js';

const workArea = { x: 0, y: 0, width: 1440, height: 900 };

describe('createAppearanceState', () => {
  it('starts on the warm theme', () => {
    expect(createAppearanceState().theme).toBe('warm');
  });
});

describe('createFloatState', () => {
  it('starts visible and without a position', () => {
    const state = createFloatState();
    expect(state.visible).toBe(true);
    expect(state.position).toBeNull();
  });
});

describe('readAppearanceView', () => {
  it('reports the current theme', () => {
    expect(readAppearanceView({ theme: 'dark' }).theme).toBe('dark');
  });
});

describe('readFloatView', () => {
  it('reports visibility and position', () => {
    const view = readFloatView({ visible: false, position: { x: 10, y: 20 } });
    expect(view.visible).toBe(false);
    expect(view.position).toEqual({ x: 10, y: 20 });
  });
});

describe('saveAppearance', () => {
  it('stores a supported theme', () => {
    const state = createAppearanceState();
    expect(saveAppearance(state, { theme: 'dark' }).ok).toBe(true);
    expect(state.theme).toBe('dark');
  });

  it('rejects an unknown theme and keeps the old one', () => {
    const state = createAppearanceState();
    const result = saveAppearance(state, { theme: 'neon' });
    expect(result.ok).toBe(false);
    expect(result.code).toBe('INVALID_ARGUMENT');
    expect(state.theme).toBe('warm');
  });
});

describe('clampFloatPosition', () => {
  it('keeps a position inside the work area', () => {
    expect(clampFloatPosition({ x: 400, y: 300 }, workArea)).toEqual({ x: 400, y: 300 });
  });

  it('clamps a position past the bottom right corner', () => {
    const maxX = workArea.width - FLOAT_BALL_SIZE - FLOAT_EDGE_MARGIN;
    const maxY = workArea.height - FLOAT_BALL_SIZE - FLOAT_EDGE_MARGIN;
    expect(clampFloatPosition({ x: 9999, y: 9999 }, workArea)).toEqual({ x: maxX, y: maxY });
  });

  it('clamps a position past the top left corner', () => {
    expect(clampFloatPosition({ x: -500, y: -500 }, workArea)).toEqual({
      x: FLOAT_EDGE_MARGIN,
      y: FLOAT_EDGE_MARGIN,
    });
  });

  it('respects a work area that does not start at the origin', () => {
    const offset = { x: 100, y: 50, width: 800, height: 600 };
    expect(clampFloatPosition({ x: 0, y: 0 }, offset)).toEqual({
      x: 100 + FLOAT_EDGE_MARGIN,
      y: 50 + FLOAT_EDGE_MARGIN,
    });
  });

  it('keeps the ball reachable in a work area smaller than the ball', () => {
    const tiny = { x: 0, y: 0, width: 30, height: 30 };
    expect(clampFloatPosition({ x: 10, y: 10 }, tiny)).toEqual({
      x: FLOAT_EDGE_MARGIN,
      y: FLOAT_EDGE_MARGIN,
    });
  });
});

describe('defaultFloatPosition', () => {
  it('puts the ball in the bottom right corner of the work area', () => {
    expect(defaultFloatPosition(workArea)).toEqual({
      x: workArea.width - FLOAT_BALL_SIZE - FLOAT_EDGE_MARGIN,
      y: workArea.height - FLOAT_BALL_SIZE - FLOAT_EDGE_MARGIN,
    });
  });

  it('places the ball inside a work area that does not start at the origin', () => {
    const offset = { x: 100, y: 50, width: 800, height: 600 };
    expect(defaultFloatPosition(offset)).toEqual({
      x: 100 + 800 - FLOAT_BALL_SIZE - FLOAT_EDGE_MARGIN,
      y: 50 + 600 - FLOAT_BALL_SIZE - FLOAT_EDGE_MARGIN,
    });
  });
});
