/**
 * Type declarations for the mock Gateway console script.
 *
 * The script itself is plain ESM JavaScript, so it is not part of the
 * TypeScript program. These declarations let the tests import it with full
 * type information instead of falling back to `any`.
 */

/** Path the Desktop connects to. */
export declare const CONNECT_PATH: '/v1/clients/connect';

/** Default port of the mock Gateway. */
export declare const DEFAULT_PORT: number;

/** Default access token the mock Gateway accepts. */
export declare const DEFAULT_TOKEN: string;

/** Maximum number of frames kept for the console. */
export declare const MAX_HISTORY: number;

/** The payload of a `task.dispatch` frame. */
export type MockDispatchPayload = {
  task_id: string;
  project_id: string;
  agent: string;
  capabilities: string[];
  prompt: string;
  acceptance_criteria: string[];
  policy_version: string;
  continuation: { parent_task_id: string } | null;
};

/** The payload of a `task.cancel` frame. */
export type MockCancelPayload = {
  task_id: string;
  reason: string;
};

/** A frame as it travels over the wire. */
export type MockFrame = {
  readonly protocol_version: string;
  readonly message_id: string;
  readonly sequence: number;
  readonly type: string;
  readonly sent_at: string;
  readonly expires_at?: string;
  readonly payload: MockDispatchPayload | MockCancelPayload | Record<string, unknown>;
};

/** A `task.dispatch` frame, whose payload shape is known. */
export type MockDispatchFrame = MockFrame & { readonly payload: MockDispatchPayload };

/** A `task.cancel` frame, whose payload shape is known. */
export type MockCancelFrame = MockFrame & { readonly payload: MockCancelPayload };

/** One row of the console message list. */
export type MockConsoleEntry = {
  readonly id: string;
  readonly direction: 'inbound' | 'outbound';
  readonly type: string;
  readonly summary: string;
  readonly receivedAt: string;
  readonly frame: MockFrame;
};

/** A running mock Gateway with its browser console. */
export type MockGatewayConsole = {
  readonly port: number;
  readonly received: MockFrame[];
  readonly sent: MockFrame[];
  close(): Promise<void>;
};

/** Build a `task.dispatch` frame. */
export declare function buildDispatchFrame(
  overrides?: Partial<MockDispatchPayload>,
): MockDispatchFrame;

/** Build a `task.cancel` frame. */
export declare function buildCancelFrame(taskId: string, reason?: string): MockCancelFrame;

/** Summarise a frame for the console list. */
export declare function summarizeFrame(frame: unknown): string;

/** Build a console entry for a frame already in the history. */
export declare function toHistoryEntry(
  frame: unknown,
  direction: 'inbound' | 'outbound',
): MockConsoleEntry;

/** Start the mock Gateway with a browser console. */
export declare function startMockGatewayConsole(options?: {
  port?: number;
  token?: string;
  onFrame?: (frame: MockFrame) => void;
  verbose?: boolean;
}): Promise<MockGatewayConsole>;
