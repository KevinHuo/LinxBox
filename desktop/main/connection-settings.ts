/**
 * Local connection settings on disk.
 *
 * The Gateway URL, the access token and the computer name are the only
 * settings the user has to enter, and re-entering them on every restart is
 * needless friction. They are stored in the app's own user-data directory as
 * a small JSON file.
 *
 * The file holds a secret, so it is written with owner-only permissions and
 * never written to a log. The token is never handed to the renderer: the
 * renderer only ever sees a mask of it.
 */
import { chmodSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';

/** The settings as they are stored on disk. */
export interface StoredConnectionSettings {
  readonly gatewayUrl: string;
  readonly accessToken: string;
  readonly computerName: string;
}

/** Where the settings live inside the user-data directory. */
export const SETTINGS_FILE_NAME = 'connection.json';

/** The file mode of the settings file: readable by the owner only. */
const OWNER_ONLY = 0o600;

/**
 * The path of the settings file.
 *
 * The user-data directory is created by Electron, but the tests run outside
 * it, so the parent directory is created here when it is missing.
 */
export function resolveSettingsPath(userDataDir: string): string {
  return join(userDataDir, SETTINGS_FILE_NAME);
}

/**
 * Read the stored settings.
 *
 * A missing file means "not configured yet". A damaged file is treated the
 * same way: the user can simply enter the settings again, and the broken file
 * does not stop the client from starting.
 */
export function readConnectionSettings(userDataDir: string): StoredConnectionSettings | null {
  const path = resolveSettingsPath(userDataDir);
  let raw: string;
  try {
    raw = readFileSync(path, 'utf8');
  } catch {
    return null;
  }

  try {
    const parsed = JSON.parse(raw) as Partial<StoredConnectionSettings>;
    if (
      typeof parsed.gatewayUrl !== 'string' ||
      typeof parsed.accessToken !== 'string' ||
      typeof parsed.computerName !== 'string'
    ) {
      return null;
    }
    if (parsed.gatewayUrl === '' || parsed.accessToken === '') {
      return null;
    }
    return {
      gatewayUrl: parsed.gatewayUrl,
      accessToken: parsed.accessToken,
      computerName: parsed.computerName,
    };
  } catch {
    return null;
  }
}

/**
 * Write the settings.
 *
 * The file is written with owner-only permissions because it holds a token.
 */
export function writeConnectionSettings(
  userDataDir: string,
  settings: StoredConnectionSettings,
): void {
  const path = resolveSettingsPath(userDataDir);
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, `${JSON.stringify(settings, null, 2)}\n`, { mode: OWNER_ONLY });
  // `writeFileSync` only applies the mode when it creates the file, so the
  // permissions are set again for an existing file.
  chmodSync(path, OWNER_ONLY);
}

/** Remove the stored settings. */
export function clearConnectionSettings(userDataDir: string): void {
  const path = resolveSettingsPath(userDataDir);
  try {
    writeFileSync(path, '{}\n', { mode: OWNER_ONLY });
  } catch {
    // Nothing to clear.
  }
}
