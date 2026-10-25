import { afterEach, describe, expect, it } from 'vitest';

import { WebSocket, type RawData } from 'ws';

import { startMockGatewayConsole, CONNECT_PATH, DEFAULT_TOKEN } from './mock-gateway-console.mjs';

/**
 * Decode a WebSocket message into text.
 *
 * `RawData` carries several shapes depending on the negotiated frame type,
 * so every variant is normalised to a UTF-8 string here.
 */
function decodeMessage(data: RawData): string {
  if (typeof data === 'string') {
    return data;
  }
  if (Buffer.isBuffer(data)) {
    return data.toString('utf8');
  }
  if (Array.isArray(data)) {
    return Buffer.concat(data).toString('utf8');
  }
  return Buffer.from(data).toString('utf8');
}

/** Parse a WebSocket message into a frame object. */
function parseFrame<T>(data: RawData): T {
  return JSON.parse(decodeMessage(data)) as T;
}

/** A started service that is closed after the test. */
let running: { close: () => Promise<void> } | null = null;

afterEach(async () => {
  if (running !== null) {
    await running.close();
    running = null;
  }
});

/** Start the service on an ephemeral port. */
async function start() {
  const service = await startMockGatewayConsole({ port: 0, token: DEFAULT_TOKEN });
  running = service;
  return service;
}

/** Connect a fake Desktop and wait until the server registered it. */
function connectDesktop(port: number): Promise<WebSocket> {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(`ws://127.0.0.1:${port}${CONNECT_PATH}`, [
      'linxbox.v1',
      `linxbox-token.${DEFAULT_TOKEN}`,
    ]);
    socket.once('open', () => resolve(socket));
    socket.once('error', reject);
  });
}

/** Wait until a predicate holds, or fail after the timeout. */
async function waitFor(predicate: () => boolean, timeoutMs = 3000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  throw new Error('等待超时');
}

