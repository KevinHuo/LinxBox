/**
 * IPC allowlist between the sandboxed renderer and the Electron main process.
 *
 * The renderer never receives a generic `invoke(channel, payload)` function.
 * It gets a fixed set of named methods, and every payload is validated at
 * runtime against a strict schema before a handler sees it.
 *
 * The renderer also never receives a token, a raw provider event or an
 * absolute path of an unauthorised directory.
 */
import { z } from 'zod';

export {
  IPC_CHANNELS,
  IPC_EVENT_CHANNELS,
  IPC_EVENT_CHANNEL_LIST,
  IPC_METHODS,
  IPC_NAMESPACE,
  IPC_VERSION,
  channelFor,
  isAllowedChannel,
  isAllowedEventChannel,
  type IpcChannel,
  type IpcEventChannel,
  type IpcMethod,
} from './ipc-channels.js';

import { type IpcMethod } from './ipc-channels.js';

/* ------------------------------------------------------------------ */
/* Request payloads                                                     */
/* ------------------------------------------------------------------ */

export const projectCapabilitySchema = z.enum(['read', 'write', 'test']);

const emptyPayloadSchema = z.object({}).strict();

const connectionSaveSchema = z
  .object({
    gatewayUrl: z.string().url().max(2048),
    accessToken: z.string().min(1).max(4096),
    computerName: z.string().min(1).max(128),
  })
  .strict();

/**
 * The project root is deliberately absent.
 *
 * A grant is only created from the macOS directory picker inside the main
 * process. Accepting a path from the renderer would let a compromised renderer
 * authorise any directory on the machine.
 */
const projectGrantSchema = z
  .object({
    displayName: z.string().min(1).max(200),
    capabilities: z
      .array(projectCapabilitySchema)
      .min(1)
      .max(projectCapabilitySchema.options.length),
  })
  .strict();

const projectRevokeSchema = z
  .object({
    projectId: z.string().min(1).max(128),
  })
  .strict();

const taskGetSchema = z
  .object({
    taskId: z.string().min(1).max(128),
  })
  .strict();

const taskCancelSchema = z
  .object({
    taskId: z.string().min(1).max(128),
  })
  .strict();

const taskListSchema = z
  .object({
    limit: z.number().int().min(1).max(200).optional(),
  })
  .strict();

const codexProbeSchema = z.object({}).strict();

const codexTurnStartSchema = z
  .object({
    prompt: z.string().min(1).max(20_000),
    projectId: z.string().min(1).max(128),
  })
  .strict();

const codexTurnStatusSchema = z
  .object({
    turnId: z.string().min(1).max(128).optional(),
  })
  .strict();

const codexTurnInterruptSchema = z
  .object({
    turnId: z.string().min(1).max(128),
  })
  .strict();

const codexSessionsListSchema = z
  .object({
    limit: z.number().int().min(1).max(200).optional(),
  })
  .strict();

const codexSessionsReadSchema = z
  .object({
    threadId: z.string().min(1).max(128),
  })
  .strict();

const codexSessionContinueSchema = z
  .object({
    threadId: z.string().min(1).max(128),
    prompt: z.string().min(1).max(20_000),
  })
  .strict();

const codexProjectsListSchema = z.object({}).strict();

const codexConversationReadSchema = z
  .object({
    threadId: z.string().min(1).max(128),
  })
  .strict();

/**
 * Appearance preference.
 *
 * The theme is a local presentation choice. It never leaves the machine and
 * never changes what the Desktop executes.
 */
export const appearanceThemeSchema = z.enum(['warm', 'dark']);

/** The appearance themes the Desktop offers. */
export type AppearanceTheme = z.infer<typeof appearanceThemeSchema>;

/**
 * The floating window position.
 *
 * The renderer reports where the user dropped the ball. The main process
 * clamps it to the display work area before applying it.
 */
const floatMoveSchema = z
  .object({
    x: z.number().finite(),
    y: z.number().finite(),
  })
  .strict();

const floatSetVisibleSchema = z
  .object({
    visible: z.boolean(),
  })
  .strict();

const appearanceSaveSchema = z
  .object({
    theme: appearanceThemeSchema,
  })
  .strict();

