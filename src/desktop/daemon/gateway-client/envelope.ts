/**
 * 入站帧校验 —— 用 zod 严格校验 Gateway 发来的 envelope 与 payload。
 *
 * 契约来源：`docs/contracts/gateway-desktop.md` §2/§4/§5 与 SPEC-0003 §6.3。
 *
 * 校验规则：
 * - `protocol_version` 必须是本构建支持的版本；major 不同即拒绝（fail closed）；
 * - `type` 必须是已注册的消息类型，未知安全消息**拒绝**而不是忽略；
 * - `message_id`/`sequence`/`sent_at` 必须存在且格式正确，`sequence` 为非负整数；
 * - payload 按类型严格校验，未知字段一律拒绝。
 */

import { z } from 'zod';

import {
  GATEWAY_MESSAGE_TYPES,
  PROJECT_CAPABILITIES,
  SUPPORTED_PROTOCOL_VERSIONS,
} from '../../contracts/index.js';
import type { GatewayFrame, PublicError, PublicErrorCode } from '../../contracts/index.js';

/** 入站帧校验结果。 */
export type InboundFrameResult =
  | { readonly ok: true; readonly frame: GatewayFrame }
  | { readonly ok: false; readonly error: PublicError };

const ISO_DATE_TIME_PATTERN =
  /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?(?:Z|[+-]\d{2}:\d{2})$/;

const isoDateTimeSchema = z
  .string()
  .regex(ISO_DATE_TIME_PATTERN, 'sent_at/expires_at 必须是 UTC ISO 8601 时间');

const capabilitiesSchema = z.array(z.enum(PROJECT_CAPABILITIES));

const taskDispatchPayloadSchema = z.strictObject({
  task_id: z.string().min(1),
  project_id: z.string().min(1),
  agent: z.string().min(1),
  capabilities: capabilitiesSchema,
  prompt: z.string(),
  acceptance_criteria: z.array(z.string()),
  policy_version: z.string().min(1),
  continuation: z.union([z.strictObject({ parent_task_id: z.string().min(1) }), z.null()]),
});

const taskCancelPayloadSchema = z.strictObject({
  task_id: z.string().min(1),
  reason: z.string(),
});

const projectUpsertPayloadSchema = z.strictObject({
  project_id: z.string().min(1),
  display_name: z.string(),
  capabilities: capabilitiesSchema,
  availability: z.enum(['online', 'offline']),
  grant_version: z.number().int().nonnegative(),
});

const projectRevokePayloadSchema = z.strictObject({
  project_id: z.string().min(1),
  grant_version: z.number().int().nonnegative(),
  revoked_at: isoDateTimeSchema,
});

const frameEnvelopeFields = {
  protocol_version: z.enum(SUPPORTED_PROTOCOL_VERSIONS),
  message_id: z.string().min(1),
  sequence: z.number().int().nonnegative(),
  sent_at: isoDateTimeSchema,
  expires_at: isoDateTimeSchema.optional(),
};

/** 完整入站帧的 zod schema。 */
export const gatewayFrameSchema = z.discriminatedUnion('type', [
  z.strictObject({
    ...frameEnvelopeFields,
    type: z.literal('task.dispatch'),
    payload: taskDispatchPayloadSchema,
  }),
  z.strictObject({
    ...frameEnvelopeFields,
    type: z.literal('task.cancel'),
    payload: taskCancelPayloadSchema,
  }),
  z.strictObject({
    ...frameEnvelopeFields,
    type: z.literal('project.upsert'),
    payload: projectUpsertPayloadSchema,
  }),
  z.strictObject({
    ...frameEnvelopeFields,
    type: z.literal('project.revoke'),
    payload: projectRevokePayloadSchema,
  }),
]);

const looseEnvelopeSchema = z.strictObject({
  protocol_version: z.string(),
  message_id: z.string(),
  sequence: z.number(),
  type: z.string(),
  sent_at: z.string(),
  expires_at: z.string().optional(),
  payload: z.unknown(),
});

