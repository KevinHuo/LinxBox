/**
 * Gateway/Desktop `1.0` wire contract — draft slice of SPEC-0001.
 *
 * Provenance: transcribed from the `Gateway/Desktop 1.0` section of the MVP
 * contract baseline drafted in SPEC-0003 (`docs/contracts/gateway-desktop.md`
 * on the `feat/9-desktop-client` branch). That baseline is not on the default
 * branch yet, so this file is the local, reviewable copy that the Desktop
 * implementation compiles against.
 *
 * Status: `Proposed`. This slice is NOT the authoritative contract. When
 * SPEC-0001 lands the shared schema and fixtures, this file must be replaced
 * by an import of that schema rather than kept as a second source of truth.
 */
import type { TaskResult } from './task-result.js';

/** Negotiated protocol version implemented by this Desktop build. */
export const PROTOCOL_VERSION = '1.0';

/** Protocol versions this build is able to speak. */
export const SUPPORTED_PROTOCOL_VERSIONS = ['1.0'] as const;

export type ProtocolVersion = (typeof SUPPORTED_PROTOCOL_VERSIONS)[number];

/** Capability names a project may be granted. */
export const PROJECT_CAPABILITIES = ['read', 'write', 'test'] as const;

export type ProjectCapability = (typeof PROJECT_CAPABILITIES)[number];

/** Message types the Gateway may send to a Desktop client. */
export const GATEWAY_MESSAGE_TYPES = [
  'task.dispatch',
  'task.cancel',
  'project.upsert',
  'project.revoke',
] as const;

export type GatewayMessageType = (typeof GATEWAY_MESSAGE_TYPES)[number];

/** Message types a Desktop client may send to the Gateway. */
export const DESKTOP_MESSAGE_TYPES = [
  'client.hello',
  'client.heartbeat',
  'task.ack',
  'task.started',
  'task.progress',
  'task.approval_required',
  'task.completed',
  'task.failed',
  'task.cancelled',
  'task.interrupted',
  'presentation.upsert',
  'presentation.revoke',
  'command.receipt',
] as const;

export type DesktopMessageType = (typeof DESKTOP_MESSAGE_TYPES)[number];

/** Common envelope shared by every frame on the wire. */
export interface Frame<TType extends string, TPayload> {
  readonly protocol_version: ProtocolVersion;
  readonly message_id: string;
  readonly sequence: number;
  readonly type: TType;
  readonly sent_at: string;
  readonly expires_at?: string;
  readonly payload: TPayload;
}

/**
 * An inbound frame, correlated by message type.
 *
 * The payload type is tied to `type`, so narrowing on `type` narrows the
 * payload as well.
 */
export type GatewayFrame<TType extends GatewayMessageType = GatewayMessageType> = {
  [K in TType]: Frame<K, GatewayPayloadByType[K]>;
}[TType];

/** An inbound `task.dispatch` frame. */
export type TaskDispatchFrame = Extract<GatewayFrame, { type: 'task.dispatch' }>;

/** An inbound `task.cancel` frame. */
export type TaskCancelFrame = Extract<GatewayFrame, { type: 'task.cancel' }>;

export type DesktopFrame<TType extends DesktopMessageType = DesktopMessageType> = {
  [K in TType]: Frame<K, DesktopPayloadByType[K]>;
}[TType];

/** `client.hello` payload. */
export interface ClientHelloPayload {
  readonly desktop_version: string;
  readonly protocol_versions: readonly ProtocolVersion[];
  readonly last_acked_sequence: number;
  readonly capabilities: readonly string[];
}

/** `client.heartbeat` payload. Carries no task truth, only presence. */
export interface ClientHeartbeatPayload {
  readonly client_time: string;
  readonly summary?: string;
}

