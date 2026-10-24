/**
 * `task-domain/v1` — draft slice of SPEC-0001.
 *
 * Provenance: transcribed from the MVP contract baseline drafted in SPEC-0003
 * (`docs/contracts/domain.md` on the `feat/9-desktop-client` branch).
 *
 * Status: `Proposed`. Replace with the shared schema once SPEC-0001 lands.
 */

/** Every state a task may occupy. */
export const TASK_STATES = [
  'requested',
  'rejected',
  'queued',
  'waiting_for_client',
  'expired',
  'dispatched',
  'accepted',
  'running',
  'succeeded',
  'failed',
  'interrupted',
  'approval_required',
  'cancel_requested',
  'cancelled',
] as const;

export type TaskState = (typeof TASK_STATES)[number];

/** States a task can never leave. */
export const TERMINAL_TASK_STATES = [
  'rejected',
  'expired',
  'succeeded',
  'failed',
  'cancelled',
  'interrupted',
] as const;

export type TerminalTaskState = (typeof TERMINAL_TASK_STATES)[number];

/**
 * User-visible stage. It projects the task for display and never drives a
 * domain state transition.
 */
export const TASK_STAGES = [
  'validating',
  'waiting_for_client',
  'preparing',
  'analyzing',
  'editing',
  'testing',
  'summarizing',
  'waiting_for_approval',
  'finished',
] as const;

export type TaskStage = (typeof TASK_STAGES)[number];

/** Device-facing projection of a task state. */
export const DEVICE_TASK_STATES = [
  'queued',
  'needs_attention',
  'running',
  'completed',
  'failed',
] as const;

export type DeviceTaskState = (typeof DEVICE_TASK_STATES)[number];

/**
 * Allowed transitions of `task-domain/v1`.
 *
 * A terminal state never transitions back to an active state. `cancel_requested`
 * is reachable from any non-terminal state and only resolves into `cancelled`
 * or `interrupted`.
 */
export const TASK_TRANSITIONS: Readonly<Record<TaskState, readonly TaskState[]>> = {
  requested: ['rejected', 'queued'],
  rejected: [],
  queued: ['waiting_for_client', 'dispatched', 'expired', 'cancel_requested'],
  waiting_for_client: ['dispatched', 'expired', 'cancel_requested'],
  expired: [],
  dispatched: ['accepted', 'cancel_requested'],
  accepted: ['running', 'cancel_requested'],
  running: ['succeeded', 'failed', 'interrupted', 'approval_required', 'cancel_requested'],
  succeeded: [],
  failed: [],
  interrupted: [],
  approval_required: ['running', 'failed', 'cancelled'],
  cancel_requested: ['cancelled', 'interrupted'],
  cancelled: [],
};

/** Risk capability levels. */
export const RISK_LEVELS = ['R0', 'R1', 'R2', 'R3'] as const;

export type RiskLevel = (typeof RISK_LEVELS)[number];

/**
 * Risk levels a Desktop may execute without a trusted local approval UI.
 * R2 requires a trustworthy local approval surface; R3 is never approvable
 * from a voice entry point.
 */
export const RISK_RULES: Readonly<
  Record<RiskLevel, { readonly allowed: boolean; readonly reason: string }>
> = {
  R0: { allowed: true, reason: '只读既有项目数据' },
  R1: { allowed: true, reason: '编辑已授权项目并运行既有测试' },
  R2: { allowed: false, reason: '外部写入、联网、安装、提交或迁移需要可信本地审批' },
  R3: { allowed: false, reason: '推送、发布、部署、凭据与系统设置永久禁止从语音入口批准' },
};

/** States a Desktop owns locally, starting from `dispatched`. */
export const DESKTOP_OWNED_STATES = [
  'dispatched',
  'accepted',
  'running',
  'approval_required',
  'cancel_requested',
  'succeeded',
  'failed',
  'interrupted',
  'cancelled',
] as const;

/** Device projection per domain state. */
export const DEVICE_STATE_PROJECTION: Readonly<Record<TaskState, DeviceTaskState>> = {
  requested: 'queued',
  queued: 'queued',
  dispatched: 'queued',
  waiting_for_client: 'needs_attention',
  accepted: 'running',
  running: 'running',
  cancel_requested: 'running',
  approval_required: 'needs_attention',
  succeeded: 'completed',
  rejected: 'failed',
  expired: 'failed',
  failed: 'failed',
  cancelled: 'failed',
  interrupted: 'failed',
};

/** Identifier value objects. They are distinct types and never interchangeable. */
export type DeviceId = string & { readonly __brand: 'DeviceId' };
export type ClientId = string & { readonly __brand: 'ClientId' };
export type ProjectId = string & { readonly __brand: 'ProjectId' };
export type TaskId = string & { readonly __brand: 'TaskId' };
export type MessageId = string & { readonly __brand: 'MessageId' };
export type RequestId = string & { readonly __brand: 'RequestId' };
export type ProviderThreadId = string & { readonly __brand: 'ProviderThreadId' };
export type ProviderTurnId = string & { readonly __brand: 'ProviderTurnId' };