export type ConnectionSaveInput = z.infer<typeof connectionSaveSchema>;
export type ProjectGrantInput = z.infer<typeof projectGrantSchema>;
export type ProjectRevokeInput = z.infer<typeof projectRevokeSchema>;
export type TaskGetInput = z.infer<typeof taskGetSchema>;
export type TaskCancelInput = z.infer<typeof taskCancelSchema>;
export type TaskListInput = z.infer<typeof taskListSchema>;
export type AppearanceSaveInput = z.infer<typeof appearanceSaveSchema>;
export type FloatMoveInput = z.infer<typeof floatMoveSchema>;
export type FloatSetVisibleInput = z.infer<typeof floatSetVisibleSchema>;

export const IPC_REQUEST_SCHEMAS: Readonly<Record<IpcMethod, z.ZodType>> = {
  'codex.probe': codexProbeSchema,
  'codex.turn.start': codexTurnStartSchema,
  'codex.turn.status': codexTurnStatusSchema,
  'codex.turn.interrupt': codexTurnInterruptSchema,
  'codex.sessions.list': codexSessionsListSchema,
  'codex.sessions.read': codexSessionsReadSchema,
  'codex.session.continue': codexSessionContinueSchema,
  'codex.projects.list': codexProjectsListSchema,
  'codex.conversation.read': codexConversationReadSchema,
  'connection.read': emptyPayloadSchema,
  'connection.save': connectionSaveSchema,
  'connection.test': emptyPayloadSchema,
  'projects.list': emptyPayloadSchema,
  'projects.grant': projectGrantSchema,
  'projects.revoke': projectRevokeSchema,
  'tasks.list': taskListSchema,
  'tasks.get': taskGetSchema,
  'tasks.cancel': taskCancelSchema,
  'diagnostics.read': emptyPayloadSchema,
  'appearance.read': emptyPayloadSchema,
  'appearance.save': appearanceSaveSchema,
  'window.float.read': emptyPayloadSchema,
  'window.float.setVisible': floatSetVisibleSchema,
  'window.float.move': floatMoveSchema,
};

/* ------------------------------------------------------------------ */
/* Response payloads                                                    */
/* ------------------------------------------------------------------ */

/**
 * Connection state shown to the renderer.
 *
 * The access token is never part of this shape. Only a short hint that lets
 * the operator confirm which token is stored.
 */
export interface ConnectionView {
  readonly configured: boolean;
  readonly connected: boolean;
  readonly gatewayUrl: string | null;
  readonly tokenHint: string | null;
  readonly computerName: string | null;
}

/**
 * The live part of the connection state.
 *
 * The main process reports what the Gateway client actually knows: whether the
 * socket is up, and the last error it hit.
 */
export interface GatewayStatusView {
  readonly configured: boolean;
  readonly connected: boolean;
  readonly lastError: string | null;
  readonly lastActivityAt: string | null;
}

/** The connection facts and the live status together. */
export interface ConnectionStateView extends ConnectionView, GatewayStatusView {}

/**
 * A locally authorised project.
 *
 * `rootPath` is the realpath the user picked in the system dialog. It stays
 * on the machine and never reaches the Gateway.
 */
export interface ProjectView {
  readonly projectId: string;
  readonly displayName: string;
  readonly rootPath: string;
  readonly capabilities: readonly string[];
  readonly grantVersion: number;
  readonly enabled: boolean;
}

/** Compact task row for the task list. */
export interface TaskSummaryView {
  readonly taskId: string;
  readonly title: string;
  readonly projectId: string;
  readonly state: string;
  readonly stage: string;
  readonly updatedAt: string;
  readonly needsAttention: boolean;
}

/** Full local task detail, including the local diff that never leaves the machine. */
export interface TaskDetailView {
  readonly taskId: string;
  readonly title: string;
  readonly projectId: string;
  readonly state: string;
  readonly stage: string;
  readonly intent: string;
  readonly summary: string;
  readonly changes: readonly { readonly path: string; readonly description: string }[];
  readonly verification: readonly {
    readonly command: string;
    readonly outcome: string;
    readonly summary: string;
  }[];
  readonly risks: readonly { readonly code: string; readonly summary: string }[];
  readonly needsUserAction: { readonly summary: string } | null;
  readonly localDiff: string | null;
}

/** Local diagnostics. Contains no token, no absolute path and no task content. */
export interface DiagnosticsView {
  readonly desktopVersion: string;
  readonly protocolVersions: readonly string[];
  readonly databasePath: string | null;
  readonly connected: boolean;
  readonly lastError: string | null;
}

/**
 * What the local Codex app-server reports about itself.
 *
 * `binaryPath` is a local fact and never leaves the machine.
 */
