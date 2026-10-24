/**
 * The renderer-facing API surface, expressed without any Electron import.
 *
 * Keeping the shape here makes it unit testable and guarantees that the
 * preload bridge exposes named methods only. There is deliberately no generic
 * `invoke(channel, payload)`: the renderer cannot reach a channel that is not
 * listed here.
 */
import { channelFor, type IpcMethod } from '../shared/ipc-channels.js';

export type InvokeFn = (channel: string, payload: unknown) => Promise<unknown>;

/** The methods the preload bridge exposes to the renderer. */
export interface LinxboxApi {
  probeCodex(): Promise<unknown>;
  listCodexSessions(payload?: unknown): Promise<unknown>;
  listCodexProjects(payload?: unknown): Promise<unknown>;
  readCodexConversation(payload: unknown): Promise<unknown>;
  continueCodexSession(payload: unknown): Promise<unknown>;
  readCodexSession(payload: unknown): Promise<unknown>;
  startCodexTurn(payload: unknown): Promise<unknown>;
  readCodexTurn(payload?: unknown): Promise<unknown>;
  interruptCodexTurn(payload: unknown): Promise<unknown>;
  readConnection(): Promise<unknown>;
  saveConnection(payload: unknown): Promise<unknown>;
  testConnection(): Promise<unknown>;
  listProjects(): Promise<unknown>;
  grantProject(payload: unknown): Promise<unknown>;
  revokeProject(payload: unknown): Promise<unknown>;
  listTasks(payload?: unknown): Promise<unknown>;
  getTask(payload: unknown): Promise<unknown>;
  cancelTask(payload: unknown): Promise<unknown>;
  readDiagnostics(): Promise<unknown>;
}

/**
 * Build the API object from an injected `invoke` function.
 *
 * Each method forwards to exactly one allowlisted channel with a fixed
 * payload shape.
 */
export function createLinxboxApi(invoke: InvokeFn): LinxboxApi {
  const call = (method: IpcMethod, payload: unknown = {}): Promise<unknown> =>
    invoke(channelFor(method), payload);

  return {
    probeCodex: () => call('codex.probe'),
    listCodexSessions: (payload = {}) => call('codex.sessions.list', payload),
    listCodexProjects: (payload = {}) => call('codex.projects.list', payload),
    readCodexConversation: (payload) => call('codex.conversation.read', payload),
    continueCodexSession: (payload) => call('codex.session.continue', payload),
    readCodexSession: (payload) => call('codex.sessions.read', payload),
    startCodexTurn: (payload) => call('codex.turn.start', payload),
    readCodexTurn: (payload = {}) => call('codex.turn.status', payload),
    interruptCodexTurn: (payload) => call('codex.turn.interrupt', payload),
    readConnection: () => call('connection.read'),
    saveConnection: (payload) => call('connection.save', payload),
    testConnection: () => call('connection.test'),
    listProjects: () => call('projects.list'),
    grantProject: (payload) => call('projects.grant', payload),
    revokeProject: (payload) => call('projects.revoke', payload),
    listTasks: (payload = {}) => call('tasks.list', payload),
    getTask: (payload) => call('tasks.get', payload),
    cancelTask: (payload) => call('tasks.cancel', payload),
    readDiagnostics: () => call('diagnostics.read'),
  };
}

/** Name of the object exposed on `window` by the preload bridge. */
export const BRIDGE_NAMESPACE = 'linxbox';