describe('mock gateway console', () => {
  it('serves the console page', async () => {
    const service = await start();
    const response = await fetch(`http://127.0.0.1:${service.port}/`);
    expect(response.status).toBe(200);
    expect(response.headers.get('content-type')).toContain('text/html');
    const html = await response.text();
    expect(html).toContain('Mock Gateway 控制台');
  });

  it('accepts a Desktop connection and records the hello frame', async () => {
    const service = await start();
    const socket = await connectDesktop(service.port);

    socket.send(
      JSON.stringify({
        protocol_version: '1.0',
        message_id: 'msg_1',
        sequence: 1,
        type: 'client.hello',
        sent_at: new Date().toISOString(),
        payload: {
          desktop_version: '0.1.0',
          protocol_versions: ['1.0'],
          last_acked_sequence: 0,
          capabilities: ['project_registry'],
        },
      }),
    );

    await waitFor(() => service.received.length > 0);
    expect(service.received[0]?.type).toBe('client.hello');

    socket.close();
  });

  it('refuses a connection with the wrong token', async () => {
    const service = await start();
    const closed = new Promise<void>((resolve) => {
      const socket = new WebSocket(`ws://127.0.0.1:${service.port}${CONNECT_PATH}`, [
        'linxbox.v1',
        'linxbox-token.wrong',
      ]);
      socket.once('close', () => resolve());
      socket.once('error', () => resolve());
    });
    await closed;
    expect(service.received).toHaveLength(0);
  });

  it('delivers a dispatch to the connected Desktop', async () => {
    const service = await start();
    const socket = await connectDesktop(service.port);

    const received: { type: string; payload: { prompt: string } }[] = [];
    socket.on('message', (data) => {
      received.push(parseFrame<{ type: string; payload: { prompt: string } }>(data));
    });

    const response = await fetch(`http://127.0.0.1:${service.port}/api/dispatch`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        kind: 'dispatch',
        payload: { prompt: '测试提示词', project_id: 'prj_demo_0001' },
      }),
    });
    expect(response.status).toBe(200);

    await waitFor(() => received.length > 0);
    const frame = received[0] as { type: string; payload: { prompt: string } };
    expect(frame.type).toBe('task.dispatch');
    expect(frame.payload.prompt).toBe('测试提示词');

    socket.close();
  });

  it('stores the Desktop session catalogue and dispatches the selected session', async () => {
    const service = await start();
    const socket = await connectDesktop(service.port);
    const received: { type: string; payload: Record<string, unknown> }[] = [];
    socket.on('message', (data) => {
      received.push(parseFrame<{ type: string; payload: Record<string, unknown> }>(data));
    });

    socket.send(
      JSON.stringify({
        protocol_version: '1.0',
        message_id: 'sync_1',
        sequence: 2,
        type: 'agent_sessions.sync',
        sent_at: new Date().toISOString(),
        payload: {
          items: [
            {
              agent_session_id: 'codex_th_1',
              thread_id: 'th_1',
              project_id: 'prj_local_1',
              project_name: 'LinxBox',
              agent: 'codex',
              title: '已有会话',
              status: 'idle',
              summary: '已有会话',
            },
          ],
        },
      }),
    );
    await waitFor(() => service.agentSessions.length === 1);

    const response = await fetch(`http://127.0.0.1:${service.port}/api/dispatch`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        kind: 'dispatch',
        agent_session_id: 'codex_th_1',
        project_id: 'prj_local_1',
        title: '已有会话',
        payload: { prompt: '继续检查' },
      }),
    });
    expect(response.status).toBe(200);
    await waitFor(() => received.some((frame) => frame.type === 'task.dispatch'));

    const dispatch = received.find((frame) => frame.type === 'task.dispatch');
    expect(dispatch?.payload['project_id']).toBe('prj_local_1');
    expect(dispatch?.payload['continuation']).toEqual({
      agent_session_id: 'codex_th_1',
      thread_id: 'th_1',
    });
    socket.close();
  });

  it('refuses to dispatch when no Desktop is connected', async () => {
    const service = await start();
    const response = await fetch(`http://127.0.0.1:${service.port}/api/dispatch`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ kind: 'dispatch', payload: {} }),
    });
    expect(response.status).toBe(409);
  });

  it('delivers a cancel to the connected Desktop', async () => {
    const service = await start();
    const socket = await connectDesktop(service.port);

    const received: { type: string; payload: { task_id: string } }[] = [];
    socket.on('message', (data) => {
      received.push(parseFrame<{ type: string; payload: { task_id: string } }>(data));
    });

    await fetch(`http://127.0.0.1:${service.port}/api/dispatch`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ kind: 'cancel', task_id: 'tsk_1' }),
    });

    await waitFor(() => received.length > 0);
    expect(received[0]?.type).toBe('task.cancel');
    expect(received[0]?.payload.task_id).toBe('tsk_1');

    socket.close();
  });

  it.each([
    [
      'project.upsert',
      {
        project_id: 'prj_local_1',
        display_name: 'LinxBox',
        capabilities: ['read', 'write', 'test'],
        availability: 'online',
        grant_version: 2,
      },
    ],
    [
      'project.revoke',
      { project_id: 'prj_local_1', grant_version: 2, revoked_at: '2026-10-25T00:00:00.000Z' },
    ],
  ] as const)('delivers %s to the connected Desktop', async (kind, payload) => {
    const service = await start();
    const socket = await connectDesktop(service.port);
    const received: { type: string; payload: Record<string, unknown> }[] = [];
    socket.on('message', (data) => received.push(parseFrame(data)));

    const response = await fetch(`http://127.0.0.1:${service.port}/api/dispatch`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ kind, ...payload }),
    });
    expect(response.status).toBe(200);
    await waitFor(() => received.length > 0);
    expect(received[0]?.type).toBe(kind);
    expect(received[0]?.payload).toMatchObject(
      kind === 'project.revoke'
        ? { project_id: payload.project_id, grant_version: payload.grant_version }
        : payload,
    );
    expect(service.sent[0]?.type).toBe(kind);
    socket.close();
  });

  it('reports the connection count and the recorded frames as state', async () => {
    const service = await start();
    const socket = await connectDesktop(service.port);

    socket.send(
      JSON.stringify({
        protocol_version: '1.0',
        message_id: 'msg_1',
        sequence: 1,
        type: 'client.hello',
        sent_at: new Date().toISOString(),
        payload: { desktop_version: '0.1.0', protocol_versions: ['1.0'] },
      }),
    );
    await waitFor(() => service.received.length > 0);

    const response = await fetch(`http://127.0.0.1:${service.port}/api/state`);
    const state = (await response.json()) as {
      connections: number;
      received: { type: string; summary: string }[];
    };
    expect(state.connections).toBe(1);
    expect(state.received[0]?.type).toBe('client.hello');
    expect(state.received[0]?.summary.length).toBeGreaterThan(0);

    socket.close();
  });

  it('answers an unknown path with a not found', async () => {
    const service = await start();
    const response = await fetch(`http://127.0.0.1:${service.port}/nope`);
    expect(response.status).toBe(404);
  });
});