/**
 * 校验一个收到的帧。
 *
 * 未知字段、未知消息类型、不兼容的协议版本与不合法的 payload 一律返回
 * `INVALID_ARGUMENT`；`details.reason` 给出稳定的原因码，便于脱敏上报。
 */
export function parseInboundFrame(input: unknown): InboundFrameResult {
  const envelope = looseEnvelopeSchema.safeParse(input);
  if (!envelope.success) {
    return reject(reasonOf(envelope.error.issues), '入站帧结构不合法', input);
  }

  const data = envelope.data;
  if (!isSupportedProtocolVersion(data.protocol_version)) {
    return reject('protocol_version_incompatible', '不支持的协议版本', input);
  }
  if (!(GATEWAY_MESSAGE_TYPES as readonly string[]).includes(data.type)) {
    return reject('unknown_message_type', '未知的消息类型', input);
  }

  const parsed = gatewayFrameSchema.safeParse(input);
  if (!parsed.success) {
    return reject(reasonOf(parsed.error.issues), '入站帧内容不合法', input);
  }

  return { ok: true, frame: toGatewayFrame(parsed.data) };
}

/** zod 解析出来的帧类型。 */
type ParsedGatewayFrame = z.infer<typeof gatewayFrameSchema>;

/**
 * 把 zod 的解析结果收敛成契约里的 `GatewayFrame`。
 *
 * `expires_at` 是可选字段：缺省时整个键不出现，而不是显式写成 `undefined`。
 */
function toGatewayFrame(data: ParsedGatewayFrame): GatewayFrame {
  const base = {
    protocol_version: data.protocol_version,
    message_id: data.message_id,
    sequence: data.sequence,
    sent_at: data.sent_at,
  };
  const envelope = data.expires_at === undefined ? base : { ...base, expires_at: data.expires_at };

  switch (data.type) {
    case 'task.dispatch':
      return { ...envelope, type: 'task.dispatch', payload: data.payload };
    case 'task.cancel':
      return { ...envelope, type: 'task.cancel', payload: data.payload };
    case 'project.upsert':
      return { ...envelope, type: 'project.upsert', payload: data.payload };
    case 'project.revoke':
      return { ...envelope, type: 'project.revoke', payload: data.payload };
  }
}

/** 协议版本是否被本构建精确支持。 */
export function isSupportedProtocolVersion(version: string): boolean {
  return (SUPPORTED_PROTOCOL_VERSIONS as readonly string[]).includes(version);
}

/**
 * 协议版本是否与本构建兼容。
 *
 * 兼容按 major 判定：同 major 的不同 minor 视为兼容，不同 major 直接拒绝。
 */
export function isProtocolVersionCompatible(version: string): boolean {
  const major = majorVersionOf(version);
  if (major === undefined) {
    return false;
  }
  return SUPPORTED_PROTOCOL_VERSIONS.some((supported) => majorVersionOf(supported) === major);
}

function majorVersionOf(version: string): number | undefined {
  if (!/^\d+\.\d+$/.test(version)) {
    return undefined;
  }
  return Number.parseInt(version, 10);
}

function reasonOf(issues: readonly z.core.$ZodIssue[]): string {
  for (const issue of issues) {
    if (issue.code === 'unrecognized_keys') {
      return 'unknown_field';
    }
  }
  return 'malformed_frame';
}

function requestIdOf(input: unknown): string {
  if (typeof input === 'object' && input !== null) {
    const candidate = (input as { message_id?: unknown }).message_id;
    if (typeof candidate === 'string' && candidate.length > 0) {
      return candidate;
    }
  }
  return 'unknown';
}

function reject(
  reason: string,
  message: string,
  input: unknown,
): { readonly ok: false; readonly error: PublicError } {
  const error: PublicError = {
    code: 'INVALID_ARGUMENT' satisfies PublicErrorCode,
    message,
    retryable: false,
    request_id: requestIdOf(input),
    details: { reason },
  };
  return { ok: false, error };
}
