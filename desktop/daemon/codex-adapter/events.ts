/**
 * Mapping from Codex app-server notifications to LinxBox domain events.
 *
 * This module is pure: it turns one raw notification into zero or one domain
 * event. Keeping the mapping free of I/O makes the "what does the provider
 * actually mean" rules testable without a provider.
 *
 * The terminal rule comes straight from the contract: only an explicitly
 * successful provider completion may become `succeeded`. A crash, a dropped
 * connection, or any status that cannot be proven must become `interrupted`.
 */
import {
  accountUpdatedParamsSchema,
  CODEX_NOTIFICATIONS,
  itemCompletedParamsSchema,
  turnCompletedParamsSchema,
  type AccountUpdatedParams,
  type ItemCompletedParams,
  type TurnCompletedParams,
} from './protocol.js';

/** Provider terminal statuses the adapter understands. */
export const PROVIDER_TERMINAL_STATUSES = ['completed', 'interrupted', 'failed'] as const;

export type ProviderTerminalStatus = (typeof PROVIDER_TERMINAL_STATUSES)[number];

/**
 * A provider terminal status.
 *
 * `interrupted` is the fallback for every status that cannot be proven to be
 * a success or a clean failure.
 */
export type ProviderOutcome = ProviderTerminalStatus;

/** A domain event produced from a provider notification. */
export type CodexDomainEvent =
  | { readonly type: 'thread-started'; readonly threadId: string }
  | { readonly type: 'turn-started'; readonly threadId: string; readonly turnId: string }
  | {
      readonly type: 'progress';
      readonly threadId: string | null;
      readonly turnId: string | null;
      readonly text: string;
    }
  | {
      readonly type: 'final';
      readonly threadId: string | null;
      readonly turnId: string | null;
      readonly text: string;
    }
  | {
      readonly type: 'terminal';
      readonly threadId: string | null;
      readonly turnId: string | null;
      readonly outcome: ProviderOutcome;
      readonly error: string | null;
    }
  | { readonly type: 'warning'; readonly threadId: string | null; readonly message: string }
  | {
      readonly type: 'provider-error';
      readonly threadId: string | null;
      readonly turnId: string | null;
      readonly message: string;
    }
  | {
      readonly type: 'account';
      readonly authMode: string | null;
      readonly planType: string | null;
    };

/**
 * Resolve a provider turn status into a provider outcome.
 *
 * Only `completed` is a success. `interrupted` and `failed` are kept as they
 * are. Every other value - including an unknown or missing status - becomes
 * `interrupted`, because an unprovable outcome must never be reported as a
 * success or as a clean failure.
 */
export function resolveProviderOutcome(status: string | undefined | null): ProviderOutcome {
  if (status === 'completed' || status === 'interrupted' || status === 'failed') {
    return status;
  }
  return 'interrupted';
}

/**
 * Extract a human-readable message from a provider error payload.
 *
 * The app-server sometimes wraps the real message in a JSON string, for
 * example `{"type":"error","status":400,"error":{"message":"..."}}`. The
 * message is unwrapped when possible and truncated otherwise.
 */
export function extractProviderErrorMessage(raw: unknown, maxLength = 400): string | null {
  if (typeof raw !== 'string' || raw.trim().length === 0) {
    return null;
  }
  let candidate = raw;
  try {
    const parsed: unknown = JSON.parse(raw);
    if (typeof parsed === 'object' && parsed !== null) {
      const inner = (parsed as { error?: unknown }).error;
      if (typeof inner === 'object' && inner !== null) {
        const message = (inner as { message?: unknown }).message;
        if (typeof message === 'string' && message.length > 0) {
          candidate = message;
        }
      } else if (typeof inner === 'string' && inner.length > 0) {
        candidate = inner;
      }
    }
  } catch {
    // Not a JSON payload; the raw text is the message.
  }
  const trimmed = candidate.trim();
  if (trimmed.length === 0) {
    return null;
  }
  return trimmed.length > maxLength ? `${trimmed.slice(0, maxLength)}…` : trimmed;
}

/**
 * Map one provider notification to a domain event.
 *
 * Returns `null` for notifications that carry no domain meaning, for example
 * MCP server start-up chatter.
 */
export function mapNotification(method: string, params: unknown): CodexDomainEvent | null {
  switch (method) {
    case CODEX_NOTIFICATIONS.turnStarted: {
      const payload = asRecord(params);
      const turn = asRecord(payload['turn']);
      const turnId = asString(turn['id']);
      const threadId = asString(payload['threadId']);
      if (turnId === null) {
        return null;
      }
      return { type: 'turn-started', threadId: threadId ?? '', turnId };
    }

    case CODEX_NOTIFICATIONS.itemCompleted: {
      const parsed = itemCompletedParamsSchema.safeParse(params);
      if (!parsed.success) {
        return null;
      }
      const payload: ItemCompletedParams = parsed.data;
      const item = asRecord(payload.item);
      if (item['type'] !== 'agentMessage') {
        return null;
      }
      const text = asString(item['text']);
      if (text === null || text.length === 0) {
        return null;
      }
      const phase = asString(item['phase']);
      if (phase === 'commentary') {
        return {
          type: 'progress',
          threadId: asString(payload.threadId),
          turnId: asString(payload.turnId),
          text,
        };
      }
      if (phase === 'final_answer' || phase === null) {
        return {
          type: 'final',
          threadId: asString(payload.threadId),
          turnId: asString(payload.turnId),
          text,
        };
      }
      return null;
    }

    case CODEX_NOTIFICATIONS.turnCompleted: {
      const parsed = turnCompletedParamsSchema.safeParse(params);
      if (!parsed.success) {
        return null;
      }
      const payload: TurnCompletedParams = parsed.data;
      const turn = asRecord(payload.turn);
      const outcome = resolveProviderOutcome(asString(turn['status']));
      const turnError = asRecord(turn['error']);
      return {
        type: 'terminal',
        threadId: asString(payload.threadId),
        turnId: asString(payload.turnId) ?? asString(turn['id']),
        outcome,
        error: extractProviderErrorMessage(turnError['message']),
      };
    }

    case CODEX_NOTIFICATIONS.error: {
      const payload = asRecord(params);
      const error = asRecord(payload['error']);
      const message =
        extractProviderErrorMessage(error['message']) ??
        extractProviderErrorMessage(payload['message']) ??
        '本机 Codex 返回了未知错误';
      return {
        type: 'provider-error',
        threadId: asString(payload['threadId']),
        turnId: asString(payload['turnId']),
        message,
      };
    }

    case CODEX_NOTIFICATIONS.warning: {
      const payload = asRecord(params);
      const message = asString(payload['message']);
      if (message === null) {
        return null;
      }
      return { type: 'warning', threadId: asString(payload['threadId']), message };
    }

    case CODEX_NOTIFICATIONS.accountUpdated: {
      const parsed = accountUpdatedParamsSchema.safeParse(params);
      if (!parsed.success) {
        return null;
      }
      const payload: AccountUpdatedParams = parsed.data;
      return {
        type: 'account',
        authMode: payload.authMode ?? null,
        planType: payload.planType ?? null,
      };
    }

    default:
      return null;
  }
}

function asRecord(value: unknown): Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}

function asString(value: unknown): string | null {
  return typeof value === 'string' ? value : null;
}
