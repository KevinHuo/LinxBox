/**
 * The renderer-facing API surface, expressed without any Electron import.
 *
 * Keeping the shape here makes it unit testable and guarantees that the
 * preload bridge exposes named methods only. There is deliberately no generic
 * `invoke(channel, payload)`: the renderer cannot reach a channel that is not
 * listed here.
 */
import { channelFor, IPC_EVENT_CHANNELS, type IpcMethod } from '../shared/ipc-channels.js';

export type InvokeFn = (channel: string, payload: unknown) => Promise<unknown>;

/** A listener the renderer subscribes with. */
export type EventListener = (payload: unknown) => void;

/**
 * Subscribe to a main-process event channel.
 *
 * Returns an unsubscribe function. The channel must be one the bridge
 * exposes; the renderer cannot subscribe to an arbitrary channel.
 */
export type SubscribeFn = (channel: string, listener: EventListener) => () => void;

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
  bindDevice(payload: unknown): Promise<unknown>;
  listProjects(): Promise<unknown>;
  grantProject(payload: unknown): Promise<unknown>;
  revokeProject(payload: unknown): Promise<unknown>;
  listTasks(payload?: unknown): Promise<unknown>;
  getTask(payload: unknown): Promise<unknown>;
  cancelTask(payload: unknown): Promise<unknown>;
  readDiagnostics(): Promise<unknown>;
  readAppearance(): Promise<unknown>;
  saveAppearance(payload: unknown): Promise<unknown>;
  readFloatWindow(): Promise<unknown>;
  setFloatWindowVisible(payload: unknown): Promise<unknown>;
  moveFloatWindow(payload: unknown): Promise<unknown>;
  focusMainWindow(): Promise<unknown>;
  /** Subscribe to the Gateway connection status changes. */
  onConnectionChanged(listener: EventListener): () => void;
  onTaskChanged(listener: EventListener): () => void;
}

/**
 * Build the API object from an injected `invoke` function.
 *
 * Each method forwards to exactly one allowlisted channel with a fixed
 * payload shape. The event subscription is separate: the renderer can only
 * listen to the named events the bridge exposes.
 */
export function createLinxboxApi(
  invoke: InvokeFn,
  subscribe: SubscribeFn = () => () => {},
): LinxboxApi {
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
    bindDevice: (payload) => call('device.bind', payload),
    listProjects: () => call('projects.list'),
    grantProject: (payload) => call('projects.grant', payload),
    revokeProject: (payload) => call('projects.revoke', payload),
    listTasks: (payload = {}) => call('tasks.list', payload),
    getTask: (payload) => call('tasks.get', payload),
    cancelTask: (payload) => call('tasks.cancel', payload),
    readDiagnostics: () => call('diagnostics.read'),
    readAppearance: () => call('appearance.read'),
    saveAppearance: (payload) => call('appearance.save', payload),
    readFloatWindow: () => call('window.float.read'),
    setFloatWindowVisible: (payload) => call('window.float.setVisible', payload),
    moveFloatWindow: (payload) => call('window.float.move', payload),
    focusMainWindow: () => call('window.main.focus'),
    onConnectionChanged: (listener) => subscribe(IPC_EVENT_CHANNELS.connectionChanged, listener),
    onTaskChanged: (listener) => subscribe(IPC_EVENT_CHANNELS.taskChanged, listener),
  };
}

/** Name of the object exposed on `window` by the preload bridge. */
export const BRIDGE_NAMESPACE = 'linxbox';
