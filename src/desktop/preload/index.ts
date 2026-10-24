/**
 * Preload bridge.
 *
 * Compiled as CommonJS because a sandboxed preload script cannot be an ES
 * module. It exposes a fixed set of named methods and never hands
 * `ipcRenderer` to the renderer.
 */
import { contextBridge, ipcRenderer } from 'electron';

import { BRIDGE_NAMESPACE, createLinxboxApi } from './api.js';

const api = createLinxboxApi((channel: string, payload: unknown): Promise<unknown> =>
  ipcRenderer.invoke(channel, payload),
);

contextBridge.exposeInMainWorld(BRIDGE_NAMESPACE, api);