export interface CodexProbeView {
  readonly available: boolean;
  readonly binaryPath: string | null;
  readonly version: string | null;
  readonly userAgent: string | null;
  readonly authMode: string | null;
  readonly planType: string | null;
  readonly error: string | null;
}

/**
 * A session the local provider knows about.
 *
 * This is a local read. The transcript is the user's own conversation and
 * never leaves the machine.
 */
export interface CodexSessionSummaryView {
  readonly threadId: string;
  readonly title: string;
  readonly cwd: string | null;
  readonly model: string | null;
  readonly createdAt: string | null;
  readonly updatedAt: string | null;
  readonly status: string;
}

/** One message inside a session transcript. */
export interface CodexSessionMessageView {
  readonly id: string;
  readonly role: 'user' | 'assistant';
  readonly text: string;
  readonly phase: string | null;
}

/** A session with its transcript. */
export interface CodexSessionDetailView {
  readonly threadId: string;
  readonly title: string;
  readonly cwd: string | null;
  readonly turns: readonly {
    readonly turnId: string;
    readonly status: string;
    readonly startedAt: string | null;
    readonly messages: readonly CodexSessionMessageView[];
  }[];
}

/** A project the provider groups working directories under. */
export interface CodexProjectView {
  readonly projectId: string;
  readonly name: string;
  readonly roots: readonly string[];
  readonly position: number;
}

/** The kind of a detail entry inside a conversation. */
export type CodexDetailKind =
  | 'user'
  | 'agent'
  | 'reasoning'
  | 'command'
  | 'file-change'
  | 'mcp'
  | 'web-search'
  | 'image'
  | 'image-generation'
  | 'compaction'
  | 'other';

/** One entry inside a conversation. */
export interface CodexDetailItemView {
  readonly kind: CodexDetailKind;
  readonly id: string;
  readonly title: string;
  readonly body: string;
  readonly meta: string | null;
}

/** A conversation with every detail entry. */
export interface CodexConversationView {
  readonly threadId: string;
  readonly title: string;
  readonly cwd: string | null;
  readonly turns: readonly {
    readonly turnId: string;
    readonly status: string;
    readonly startedAt: string | null;
    readonly details: readonly CodexDetailItemView[];
  }[];
}

/** The state of the turn the Desktop is currently driving. */
export interface CodexTurnView {
  readonly threadId: string | null;
  readonly turnId: string | null;
  readonly status: 'idle' | 'running' | 'completed' | 'failed' | 'interrupted';
  /** Latest provider commentary. */
  readonly progress: string;
  /** The provider's answer. */
  readonly finalText: string;
  readonly error: string | null;
  readonly startedAt: string | null;
  readonly completedAt: string | null;
}

/**
 * Appearance preference shown by the renderer.
 *
 * `warm` follows the hardware-side palette; `dark` is the desktop dark theme.
 * Both are presentation only.
 */
export interface AppearanceView {
  readonly theme: AppearanceTheme;
}

/**
 * Floating window facts.
 *
 * The position is reported in screen coordinates so the renderer can place the
 * ball before the first paint. It is `null` until the user moves the ball.
 */
export interface FloatWindowView {
  readonly visible: boolean;
  readonly position: { readonly x: number; readonly y: number } | null;
}

/* ------------------------------------------------------------------ */
/* Request parsing                                                      */
/* ------------------------------------------------------------------ */

export type IpcParseResult<T> =
  | { readonly ok: true; readonly data: T }
  | { readonly ok: false; readonly code: string; readonly message: string };

/**
 * Validate a renderer request against the allowlist.
 *
 * Unknown methods, unknown fields and malformed values are all rejected
 * before any handler runs.
 */
export function parseIpcRequest(method: string, payload: unknown): IpcParseResult<unknown> {
  const schema = IPC_REQUEST_SCHEMAS[method as IpcMethod];
  if (!schema) {
    return {
      ok: false,
      code: 'INVALID_ARGUMENT',
      message: '未知的桌面端调用',
    };
  }
  const result = schema.safeParse(payload);
  if (!result.success) {
    return {
      ok: false,
      code: 'INVALID_ARGUMENT',
      message: '调用参数不合法',
    };
  }
  return { ok: true, data: result.data };
}

/**
 * Mask an access token so the operator can recognise it without seeing it.
 *
 * Only the last four characters survive; anything shorter is fully masked.
 */
export function maskToken(token: string): string {
  if (token.length <= 4) {
    return '•'.repeat(token.length);
  }
  return `${'•'.repeat(8)}${token.slice(-4)}`;
}
