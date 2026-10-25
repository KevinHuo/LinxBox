import { describe, expect, it } from 'vitest';

import {
  CODEX_BINARY_CANDIDATES,
  appServerArgs,
  discoverCodexBinary,
  readCodexVersion,
} from './discovery.js';

describe('discoverCodexBinary', () => {
  it('returns the first candidate that exists', () => {
    const binary = discoverCodexBinary(
      ['/missing/one', '/present/two', '/present/three'],
      (path) => path === '/present/two',
      () => 'codex-cli 1.2.3',
    );
    expect(binary?.path).toBe('/present/two');
    expect(binary?.version).toBe('codex-cli 1.2.3');
  });

  it('returns null when no candidate exists', () => {
    expect(discoverCodexBinary(['/a', '/b'], () => false)).toBeNull();
  });

  it('returns a binary even when the version cannot be read', () => {
    const binary = discoverCodexBinary(
      ['/a'],
      () => true,
      () => null,
    );
    expect(binary?.path).toBe('/a');
    expect(binary?.version).toBeNull();
  });

  it('looks for the app-server inside the ChatGPT desktop app by default', () => {
    expect(CODEX_BINARY_CANDIDATES[0]).toContain('ChatGPT.app');
  });
});

describe('readCodexVersion', () => {
  it('区分正常版本文本和成功但无输出的进程', () => {
    expect(readCodexVersion(process.execPath)).toMatch(/^v\d+\./);
    expect(readCodexVersion('/usr/bin/true')).toBeNull();
  });
  it('returns null when the binary refuses to report a version', () => {
    expect(readCodexVersion('/definitely/not/a/binary', 200)).toBeNull();
  });

  it('returns null when the probe times out', () => {
    expect(readCodexVersion('/definitely/not/a/binary', 1)).toBeNull();
  });
});

describe('appServerArgs', () => {
  it('asks for the stdio app-server', () => {
    expect(appServerArgs()).toEqual(['app-server', '--stdio']);
  });
});
