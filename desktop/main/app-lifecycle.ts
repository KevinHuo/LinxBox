/** Decide whether closing the floating window should only hide it. */
export function shouldHideFloatWindow(isQuitting: boolean): boolean {
  return !isQuitting;
}
