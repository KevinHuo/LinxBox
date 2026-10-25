/**
 * Gateway/Desktop `1.0` wire format.
 *
 * This module is the concrete answer to "远端要发什么格式才能驱动本地 Codex".
 * It encodes and decodes the frames that travel over the outbound WSS
 * connection between the Cloud Gateway and this Desktop.
 *
 * The Desktop is always the client: it dials out, it never listens.
 *
 * Handshake:
 *
 * ```http
 * GET /v1/clients/connect
 * Authorization: Bearer <client_token>
 * Upgrade: websocket
 * ```
 *
 * Every frame afterwards is a newline-free JSON object with the common
 * envelope:
 *
 * ```json
 * {
 *   "protocol_version": "1.0",
 *   "message_id": "msg_01H...",
 *   "sequence": 1043,
 *   "type": "task.dispatch",
 *   "sent_at": "2026-10-24T01:00:00Z",
 *   "expires_at": "2026-10-24T01:30:00Z",
 *   "payload": { }
 * }
 * ```
 */
import { z } from 'zod';

import {
  PROTOCOL_VERSION,
  type DesktopFrame,
  type DesktopMessageType,
  type Disposition,
  type AgentSessionCatalogItem,
  type GatewayFrame,
  type GatewayMessageType,
  type ProjectCapability,
  type PublicError,
  type PublicErrorCode,
  type TaskEventPayload,
} from '../../contracts/index.js';

/** Path the Desktop connects to. */
export const GATEWAY_CONNECT_PATH = '/v1/clients/connect';

/** Public error code used when a frame cannot be trusted. */
const INVALID_FRAME = 'INVALID_ARGUMENT';
const MAC_ADDRESS = /^(?:[0-9A-Fa-f]{12}|(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}|(?:[0-9A-Fa-f]{2}-){5}[0-9A-Fa-f]{2})$/;
const DEVICE_ID = /^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$/;
const CONTRACT_ERROR_CODE = z.enum([
  'INVALID_ARGUMENT',
  'UNAUTHENTICATED',
  'PERMISSION_DENIED',
  'NOT_FOUND',
  'CONFLICT',
  'DEPENDENCY_UNAVAILABLE',
  'RESOURCE_EXHAUSTED',
  'INTERNAL',
]);

const envelopeSchema = z.looseObject({
  protocol_version: z.string(),
  message_id: z.string().min(1),
  sequence: z.number().int().nonnegative(),
  type: z.string().min(1),
  sent_at: z.string().min(1),
  expires_at: z.string().optional(),
  payload: z.unknown(),
});

/** The payload the remote sends to make this machine run a coding task. */
export const taskDispatchSchema = z.strictObject({
  task_id: z.string().min(1),
  project_id: z.string().min(1),
  agent: z.string().min(1),
  capabilities: z.array(z.enum(['read', 'write', 'test'])),
  prompt: z.string(),
  acceptance_criteria: z.array(z.string()),
  policy_version: z.string().min(1),
  continuation: z.union([
    z.strictObject({ parent_task_id: z.string().min(1) }),
    z.strictObject({
      agent_session_id: z.string().min(1),
      thread_id: z.string().min(1),
    }),
    z.null(),
  ]),
});

export const taskCancelSchema = z.strictObject({
  task_id: z.string().min(1),
  reason: z.string(),
});

const agentSessionsSyncResultSchema = z
  .strictObject({
    reply_to_message_id: z.string().min(1),
    ok: z.literal(true),
  })
  .or(
    z.strictObject({
      reply_to_message_id: z.string().min(1),
      ok: z.literal(false),
      error: z.strictObject({
        code: z.enum([
          'INVALID_ARGUMENT',
          'UNAUTHENTICATED',
          'PERMISSION_DENIED',
          'NOT_FOUND',
          'CONFLICT',
          'DEPENDENCY_UNAVAILABLE',
          'RESOURCE_EXHAUSTED',
          'INTERNAL',
        ]),
        message: z.string().min(1),
      }),
    }),
  );

const bindingResultSchema = z
  .strictObject({
    reply_to_message_id: z.string().min(1),
    ok: z.literal(true),
    data: z.strictObject({
      device_id: z.string().regex(DEVICE_ID),
      desktop_client_id: z.string().min(1),
    }),
  })
  .or(
    z.strictObject({
      reply_to_message_id: z.string().min(1),
      ok: z.literal(false),
      error: z.strictObject({ code: CONTRACT_ERROR_CODE, message: z.string().min(1) }),
    }),
  );

/**
 * Parse an inbound frame from the Gateway.
 *
 * Unknown message types are rejected rather than ignored: the contract says an
 * unknown security message must be refused.
 */
