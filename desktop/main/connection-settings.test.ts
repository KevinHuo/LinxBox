import { mkdtempSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, describe, expect, it } from 'vitest';

import {
  clearConnectionSettings,
  createConnectionSettings,
  readConnectionSettings,
  resolveSettingsPath,
  writeConnectionSettings,
} from './connection-settings.js';

/** A temporary user-data directory for one test. */
let workspace: string | null = null;

function makeWorkspace(): string {
  workspace = mkdtempSync(join(tmpdir(), 'linxbox-settings-'));
  return workspace;
}

afterEach(() => {
  if (workspace !== null) {
    rmSync(workspace, { recursive: true, force: true });
    workspace = null;
  }
});

describe('connection settings', () => {
  it.each([
    { activeProfile: 'invalid', computerName: 'mac', profiles: {} },
    { activeProfile: 'online', computerName: 1, profiles: {} },
    { activeProfile: 'online', computerName: 'mac', profiles: null },
    { activeProfile: 'online', computerName: 'mac', profiles: 'invalid' },
    { activeProfile: 'online', computerName: 'mac', profiles: { local: null, online: {} } },
    {
      activeProfile: 'online',
      computerName: 'mac',
      profiles: { local: { gatewayUrl: 'ws://local', accessToken: 'local' }, online: 1 },
    },
    { gatewayUrl: 'ws://localhost', accessToken: 1, computerName: 'mac' },
    { gatewayUrl: 'ws://localhost', accessToken: 'local', computerName: null },
  ])('拒绝字段类型不合法的配置 %#', (value) => {
    const dir = makeWorkspace();
    writeFileSync(resolveSettingsPath(dir), JSON.stringify(value));
    expect(readConnectionSettings(dir)).toBeNull();
  });
  it('迁移远端旧档案时不改动其中的地址与凭据', () => {
    const dir = makeWorkspace();
    const legacy = {
      gatewayUrl: 'wss://gateway.example.com/ws',
      accessToken: 'old-remote',
      computerName: 'mac',
    };
    writeFileSync(resolveSettingsPath(dir), JSON.stringify(legacy));
    expect(readConnectionSettings(dir)).toMatchObject({
      activeProfile: 'online',
      computerName: 'mac',
      profiles: { online: { gatewayUrl: legacy.gatewayUrl, accessToken: legacy.accessToken } },
    });
  });
  const settings = (computerName = 'mac') => createConnectionSettings(computerName, 'secret');
  it('puts the file inside the user-data directory', () => {
    const dir = makeWorkspace();
    expect(resolveSettingsPath(dir)).toBe(join(dir, 'connection.json'));
  });

  it('reports no settings when nothing was stored', () => {
    expect(readConnectionSettings(makeWorkspace())).toBeNull();
  });

  it('stores and reads the settings back', () => {
    const dir = makeWorkspace();
    const value = settings('工作电脑');
    writeConnectionSettings(dir, value);
    expect(readConnectionSettings(dir)).toEqual(value);
  });

  it('migrates the previous single-Gateway settings into the matching profile', () => {
    const dir = makeWorkspace();
    writeFileSync(
      resolveSettingsPath(dir),
      JSON.stringify({
        gatewayUrl: 'ws://127.0.0.1:9000/v1/clients/connect',
        accessToken: 'old-local-token',
        computerName: '旧电脑',
      }),
    );

    const result = readConnectionSettings(dir);
    expect(result?.activeProfile).toBe('local');
    expect(result?.profiles.local).toEqual({
      gatewayUrl: 'ws://127.0.0.1:9000/v1/clients/connect',
      accessToken: 'old-local-token',
    });
    expect(result?.profiles.online.gatewayUrl).toBe('ws://124.220.162.10:8787/v1/clients/connect');
  });

  it('writes the file with owner-only permissions', () => {
    const dir = makeWorkspace();
    writeConnectionSettings(dir, settings());
    const mode = statSync(resolveSettingsPath(dir)).mode & 0o777;
    expect(mode).toBe(0o600);
  });

  it('creates the directory when it is missing', () => {
    const dir = join(makeWorkspace(), 'nested', 'userData');
    writeConnectionSettings(dir, settings());
    expect(readConnectionSettings(dir)).not.toBeNull();
  });

  it('treats a damaged file as no settings', () => {
    const dir = makeWorkspace();
    writeFileSync(resolveSettingsPath(dir), '{ not json');
    expect(readConnectionSettings(dir)).toBeNull();
  });

  it('rejects a file that is missing a field', () => {
    const dir = makeWorkspace();
    writeFileSync(
      resolveSettingsPath(dir),
      JSON.stringify({ gatewayUrl: 'ws://127.0.0.1:8787', computerName: 'mac' }),
    );
    expect(readConnectionSettings(dir)).toBeNull();
  });

  it('rejects a file with an empty gateway url or token', () => {
    const dir = makeWorkspace();
    writeFileSync(
      resolveSettingsPath(dir),
      JSON.stringify({ gatewayUrl: '', accessToken: 'secret', computerName: 'mac' }),
    );
    expect(readConnectionSettings(dir)).toBeNull();

    writeFileSync(
      resolveSettingsPath(dir),
      JSON.stringify({ gatewayUrl: 'ws://127.0.0.1:8787', accessToken: '', computerName: 'mac' }),
    );
    expect(readConnectionSettings(dir)).toBeNull();
  });

  it('clears the stored settings', () => {
    const dir = makeWorkspace();
    writeConnectionSettings(dir, settings());
    clearConnectionSettings(dir);
    expect(readConnectionSettings(dir)).toBeNull();
  });
});
