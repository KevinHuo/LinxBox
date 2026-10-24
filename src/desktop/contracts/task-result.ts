/**
 * `task-result-v1` — draft slice of SPEC-0001.
 *
 * Provenance: transcribed from the MVP contract baseline drafted in SPEC-0003
 * (`docs/contracts/domain.md` on the `feat/9-desktop-client` branch).
 *
 * Status: `Proposed`. Replace with the shared schema once SPEC-0001 lands.
 */

/** Contract marker carried by every result payload. */
export const TASK_RESULT_CONTRACT_VERSION = 'task-result-v1';

export const TASK_RESULT_STATUSES = [
  'succeeded',
  'failed',
  'cancelled',
  'interrupted',
  'rejected',
] as const;

export type TaskResultStatus = (typeof TASK_RESULT_STATUSES)[number];

export const VERIFICATION_OUTCOMES = ['passed', 'failed', 'not_run'] as const;

export type VerificationOutcome = (typeof VERIFICATION_OUTCOMES)[number];

/** A single changed file, relative to the authorized project root. */
export interface TaskResultChange {
  readonly path: string;
  readonly description: string;
}

/** A verification command that was actually executed. */
export interface TaskResultVerification {
  readonly command: string;
  readonly outcome: VerificationOutcome;
  readonly summary: string;
}

/** An unresolved risk worth surfacing to the operator. */
export interface TaskResultRisk {
  readonly code: string;
  readonly summary: string;
}

/** An explicit user action that does not itself perform a high-risk operation. */
export interface TaskResultUserAction {
  readonly summary: string;
}

/**
 * The result of a task.
 *
 * `succeeded` is only legal when the requested scope is complete, every claimed
 * change is provable from local execution facts, every claimed verification was
 * really run, and the provider terminal state is explicitly successful.
 */
export interface TaskResult {
  readonly contract_version: typeof TASK_RESULT_CONTRACT_VERSION;
  readonly status: TaskResultStatus;
  readonly summary: string;
  readonly changes: readonly TaskResultChange[];
  readonly verification: readonly TaskResultVerification[];
  readonly risks: readonly TaskResultRisk[];
  readonly needs_user_action: TaskResultUserAction | null;
  readonly voice_summary: string;
}

/** Maximum length of `voice_summary`, counted in Chinese characters. */
export const VOICE_SUMMARY_MAX_LENGTH = 120;
