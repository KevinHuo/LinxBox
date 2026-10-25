/**
 * IPC channel contract between the sandboxed renderer and the Electron main
 * process.
 *
 * This module is deliberately free of any dependency, including zod, so the
 * preload bundle stays small. The request/response schemas live in
 * `shared/ipc.ts`.
 */

export const IPC_NAMESPACE = 'linxbox';
export const IPC_VERSION = 'v1';

/** Logical methods the renderer may call. */
export const IPC_METHODS = [
  'codex.probe',
  'codex.turn.interrupt',
  'codex.turn.start',
  'codex.conversation.read',
  'codex.projects.list',
  'codex.session.continue',
  'codex.sessions.list',
  'codex.sessions.read',
  'codex.turn.status',
  'connection.read',
  'connection.save',
  'connection.test',
  'device.bind',
  'diagnostics.read',
  'projects.grant',
  'projects.list',
  'projects.revoke',
  'tasks.cancel',
  'tasks.get',
  'tasks.list',
  'appearance.read',
  'appearance.save',
  'window.float.move',
  'window.float.read',
  'window.float.setVisible',
  'window.main.focus',
  'app.quit',
] as const;

export type IpcMethod = (typeof IPC_METHODS)[number];

export type IpcChannel = `${typeof IPC_NAMESPACE}.${typeof IPC_VERSION}.${IpcMethod}`;

/** Every channel string the preload bridge is allowed to use. */
export const IPC_CHANNELS: readonly IpcChannel[] = IPC_METHODS.map(
  (method): IpcChannel => `${IPC_NAMESPACE}.${IPC_VERSION}.${method}`,
);

const CHANNEL_SET: ReadonlySet<string> = new Set(IPC_CHANNELS);

/**
 * Channels the main process pushes to the renderer.
 *
 * They are not invoked by the renderer, so they are not part of
 * `IPC_METHODS`. The preload bridge subscribes to them by name.
 */
export const IPC_EVENT_CHANNELS = {
  /** The Gateway connection status changed. */
  connectionChanged: `${IPC_NAMESPACE}.${IPC_VERSION}.connection.changed`,
  taskChanged: `${IPC_NAMESPACE}.${IPC_VERSION}.task.changed`,
} as const;

export type IpcEventChannel = (typeof IPC_EVENT_CHANNELS)[keyof typeof IPC_EVENT_CHANNELS];

/** Every event channel string. */
export const IPC_EVENT_CHANNEL_LIST: readonly IpcEventChannel[] = Object.values(IPC_EVENT_CHANNELS);

const EVENT_CHANNEL_SET: ReadonlySet<string> = new Set(IPC_EVENT_CHANNEL_LIST);

/** Narrow an arbitrary channel to an allowed event channel. */
export function isAllowedEventChannel(channel: string): channel is IpcEventChannel {
  return EVENT_CHANNEL_SET.has(channel);
}

/** Narrow an arbitrary string to an allowed channel. */
export function isAllowedChannel(channel: string): channel is IpcChannel {
  return CHANNEL_SET.has(channel);
}

/** Map a method to its channel string. */
export function channelFor(method: IpcMethod): IpcChannel {
  return `${IPC_NAMESPACE}.${IPC_VERSION}.${method}`;
}
