/** Persistent Gateway profiles stored in Electron's user-data directory. */
import { chmodSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';

export type GatewayProfileId = 'local' | 'online';

export interface StoredGatewayProfile {
  readonly gatewayUrl: string;
  readonly accessToken: string;
}

export interface StoredConnectionSettings {
  readonly activeProfile: GatewayProfileId;
  readonly profiles: Readonly<Record<GatewayProfileId, StoredGatewayProfile>>;
  readonly computerName: string;
}

interface LegacyConnectionSettings {
  readonly gatewayUrl: string;
  readonly accessToken: string;
  readonly computerName: string;
}

export const LOCAL_GATEWAY_URL = 'ws://127.0.0.1:8787/v1/clients/connect';
export const ONLINE_GATEWAY_URL = 'ws://124.220.162.10:8787/v1/clients/connect';
export const SETTINGS_FILE_NAME = 'connection.json';
const OWNER_ONLY = 0o600;

export function resolveSettingsPath(userDataDir: string): string {
  return join(userDataDir, SETTINGS_FILE_NAME);
}

export function createConnectionSettings(
  computerName = '',
  onlineToken = '',
): StoredConnectionSettings {
  return {
    activeProfile: 'online',
    profiles: {
      local: { gatewayUrl: LOCAL_GATEWAY_URL, accessToken: 'demo-client-token' },
      online: { gatewayUrl: ONLINE_GATEWAY_URL, accessToken: onlineToken },
    },
    computerName,
  };
}

function isProfile(value: unknown): value is StoredGatewayProfile {
  if (!value || typeof value !== 'object') return false;
  const profile = value as Partial<StoredGatewayProfile>;
  return typeof profile.gatewayUrl === 'string' && typeof profile.accessToken === 'string';
}

function migrateLegacy(value: Partial<LegacyConnectionSettings>): StoredConnectionSettings | null {
  if (
    typeof value.gatewayUrl !== 'string' ||
    typeof value.accessToken !== 'string' ||
    typeof value.computerName !== 'string' ||
    value.gatewayUrl === '' ||
    value.accessToken === ''
  ) {
    return null;
  }
  const activeProfile: GatewayProfileId = /^(ws|wss):\/\/(127\.0\.0\.1|localhost)(:|\/)/i.test(
    value.gatewayUrl,
  )
    ? 'local'
    : 'online';
  const defaults = createConnectionSettings(value.computerName);
  return {
    ...defaults,
    activeProfile,
    profiles: {
      ...defaults.profiles,
      [activeProfile]: { gatewayUrl: value.gatewayUrl, accessToken: value.accessToken },
    },
  };
}

export function readConnectionSettings(userDataDir: string): StoredConnectionSettings | null {
  let raw: string;
  try {
    raw = readFileSync(resolveSettingsPath(userDataDir), 'utf8');
  } catch {
    return null;
  }
  try {
    const parsed = JSON.parse(raw) as Record<string, unknown>;
    if (!('activeProfile' in parsed)) return migrateLegacy(parsed);
    if (
      (parsed['activeProfile'] !== 'local' && parsed['activeProfile'] !== 'online') ||
      typeof parsed['computerName'] !== 'string' ||
      !parsed['profiles'] ||
      typeof parsed['profiles'] !== 'object'
    ) {
      return null;
    }
    const profiles = parsed['profiles'] as Record<string, unknown>;
    if (!isProfile(profiles['local']) || !isProfile(profiles['online'])) return null;
    return {
      activeProfile: parsed['activeProfile'],
      profiles: { local: profiles['local'], online: profiles['online'] },
      computerName: parsed['computerName'],
    };
  } catch {
    return null;
  }
}

export function writeConnectionSettings(
  userDataDir: string,
  settings: StoredConnectionSettings,
): void {
  const path = resolveSettingsPath(userDataDir);
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, `${JSON.stringify(settings, null, 2)}\n`, { mode: OWNER_ONLY });
  chmodSync(path, OWNER_ONLY);
}

export function clearConnectionSettings(userDataDir: string): void {
  try {
    writeFileSync(resolveSettingsPath(userDataDir), '{}\n', { mode: OWNER_ONLY });
  } catch {
    // Nothing to clear.
  }
}
