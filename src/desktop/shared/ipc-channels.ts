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
  'codex.sessions.list',
  'codex.sessions.read',
  'codex.turn.status',
  'connection.read',
  'connection.save',
  'connection.test',
  'diagnostics.read',
  'projects.grant',
  'projects.list',
  'projects.revoke',
  'tasks.cancel',
  'tasks.get',
  'tasks.list',
] as const;

export type IpcMethod = (typeof IPC_METHODS)[number];

export type IpcChannel = `${typeof IPC_NAMESPACE}.${typeof IPC_VERSION}.${IpcMethod}`;

/** Every channel string the preload bridge is allowed to use. */
export const IPC_CHANNELS: readonly IpcChannel[] = IPC_METHODS.map(
  (method): IpcChannel => `${IPC_NAMESPACE}.${IPC_VERSION}.${method}`,
);

const CHANNEL_SET: ReadonlySet<string> = new Set(IPC_CHANNELS);

/** Narrow an arbitrary string to an allowed channel. */
export function isAllowedChannel(channel: string): channel is IpcChannel {
  return CHANNEL_SET.has(channel);
}

/** Map a method to its channel string. */
export function channelFor(method: IpcMethod): IpcChannel {
  return `${IPC_NAMESPACE}.${IPC_VERSION}.${method}`;
}
