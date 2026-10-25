/**
 * Preload bridge.
 *
 * Compiled as CommonJS because a sandboxed preload script cannot be an ES
 * module. It exposes a fixed set of named methods and never hands
 * `ipcRenderer` to the renderer.
 */
import { contextBridge, ipcRenderer } from 'electron';

import { BRIDGE_NAMESPACE, createLinxboxApi } from './api.js';
import { IPC_EVENT_CHANNEL_LIST } from '../shared/ipc-channels.js';

/** The event channels the renderer may listen to. */
const EVENT_CHANNELS: ReadonlySet<string> = new Set(IPC_EVENT_CHANNEL_LIST);

const api = createLinxboxApi(
  (channel: string, payload: unknown): Promise<unknown> => ipcRenderer.invoke(channel, payload),
  (channel: string, listener: (payload: unknown) => void): (() => void) => {
    if (!EVENT_CHANNELS.has(channel)) {
      return () => {};
    }
    const handler = (_event: unknown, payload: unknown): void => listener(payload);
    ipcRenderer.on(channel, handler);
    return () => {
      ipcRenderer.removeListener(channel, handler);
    };
  },
);

contextBridge.exposeInMainWorld(BRIDGE_NAMESPACE, api);