export function parseInboundFrame(
  raw: unknown,
):
  | { readonly ok: true; readonly frame: GatewayFrame }
  | { readonly ok: false; readonly error: PublicError } {
  const envelope = envelopeSchema.safeParse(raw);
  if (!envelope.success) {
    return {
      ok: false,
      error: publicError(INVALID_FRAME, 'Gateway 帧格式不合法', 'gateway'),
    };
  }
  const value = envelope.data;
  if (value.protocol_version !== PROTOCOL_VERSION) {
    return {
      ok: false,
      error: publicError('INTERNAL', 'Gateway 协议版本不兼容', value.message_id),
    };
  }
  if (value.type === 'task.dispatch') {
    const payload = taskDispatchSchema.safeParse(value.payload);
    if (!payload.success) {
      return {
        ok: false,
        error: publicError(INVALID_FRAME, 'task.dispatch 负载不合法', value.message_id),
      };
    }
    return {
      ok: true,
      frame: { ...value, type: 'task.dispatch', payload: payload.data } as GatewayFrame,
    };
  }
  if (value.type === 'task.cancel') {
    const payload = taskCancelSchema.safeParse(value.payload);
    if (!payload.success) {
      return {
        ok: false,
        error: publicError(INVALID_FRAME, 'task.cancel 负载不合法', value.message_id),
      };
    }
    return {
      ok: true,
      frame: { ...value, type: 'task.cancel', payload: payload.data } as GatewayFrame,
    };
  }
  if (value.type === 'agent_sessions.sync.result') {
    const payload = agentSessionsSyncResultSchema.safeParse(value.payload);
    if (!payload.success) {
      return {
        ok: false,
        error: publicError(
          INVALID_FRAME,
          'agent_sessions.sync.result 负载不合法',
          value.message_id,
        ),
      };
    }
    return {
      ok: true,
      frame: {
        ...value,
        type: 'agent_sessions.sync.result',
        payload: payload.data,
      } as GatewayFrame,
    };
  }
  if (value.type === 'binding.result') {
    const payload = bindingResultSchema.safeParse(value.payload);
    if (!payload.success) {
      return { ok: false, error: publicError(INVALID_FRAME, 'binding.result 负载不合法', value.message_id) };
    }
    return { ok: true, frame: { ...value, type: 'binding.result', payload: payload.data } as GatewayFrame };
  }
  return {
    ok: false,
    error: publicError(INVALID_FRAME, `未知的 Gateway 消息类型：${value.type}`, value.message_id),
  };
}

/** Build the contract-defined Desktop -> Gateway device binding request. */
export function buildDeviceBindFrame(input: {
  messageId: string;
  sequence: number;
  macAddress: string;
  sentAt: string;
}): DesktopFrame<'device.bind'> {
  if (!MAC_ADDRESS.test(input.macAddress)) {
    throw new Error('device.bind requires a valid MAC address');
  }
  return {
    protocol_version: PROTOCOL_VERSION,
    message_id: input.messageId,
    sequence: input.sequence,
    type: 'device.bind',
    sent_at: input.sentAt,
    payload: { mac_address: input.macAddress },
  };
}

/** Build the `client.hello` frame. */
export function buildHelloFrame(input: {
  messageId: string;
  sequence: number;
  desktopVersion: string;
  lastAckedSequence: number;
  capabilities: readonly string[];
  sentAt: string;
}): DesktopFrame<'client.hello'> {
  return {
    protocol_version: PROTOCOL_VERSION,
    message_id: input.messageId,
    sequence: input.sequence,
    type: 'client.hello',
    sent_at: input.sentAt,
    payload: {
      desktop_version: input.desktopVersion,
      protocol_versions: [PROTOCOL_VERSION],
      last_acked_sequence: input.lastAckedSequence,
      capabilities: input.capabilities,
    },
  };
}

/** Build the `task.ack` frame. */
export function buildAckFrame(input: {
  messageId: string;
  sequence: number;
  taskId: string;
  disposition: Disposition;
  sentAt: string;
  error?: PublicError;
}): DesktopFrame<'task.ack'> {
  return {
    protocol_version: PROTOCOL_VERSION,
    message_id: input.messageId,
    sequence: input.sequence,
    type: 'task.ack',
    sent_at: input.sentAt,
    payload: {
      message_id: input.messageId,
      sequence: input.sequence,
      task_id: input.taskId,
      disposition: input.disposition,
      ...(input.error === undefined ? {} : { error: input.error }),
    },
  };
}

/** Build a task event frame. */
export function buildTaskEventFrame(input: {
  messageId: string;
  sequence: number;
  type: Extract<
    DesktopMessageType,
    | 'task.started'
    | 'task.progress'
    | 'task.approval_required'
    | 'task.completed'
    | 'task.failed'
    | 'task.cancelled'
    | 'task.interrupted'
  >;
  payload: TaskEventPayload;
  sentAt: string;
}): DesktopFrame<typeof input.type> {
  return {
    protocol_version: PROTOCOL_VERSION,
    message_id: input.messageId,
    sequence: input.sequence,
    type: input.type,
    sent_at: input.sentAt,
    payload: input.payload,
  };
}

/** Build a `client.heartbeat` frame. */
export function buildHeartbeatFrame(input: {
  messageId: string;
  sequence: number;
  clientTime: string;
  summary?: string;
}): DesktopFrame<'client.heartbeat'> {
  return {
    protocol_version: PROTOCOL_VERSION,
    message_id: input.messageId,
    sequence: input.sequence,
    type: 'client.heartbeat',
    sent_at: input.clientTime,
    payload: {
      client_time: input.clientTime,
      ...(input.summary === undefined ? {} : { summary: input.summary }),
    },
  };
}

/** Build a full replacement of the device-visible Codex session directory. */
export function buildAgentSessionsSyncFrame(input: {
  messageId: string;
  sequence: number;
  items: readonly AgentSessionCatalogItem[];
  sentAt: string;
}): DesktopFrame<'agent_sessions.sync'> {
  return {
    protocol_version: PROTOCOL_VERSION,
    message_id: input.messageId,
    sequence: input.sequence,
    type: 'agent_sessions.sync',
    sent_at: input.sentAt,
    payload: { items: input.items },
  };
}

/** Build a public error payload. */
export function publicError(
  code: PublicErrorCode,
  message: string,
  requestId: string,
  details?: Record<string, string | number | boolean>,
): PublicError {
  return {
    code,
    message,
    retryable: false,
    request_id: requestId,
    ...(details === undefined ? {} : { details }),
  };
}

/** Re-exported so the Desktop side can build a dispatch without importing contracts twice. */
export type { GatewayMessageType, ProjectCapability };
