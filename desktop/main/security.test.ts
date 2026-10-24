import { describe, expect, it } from 'vitest';

import {
  buildBrowserWindowOptions,
  isNavigationAllowed,
  isPermissionRequestAllowed,
  isWindowOpenAllowed,
  toExternalOpenUrl,
} from './security.js';

describe('buildBrowserWindowOptions', () => {
  it('runs the renderer sandboxed and isolated from Node', () => {
    const options = buildBrowserWindowOptions({ preloadPath: '/tmp/preload.js' });
    const preferences = options.webPreferences;

    expect(preferences?.sandbox).toBe(true);
    expect(preferences?.contextIsolation).toBe(true);
    expect(preferences?.nodeIntegration).toBe(false);
    expect(preferences?.nodeIntegrationInWorker).toBe(false);
    expect(preferences?.nodeIntegrationInSubFrames).toBe(false);
    expect(preferences?.webSecurity).toBe(true);
    expect(preferences?.allowRunningInsecureContent).toBe(false);
    expect(preferences?.webviewTag).toBe(false);
    expect(preferences?.experimentalFeatures).toBe(false);
    expect(preferences?.preload).toBe('/tmp/preload.js');
  });
});

describe('isNavigationAllowed', () => {
  const current =
    'file:///Applications/LinxBox.app/Contents/Resources/app/dist/renderer/index.html';

  it('allows the same local document', () => {
    expect(isNavigationAllowed(current, current)).toBe(true);
  });

  it('allows the same document with a different hash', () => {
    expect(isNavigationAllowed(`${current}#tasks`, current)).toBe(true);
  });

  it('refuses a remote origin', () => {
    expect(isNavigationAllowed('https://example.com/evil', current)).toBe(false);
  });

  it('refuses a local document with a different path', () => {
    expect(isNavigationAllowed('file:///tmp/other.html', current)).toBe(false);
  });

  it('refuses everything when the current page is not local', () => {
    expect(isNavigationAllowed('file:///tmp/other.html', 'https://example.com/')).toBe(false);
  });
});

describe('isWindowOpenAllowed', () => {
  it('never opens a second window', () => {
    expect(isWindowOpenAllowed('https://example.com')).toBe(false);
    expect(isWindowOpenAllowed('file:///tmp/other.html')).toBe(false);
  });
});

describe('toExternalOpenUrl', () => {
  it('allows https urls', () => {
    expect(toExternalOpenUrl('https://example.com/docs')).toBe('https://example.com/docs');
  });

  it('refuses file urls', () => {
    expect(toExternalOpenUrl('file:///etc/passwd')).toBeNull();
  });

  it('refuses javascript urls', () => {
    expect(toExternalOpenUrl('javascript:alert(1)')).toBeNull();
  });

  it('refuses malformed urls', () => {
    expect(toExternalOpenUrl('not a url')).toBeNull();
  });
});

describe('isPermissionRequestAllowed', () => {
  it('denies every permission request', () => {
    expect(isPermissionRequestAllowed()).toBe(false);
  });
});
