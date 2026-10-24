/**
 * Local structured logger.
 *
 * Every record passes through a redaction step before it reaches a sink. The
 * Desktop keeps full prompts, diffs, command output and credentials on the
 * machine, and logs may leave the machine through diagnostics exports, so the
 * logger must never be the place where that content leaks.
 */

export const LOG_LEVELS = ['debug', 'info', 'warn', 'error'] as const;

export type LogLevel = (typeof LOG_LEVELS)[number];

export type LogValue = string | number | boolean | null | undefined;

export type LogFields = Readonly<Record<string, LogValue>>;

export interface LogRecord {
  readonly level: LogLevel;
  readonly scope: string;
  readonly message: string;
  readonly fields: Readonly<Record<string, string | number | boolean>>;
  readonly at: string;
}

export type LogSink = (record: LogRecord) => void;

/** Field names whose value is always replaced, regardless of its shape. */
const SECRET_KEY_PATTERN =
  /token|secret|password|passphrase|cookie|authorization|credential|api[-_]?key|session[-_]?id/i;

/** Absolute POSIX paths, including the macOS user home prefix. */
const ABSOLUTE_PATH_PATTERN = /(^|[\s"'(])((?:\/[^\s"']+)+)/g;

export const REDACTED = '[redacted]';
export const REDACTED_PATH = '[path]';

/** Maximum length of a single redacted field value. */
const MAX_VALUE_LENGTH = 200;

/**
 * Redact a single field value.
 *
 * Secret-looking values are replaced wholesale. Absolute paths are replaced
 * path by path so the surrounding sentence stays readable.
 */
export function redactValue(key: string, value: LogValue): string | number | boolean {
  if (value === null || value === undefined) {
    return REDACTED;
  }
  if (typeof value === 'number' || typeof value === 'boolean') {
    return value;
  }
  if (SECRET_KEY_PATTERN.test(key)) {
    return REDACTED;
  }
  return redactText(value);
}

/** Replace absolute paths inside a free-form string. */
export function redactText(text: string): string {
  const redacted = text.replace(
    ABSOLUTE_PATH_PATTERN,
    (_match, prefix: string, path: string): string => {
      const replacement =
        path.startsWith('/Users/') || path.startsWith('/home/') || path.startsWith('/var/')
          ? REDACTED_PATH
          : `${REDACTED_PATH}${path}`;
      return `${prefix}${replacement}`;
    },
  );
  return redacted.length > MAX_VALUE_LENGTH ? `${redacted.slice(0, MAX_VALUE_LENGTH)}…` : redacted;
}

/** Redact every field of a log record. */
export function redactFields(fields: LogFields): Record<string, string | number | boolean> {
  const output: Record<string, string | number | boolean> = {};
  for (const [key, value] of Object.entries(fields)) {
    output[key] = redactValue(key, value);
  }
  return output;
}

export interface Logger {
  debug(message: string, fields?: LogFields): void;
  info(message: string, fields?: LogFields): void;
  warn(message: string, fields?: LogFields): void;
  error(message: string, fields?: LogFields): void;
}

export interface LoggerOptions {
  readonly sink?: LogSink;
  readonly now?: () => Date;
  readonly minLevel?: LogLevel;
}

const LEVEL_WEIGHT: Readonly<Record<LogLevel, number>> = {
  debug: 10,
  info: 20,
  warn: 30,
  error: 40,
};

/**
 * Sink used when no explicit sink is provided.
 *
 * `console.info` and `console.log` are not used: the Desktop writes local
 * diagnostics to stderr so that stdout stays free for the daemon's own
 * protocol frames.
 */
export const consoleSink: LogSink = (record) => {
  const line = `${record.at} ${record.level.toUpperCase()} [${record.scope}] ${record.message}`;
  const detail = Object.keys(record.fields).length > 0 ? ` ${JSON.stringify(record.fields)}` : '';
  if (record.level === 'error') {
    console.error(`${line}${detail}`);
    return;
  }
  console.warn(`${line}${detail}`);
};

/** Create a logger bound to a scope. */
export function createLogger(scope: string, options: LoggerOptions = {}): Logger {
  const sink = options.sink ?? consoleSink;
  const now = options.now ?? (() => new Date());
  const minWeight = LEVEL_WEIGHT[options.minLevel ?? 'info'];

  const emit = (level: LogLevel, message: string, fields?: LogFields): void => {
    if (LEVEL_WEIGHT[level] < minWeight) {
      return;
    }
    sink({
      level,
      scope,
      message: redactText(message),
      fields: fields ? redactFields(fields) : {},
      at: now().toISOString(),
    });
  };

  return {
    debug: (message, fields) => emit('debug', message, fields),
    info: (message, fields) => emit('info', message, fields),
    warn: (message, fields) => emit('warn', message, fields),
    error: (message, fields) => emit('error', message, fields),
  };
}
