/**
 * Delivery 处理 —— Gateway 消息的幂等接收、去重与顺序判定。
 *
 * 契约来源：`docs/contracts/gateway-desktop.md` §2/§6 与 SPEC-0003 §7.2。
 *
 * 本模块是纯函数：当前时间、已知 Delivery 与最后 ACK 的 sequence 全部由调用方注入，
 * 因此同一组事实总是得到同一个 disposition。模块不读时钟、不写存储、不产生副作用。
 *
 * 关键不变量：
 * - `message_id` 去重发生在任何 app-server side effect 之前；
 * - 相同 `message_id` + 相同 payload 返回稳定的 `duplicate`，永不重新评估成 `accepted`；
 * - 相同 `message_id` + 不同 payload 视为冲突并 fail closed；
 * - sequence 缺口或倒退都不猜测缺失消息，直接拒绝。
 */

import { createHash } from 'node:crypto';

import type {
  Disposition,
  GatewayMessageType,
  PublicError,
  PublicErrorCode,
} from '../../contracts/index.js';

/** Gateway 消息的原始事实。 */
export interface DeliveryFacts {
  readonly messageId: string;
  readonly sequence: number;
  readonly type: GatewayMessageType;
  readonly payload: unknown;
  readonly sentAt: string;
  readonly expiresAt?: string | undefined;
}

/**
 * 已持久化的 Delivery 记录（去重时使用）。
 *
 * `disposition` 是第一次处理时落盘的结果，重放时保持不变。
 */
export interface KnownDelivery {
  readonly messageId: string;
  readonly payloadHash: string;
  readonly disposition: Disposition;
  readonly error?: PublicError | undefined;
}

/** 判定所需的外部事实，全部由调用方注入以保持函数纯净。 */
export interface DeliveryEvaluationContext {
  /** 判定 `expires_at` 用的当前时间（epoch 毫秒）。 */
  readonly now: number;
  /** 该 client 最后 ACK 的 sequence；`undefined` 表示尚无 ACK 基线。 */
  readonly lastAckedSequence?: number | undefined;
  /** 同 `message_id` 的既有记录；`undefined` 表示首次收到。 */
  readonly knownDelivery?: KnownDelivery | undefined;
}

/** 落盘后的 Delivery 记录形态。 */
export interface DeliveryRecord {
  readonly messageId: string;
  readonly sequence: number;
  readonly type: GatewayMessageType;
  readonly payloadHash: string;
  readonly receivedAt: string;
  readonly disposition: Disposition;
  readonly error?: PublicError | undefined;
  readonly ackedAt?: string | undefined;
}

/** 原 disposition 与其脱敏错误，用于 `duplicate` 时保留首次结果。 */
export interface DeliveryOutcome {
  readonly disposition: Disposition;
  readonly error?: PublicError | undefined;
}

/** Delivery 判定结果。 */
export type DeliveryDecision =
  | { readonly disposition: 'accepted' }
  | { readonly disposition: 'duplicate'; readonly original: DeliveryOutcome }
  | { readonly disposition: 'expired'; readonly error: PublicError }
  | { readonly disposition: 'rejected'; readonly error: PublicError };

/**
 * 规范化 JSON 序列化：对象键按字典序排序，数组保持顺序。
 *
 * 语义与 `JSON.stringify` 对齐：`undefined`/函数/符号在对象中被丢弃、在数组中变成 `null`，
 * 非有限数字变成 `null`。目的是让 payload 哈希只依赖内容，不依赖键的书写顺序。
 */
export function canonicalJson(value: unknown): string {
  return JSON.stringify(canonicalize(value)) ?? 'null';
}

function canonicalize(value: unknown): unknown {
  if (value === null) {
    return null;
  }
  if (Array.isArray(value)) {
    return value.map((item) => canonicalize(item === undefined ? null : item));
  }
  if (typeof value === 'object') {
    const source = value as Record<string, unknown>;
    const result: Record<string, unknown> = {};
    for (const key of Object.keys(source).sort()) {
      const entry = source[key];
      if (entry === undefined || typeof entry === 'function' || typeof entry === 'symbol') {
        continue;
      }
      result[key] = canonicalize(entry);
    }
    return result;
  }
  if (typeof value === 'number' && !Number.isFinite(value)) {
    return null;
  }
  if (typeof value === 'bigint') {
    return value.toString();
  }
  if (typeof value === 'function' || typeof value === 'symbol') {
    return null;
  }
  return value;
}

