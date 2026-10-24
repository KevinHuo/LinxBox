/**
 * Codex app-server wire protocol.
 *
 * Provenance: observed against the locally installed
 * `codex-cli 0.164.0-alpha.11.1` (ChatGPT.app bundle) by driving
 * `codex app-server --stdio` over a real stdio JSON-RPC session.
 *
 * The app-server speaks newline-delimited JSON-RPC 2.0. Every message is one
 * JSON object on a single line. There are three shapes:
 *
 *  - request   `{ jsonrpc, id, method, params }`   client -> server
 *  - response  `{ id, result }` or `{ id, error }`  server -> client
 *  - notification `{ jsonrpc, method, params }`    server -> client
 *
 * The server may also send *requests* to the client (for example an approval
 * request). The Desktop has no trusted approval surface, so those are
 * rejected rather than auto-approved.
 */

import { z } from 'zod';

export const JSONRPC_VERSION = '2.0';

/** JSON-RPC error code used when the Desktop refuses a server-initiated request. */
export const METHOD_NOT_FOUND = -32601;

/** Client -> server request. */
export interface JsonRpcRequest {
  readonly jsonrpc: typeof JSONRPC_VERSION;
  readonly id: number;
  readonly method: string;
  readonly params: unknown;
}

/** Server -> client notification. */
export interface JsonRpcNotification {
  readonly jsonrpc: typeof JSONRPC_VERSION;
  readonly method: string;
  readonly params: unknown;
}

/** Server -> client request. */
export interface JsonRpcServerRequest {
  readonly jsonrpc: typeof JSONRPC_VERSION;
  readonly id: number | string;
  readonly method: string;
  readonly params: unknown;
}

/** Server -> client success response. */
export interface JsonRpcSuccessResponse {
  readonly id: number;
  readonly result: unknown;
}

/** Server -> client error response. */
export interface JsonRpcErrorResponse {
  readonly id: number;
  readonly error: {
    readonly code: number;
    readonly message: string;
    readonly data?: unknown;
  };
}

export type JsonRpcResponse = JsonRpcSuccessResponse | JsonRpcErrorResponse;

export type JsonRpcMessage =
  | JsonRpcRequest
  | JsonRpcNotification
  | JsonRpcServerRequest
  | JsonRpcSuccessResponse
  | JsonRpcErrorResponse;

/* ------------------------------------------------------------------ */
/* Method names                                                        */
/* ------------------------------------------------------------------ */

export const CODEX_METHODS = {
  initialize: 'initialize',
  initialized: 'initialized',
  configRead: 'config/read',
  threadStart: 'thread/start',
  threadResume: 'thread/resume',
  threadList: 'thread/list',
  threadRead: 'thread/read',
  threadTurnsList: 'thread/turns/list',
  projectList: 'project/list',
  turnStart: 'turn/start',
  turnInterrupt: 'turn/interrupt',
} as const;

export const CODEX_NOTIFICATIONS = {
  turnStarted: 'turn/started',
  turnCompleted: 'turn/completed',
  itemStarted: 'item/started',
  itemCompleted: 'item/completed',
  error: 'error',
  warning: 'warning',
  threadStarted: 'thread/started',
  accountUpdated: 'account/updated',
} as const;

/** Server-initiated requests the Desktop must answer. */
export const CODEX_APPROVAL_METHODS = [
  'item/commandExecution/requestApproval',
  'item/fileChange/requestApproval',
] as const;

/* ------------------------------------------------------------------ */
/* Response shapes                                                     */
/* ------------------------------------------------------------------ */

export const initializeResultSchema = z.looseObject({
  userAgent: z.string(),
  codexHome: z.string(),
  platformFamily: z.string(),
  platformOs: z.string(),
});

export type InitializeResult = z.infer<typeof initializeResultSchema>;

export const configReadResultSchema = z.looseObject({
  config: z.looseObject({
    model: z.string().nullable().optional(),
    model_provider: z.string().nullable().optional(),
  }),
});

export type ConfigReadResult = z.infer<typeof configReadResultSchema>;

export const threadSchema = z.looseObject({
  id: z.string(),
  model: z.string().optional(),
  modelProvider: z.string().optional(),
  preview: z.string().optional(),
});

export const threadStartResultSchema = z.looseObject({
  thread: threadSchema,
});

export const threadResumeResultSchema = z.looseObject({
  thread: threadSchema,
});

export const turnSchema = z.looseObject({
  id: z.string(),
  status: z.string(),
  error: z.looseObject({ message: z.string().optional() }).nullable().optional(),
});

export const turnStartResultSchema = z.looseObject({
  turn: turnSchema,
});

export const turnCompletedParamsSchema = z.looseObject({
  threadId: z.string().optional(),
  turnId: z.string().optional(),
  turn: turnSchema.optional(),
});

export type TurnCompletedParams = z.infer<typeof turnCompletedParamsSchema>;

export const agentMessageItemSchema = z.looseObject({
  type: z.literal('agentMessage'),
  id: z.string().optional(),
  phase: z.string().optional(),
  text: z.string().optional(),
});

export const itemCompletedParamsSchema = z.looseObject({
  threadId: z.string().optional(),
  turnId: z.string().optional(),
  item: z.unknown(),
});

export type ItemCompletedParams = z.infer<typeof itemCompletedParamsSchema>;

export const accountUpdatedParamsSchema = z.looseObject({
  authMode: z.string().optional(),
  planType: z.string().optional(),
});

export type AccountUpdatedParams = z.infer<typeof accountUpdatedParamsSchema>;

/**
 * Parse one raw line into a JSON-RPC message.
 *
 * Returns `null` for a line that is not a JSON object. A malformed frame is a
 * protocol error, not something to guess at.
 */
export function parseJsonRpcMessage(line: string): JsonRpcMessage | null {
  let parsed: unknown;
  try {
    parsed = JSON.parse(line);
  } catch {
    return null;
  }
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    return null;
  }
  return parsed as JsonRpcMessage;
}

/** Whether a message is a server-initiated request that expects a response. */
export function isServerRequest(message: JsonRpcMessage): message is JsonRpcServerRequest {
  return 'method' in message && 'id' in message && message.id !== undefined;
}

/** Whether a message is a server notification without a response. */
export function isNotification(message: JsonRpcMessage): message is JsonRpcNotification {
  return 'method' in message && !('id' in message);
}

/** Whether a message is a response to a client request. */
export function isResponse(message: JsonRpcMessage): message is JsonRpcResponse {
  return 'id' in message && !('method' in message);
}
