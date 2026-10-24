/**
 * Codex app-server process discovery.
 *
 * The Desktop drives the Codex app-server that ships inside the ChatGPT
 * desktop app. It never reads Codex's private database, and it never guesses
 * a binary that is not provably the Codex CLI.
 */
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';

/**
 * Candidate locations of the Codex CLI, most specific first.
 *
 * The first entry is the app-server binary inside the ChatGPT desktop app.
 * The second is a Homebrew or rustup style install on `PATH`.
 */
export const CODEX_BINARY_CANDIDATES: readonly string[] = [
  '/Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex',
  '/Applications/Codex.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex',
  '/opt/homebrew/bin/codex',
  '/usr/local/bin/codex',
];

export interface CodexBinary {
  /** Absolute path of the Codex CLI. */
  readonly path: string;
  /** Version string reported by `codex --version`, when it could be read. */
  readonly version: string | null;
}

/**
 * Resolve the Codex CLI.
 *
 * Returns `null` when no candidate exists. The version probe is best effort:
 * a binary that exists but refuses to report a version is still usable.
 */
export function discoverCodexBinary(
  candidates: readonly string[] = CODEX_BINARY_CANDIDATES,
  exists: (path: string) => boolean = existsSync,
  readVersion: (path: string) => string | null = readCodexVersion,
): CodexBinary | null {
  for (const candidate of candidates) {
    if (!exists(candidate)) {
      continue;
    }
    return { path: candidate, version: readVersion(candidate) };
  }
  return null;
}

/**
 * Read `codex --version`.
 *
 * The probe is bounded: a CLI that hangs on `--version` must not block the
 * Desktop, so the whole call is wrapped in a short timeout.
 */
export function readCodexVersion(binaryPath: string, timeoutMs = 5000): string | null {
  try {
    const output = execFileSync(binaryPath, ['--version'], {
      encoding: 'utf8',
      timeout: timeoutMs,
      stdio: ['ignore', 'pipe', 'ignore'],
    });
    const trimmed = output.trim();
    return trimmed.length > 0 ? trimmed : null;
  } catch {
    return null;
  }
}

/**
 * Build the argument list for the app-server.
 *
 * The arguments are always an array and the process is never started through
 * a shell, so nothing in the path can be interpreted as a shell command.
 */
export function appServerArgs(): readonly string[] {
  return ['app-server', '--stdio'];
}