/** payload 哈希：规范化 JSON 的 SHA-256 十六进制摘要。 */
export function hashPayload(value: unknown): string {
  return createHash('sha256').update(canonicalJson(value), 'utf8').digest('hex');
}

/**
 * 判定一个 Delivery 的 disposition。
 *
 * 判定顺序是契约规定的 fail-closed 顺序：
 * 1. 同 `message_id` + 同 payload → `duplicate`（保留首次 disposition）；
 * 2. 同 `message_id` + 不同 payload → `rejected`（`CONFLICT`）；
 * 3. `expires_at` 已过期 → `expired`；
 * 4. sequence 未严格递增 → `rejected`（`INVALID_ARGUMENT`）；
 * 5. sequence 出现缺口 → `rejected`（`INVALID_ARGUMENT`）；
 * 6. 其余 → `accepted`。
 *
 * `accepted` 只表示 Delivery 已落盘并通过初步校验，不表示 task started 或 succeeded。
 */
export function decideDelivery(
  facts: DeliveryFacts,
  context: DeliveryEvaluationContext,
): DeliveryDecision {
  const payloadHash = hashPayload(facts.payload);
  const known = context.knownDelivery;

  if (known !== undefined && known.messageId === facts.messageId) {
    if (known.payloadHash === payloadHash) {
      return {
        disposition: 'duplicate',
        original: { disposition: known.disposition, error: known.error },
      };
    }
    return {
      disposition: 'rejected',
      error: publicError(
        'CONFLICT',
        'delivery message_id 已存在但 payload 不一致',
        facts.messageId,
        { reason: 'payload_hash_conflict' },
      ),
    };
  }

  if (facts.expiresAt !== undefined) {
    const expiresAt = Date.parse(facts.expiresAt);
    if (Number.isNaN(expiresAt)) {
      return {
        disposition: 'rejected',
        error: publicError(
          'INVALID_ARGUMENT',
          'delivery expires_at 不是合法的 ISO 8601 时间',
          facts.messageId,
          { reason: 'malformed_expires_at' },
        ),
      };
    }
    if (expiresAt <= context.now) {
      return {
        disposition: 'expired',
        error: publicError('INVALID_ARGUMENT', 'delivery 已超过有效期', facts.messageId, {
          reason: 'expired',
        }),
      };
    }
  }

  const lastAcked = context.lastAckedSequence;
  if (lastAcked !== undefined) {
    if (facts.sequence <= lastAcked) {
      return {
        disposition: 'rejected',
        error: publicError('INVALID_ARGUMENT', 'delivery sequence 未严格递增', facts.messageId, {
          reason: 'sequence_not_monotonic',
          last_acked_sequence: lastAcked,
          received_sequence: facts.sequence,
        }),
      };
    }
    if (facts.sequence > lastAcked + 1) {
      return {
        disposition: 'rejected',
        error: publicError('INVALID_ARGUMENT', 'delivery sequence 出现缺口', facts.messageId, {
          reason: 'sequence_gap',
          expected_sequence: lastAcked + 1,
          received_sequence: facts.sequence,
        }),
      };
    }
  }

  return { disposition: 'accepted' };
}

/**
 * 由判定结果生成落盘记录。
 *
 * `duplicate` 时保留首次落盘的 disposition 与错误，保证记录在重放下稳定不变。
 */
export function toDeliveryRecord(
  facts: DeliveryFacts,
  decision: DeliveryDecision,
  receivedAt: string,
): DeliveryRecord {
  const outcome: DeliveryOutcome =
    decision.disposition === 'duplicate'
      ? decision.original
      : {
          disposition: decision.disposition,
          error: 'error' in decision ? decision.error : undefined,
        };

  return {
    messageId: facts.messageId,
    sequence: facts.sequence,
    type: facts.type,
    payloadHash: hashPayload(facts.payload),
    receivedAt,
    disposition: outcome.disposition,
    error: outcome.error,
  };
}

function publicError(
  code: PublicErrorCode,
  message: string,
  requestId: string,
  details: Record<string, string | number | boolean>,
): PublicError {
  return { code, message, retryable: false, request_id: requestId, details };
}
