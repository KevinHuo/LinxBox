/**
 * Gateway/Desktop `1.0` wire types used by the Desktop runtime.
 *
 * The authoritative field contract is `contracts/desktop-gateway.schema.json`.
 * Keep these compile-time types aligned with that Schema until type generation
 * is introduced; contract tests guard the legacy interfaces against drift.
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
  'binding.result',
  'agent_sessions.sync.result',
  'project.upsert',
  'project.revoke',
  'messages.list',
  'message.content.get',
] as const;

export type GatewayMessageType = (typeof GATEWAY_MESSAGE_TYPES)[number];

/** Message types a Desktop client may send to the Gateway. */
export const DESKTOP_MESSAGE_TYPES = [
  'client.hello',
  'client.heartbeat',
  'device.bind',
  'task.ack',
  'task.started',
  'task.progress',
  'task.approval_required',
  'task.completed',
  'task.failed',
  'task.cancelled',
  'task.interrupted',
  'agent_sessions.sync',
  'message.available',
  'query.result',
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

export interface DeviceBindPayload {
  readonly mac_address: string;
}

export interface BindingResultPayload {
  readonly reply_to_message_id: string;
  readonly ok: boolean;
  readonly data?: { readonly device_id: string; readonly desktop_client_id: string };
  readonly error?: ContractError;
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

export interface MessagesListPayload {
  readonly agent_session_id: string;
  readonly cursor?: string;
}

export interface ContentGetPayload {
  readonly agent_session_id: string;
  readonly message_id: string;
  readonly offset?: number;
}

export interface MessageMeta {
  readonly message_id: string;
  readonly role: 'user' | 'assistant';
  readonly summary: string;
}

export interface MessagePage {
  readonly items: readonly MessageMeta[];
  readonly next_cursor: string | null;
}

export interface ContentChunk {
  readonly text: string;
  readonly next_offset: number | null;
}

export interface MessageAvailablePayload {
  readonly agent_session_id: string;
  readonly message_id: string;
}

export interface QueryResultPayload {
  readonly reply_to_message_id: string;
  readonly ok: boolean;
  readonly data?: MessagePage | ContentChunk;
  readonly error?: ContractError;
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
  readonly continuation:
    | { readonly parent_task_id: string }
    | { readonly agent_session_id: string; readonly thread_id: string }
    | null;
}

/** A locally discovered Codex conversation that may be selected by a device. */
export interface AgentSessionCatalogItem {
  readonly agent_session_id: string;
  readonly thread_id: string;
  readonly project_id: string;
  readonly project_name: string;
  readonly agent: 'codex';
  readonly title: string;
  readonly status: 'idle' | 'running' | 'completed' | 'failed' | 'unknown';
  readonly summary: string;
}

/** Full replacement of the sessions this Desktop currently exposes. */
export interface AgentSessionsSyncPayload {
  readonly items: readonly AgentSessionCatalogItem[];
}

/** Gateway acknowledgement for one full session-directory sync. */
export interface AgentSessionsSyncResultPayload {
  readonly reply_to_message_id: string;
  readonly ok: true;
}

export interface AgentSessionsSyncErrorPayload {
  readonly reply_to_message_id: string;
  readonly ok: false;
  readonly error: ContractError;
}

/** Minimal error used by the new shared-contract operations. */
export interface ContractError {
  readonly code:
    | 'INVALID_ARGUMENT'
    | 'UNAUTHENTICATED'
    | 'PERMISSION_DENIED'
    | 'NOT_FOUND'
    | 'CONFLICT'
    | 'DEPENDENCY_UNAVAILABLE'
    | 'RESOURCE_EXHAUSTED'
    | 'INTERNAL';
  readonly message: string;
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
  'agent_sessions.sync.result': AgentSessionsSyncResultPayload | AgentSessionsSyncErrorPayload;
  'binding.result': BindingResultPayload;
  'project.upsert': ProjectUpsertPayload;
  'project.revoke': ProjectRevokePayload;
  'messages.list': MessagesListPayload;
  'message.content.get': ContentGetPayload;
}

export interface DesktopPayloadByType {
  'client.hello': ClientHelloPayload;
  'client.heartbeat': ClientHeartbeatPayload;
  'device.bind': DeviceBindPayload;
  'task.ack': TaskAckPayload;
  'task.started': TaskEventPayload;
  'task.progress': TaskEventPayload;
  'task.approval_required': TaskEventPayload;
  'task.completed': TaskEventPayload;
  'task.failed': TaskEventPayload;
  'task.cancelled': TaskEventPayload;
  'task.interrupted': TaskEventPayload;
  'agent_sessions.sync': AgentSessionsSyncPayload;
  'message.available': MessageAvailablePayload;
  'query.result': QueryResultPayload;
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
