import { describe, expect, it } from 'vitest';

import { shouldHideFloatWindow } from './app-lifecycle.js';

describe('shouldHideFloatWindow', () => {
  it('keeps the resident application alive during a normal panel close', () => {
    expect(shouldHideFloatWindow(false)).toBe(true);
  });

  it('allows the panel to be destroyed while the application is quitting', () => {
    expect(shouldHideFloatWindow(true)).toBe(false);
  });
});