/** `project.upsert` payload. Absolute paths never appear in this protocol. */
export interface ProjectUpsertPayload {
  readonly project_id: string;
  readonly display_name: string;
  readonly capabilities: readonly ProjectCapability[];
  readonly availability: 'online' | 'offline';
  readonly grant_version: number;
}

/** `project.revoke` payload. */
export interface ProjectRevokePayload {
  readonly project_id: string;
  readonly grant_version: number;
  readonly revoked_at: string;
}

/** `task.dispatch` payload. */
export interface TaskDispatchPayload {
  readonly task_id: string;
  readonly project_id: string;
  readonly agent: string;
  readonly capabilities: readonly ProjectCapability[];
  readonly prompt: string;
  readonly acceptance_criteria: readonly string[];
  readonly policy_version: string;
  readonly continuation: { readonly parent_task_id: string } | null;
}

/** `task.cancel` payload. A cancel request is not proof of a terminal state. */
export interface TaskCancelPayload {
  readonly task_id: string;
  readonly reason: string;
}

/** Outcome of a delivery, returned to the Gateway in `task.ack`. */
export const DISPOSITIONS = ['accepted', 'duplicate', 'expired', 'rejected'] as const;

export type Disposition = (typeof DISPOSITIONS)[number];

/** `task.ack` payload. */
export interface TaskAckPayload {
  readonly message_id: string;
  readonly sequence: number;
  readonly task_id: string;
  readonly disposition: Disposition;
  readonly error?: PublicError;
}

/** Payload shared by every task event frame. */
export interface TaskEventPayload {
  readonly task_id: string;
  readonly event_id: string;
  readonly event_sequence: number;
  readonly occurred_at: string;
  readonly state: string;
  readonly stage: string;
  readonly summary: string;
  readonly result: TaskResult | null;
  readonly error: PublicError | null;
  /**
   * The provider thread this task actually runs in.
   *
   * `null` until the Desktop has started the turn. It is the only stable
   * handle the Gateway can use to locate the local conversation.
   */
  readonly thread_id: string | null;
  /**
   * The provider turn this task actually runs as.
   *
   * `null` until the Desktop has started the turn.
   */
  readonly turn_id: string | null;
}

export interface GatewayPayloadByType {
  'task.dispatch': TaskDispatchPayload;
  'task.cancel': TaskCancelPayload;
  'project.upsert': ProjectUpsertPayload;
  'project.revoke': ProjectRevokePayload;
}

export interface DesktopPayloadByType {
  'client.hello': ClientHelloPayload;
  'client.heartbeat': ClientHeartbeatPayload;
  'task.ack': TaskAckPayload;
  'task.started': TaskEventPayload;
  'task.progress': TaskEventPayload;
  'task.approval_required': TaskEventPayload;
  'task.completed': TaskEventPayload;
  'task.failed': TaskEventPayload;
  'task.cancelled': TaskEventPayload;
  'task.interrupted': TaskEventPayload;
  'presentation.upsert': Record<string, unknown>;
  'presentation.revoke': Record<string, unknown>;
  'command.receipt': Record<string, unknown>;
}

/**
 * Public error shape shared by every boundary.
 *
 * `details` must never contain tokens, cookies, absolute paths, raw prompts,
 * code, command output, environment variables or provider-internal errors.
 */
export interface PublicError {
  readonly code: PublicErrorCode;
  readonly message: string;
  readonly retryable: boolean;
  readonly request_id: string;
  readonly details?: Record<string, string | number | boolean>;
}

export const PUBLIC_ERROR_CODES = [
  'UNAUTHENTICATED',
  'PERMISSION_DENIED',
  'INVALID_ARGUMENT',
  'NOT_FOUND',
  'CONFLICT',
  'RESOURCE_EXHAUSTED',
  'DEPENDENCY_UNAVAILABLE',
  'APPROVAL_REQUIRED',
  'POLICY_DENIED',
  'INTERNAL',
] as const;

export type PublicErrorCode = (typeof PUBLIC_ERROR_CODES)[number];
