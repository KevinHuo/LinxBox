import { describe, expect, it, vi } from 'vitest';

import { GatewayClient, buildConnectUrl, type SocketLike } from './client.js';
import type { LogRecord } from '../../shared/logger.js';
import { createLogger } from '../../shared/logger.js';

class FakeSocket implements SocketLike {
  readyState = 1;
  readonly sent: string[] = [];
  private readonly listeners = new Map<string, ((event: never) => void)[]>();

  addEventListener(
    type: 'open' | 'message' | 'close' | 'error',
    listener: (event: never) => void,
  ): void {
    const existing = this.listeners.get(type) ?? [];
    existing.push(listener);
    this.listeners.set(type, existing);
  }

  send(data: string): void {
    this.sent.push(data);
  }

  close(): void {
    this.readyState = 3;
    this.fire('close', { code: 1000 });
  }

  fire(type: string, event: unknown): void {
    for (const listener of this.listeners.get(type) ?? []) {
      (listener as (value: unknown) => void)(event);
    }
  }

  open(): void {
    this.fire('open', undefined);
  }

  receive(message: unknown): void {
    this.fire('message', { data: JSON.stringify(message) });
  }

  sentMessages(): Record<string, unknown>[] {
    return this.sent.map((line) => JSON.parse(line) as Record<string, unknown>);
  }
}

function createClient(options: { heartbeatMs?: number } = {}) {
  const socket = new FakeSocket();
  const connected: { url: string; protocols: string[] }[] = [];
  const client = new GatewayClient({
    url: 'ws://127.0.0.1:8787',
    token: 'demo-token',
    desktopVersion: '0.1.0',
    capabilities: ['project_registry'],
    heartbeatMs: options.heartbeatMs ?? 50,
    socketFactory: (url, protocols) => {
      connected.push({ url, protocols });
      return socket;
    },
  });
  return { client, socket, connected };
}

const dispatchFrame = {
  protocol_version: '1.0',
  message_id: 'msg_1',
  sequence: 1043,
  type: 'task.dispatch',
  sent_at: '2026-10-24T01:00:00Z',
  payload: {
    task_id: 'tsk_1',
    project_id: 'prj_1',
    agent: 'codex',
    capabilities: ['read'],
    prompt: 'x',
    acceptance_criteria: [],
    policy_version: '2026-10-24',
    continuation: null,
  },
};

describe('buildConnectUrl', () => {
  it('appends the connect path to a bare base url', () => {
    expect(buildConnectUrl('wss://gw.example')).toBe('wss://gw.example/v1/clients/connect');
  });

  it('keeps an explicit path', () => {
    expect(buildConnectUrl('wss://gw.example/custom')).toBe('wss://gw.example/custom');
  });
});

describe('GatewayClient', () => {
  it('logs malformed inbound data by size without logging its content', () => {
    const records: LogRecord[] = [];
    const socket = new FakeSocket();
    const client = new GatewayClient({
      url: 'ws://127.0.0.1:8787',
      token: 'demo-token',
      desktopVersion: '0.1.0',
      capabilities: [],
      socketFactory: () => socket,
      logger: createLogger('test.gateway', { sink: (record) => records.push(record) }),
    });

    client.connect();
    socket.open();
    socket.fire('message', { data: 'private malformed content' });
    client.close();

    const warning = records.find((record) => record.message === 'WSS 收到非法 JSON');
    expect(warning?.fields).toMatchObject({ direction: 'gateway->desktop', bytes: 25 });
    expect(JSON.stringify(records)).not.toContain('private malformed content');
  });

  it('uses the platform WebSocket implementation when no factory is supplied', () => {
    const connected: { url: string; protocols: string[] }[] = [];
    class StubWebSocket extends FakeSocket {
      constructor(url: string, protocols: string[]) {
        super();
        connected.push({ url, protocols });
      }
    }
    vi.stubGlobal('WebSocket', StubWebSocket);
    try {
      const client = new GatewayClient({
        url: 'wss://gateway.example',
        token: 'demo-token',
        desktopVersion: '0.1.0',
        capabilities: [],
      });
      client.connect();
      expect(connected).toEqual([
        {
          url: 'wss://gateway.example/v1/clients/connect',
          protocols: ['linxbox.v1', 'linxbox-token.demo-token'],
        },
      ]);
      client.close();
    } finally {
      vi.unstubAllGlobals();
    }
  });

  it('connects to the connect path and carries the token as a sub-protocol', () => {
    const { client, connected } = createClient();
    client.connect();
    expect(connected[0]?.url).toBe('ws://127.0.0.1:8787/v1/clients/connect');
    expect(connected[0]?.protocols).toContain('linxbox.v1');
    expect(connected[0]?.protocols).toContain('linxbox-token.demo-token');
    client.close();
  });

  it('sends hello when the socket opens', () => {
    const { client, socket } = createClient();
    client.connect();
    socket.open();

    const hello = socket.sentMessages()[0];
    expect(hello?.['type']).toBe('client.hello');
    const payload = hello?.['payload'] as Record<string, unknown>;
    expect(payload['protocol_versions']).toEqual(['1.0']);
    expect(payload['desktop_version']).toBe('0.1.0');
    client.close();
  });

  it('emits a parsed inbound frame', () => {
    const { client, socket } = createClient();
    const frames: unknown[] = [];
    client.on((event) => {
      if (event.type === 'frame') {
        frames.push(event.frame);
      }
    });
    client.connect();
    socket.open();
    socket.receive(dispatchFrame);

    expect(frames).toHaveLength(1);
    client.close();
  });

  it('rejects a frame it cannot trust', () => {
    const { client, socket } = createClient();
    const errors: string[] = [];
    client.on((event) => {
      if (event.type === 'rejected') {
        errors.push(event.error.message);
      }
    });
    client.connect();
    socket.open();
    socket.receive({
      protocol_version: '9.9',
      message_id: 'm',
      sequence: 1,
      type: 'task.dispatch',
      sent_at: 'x',
      payload: {},
    });

    expect(errors.length).toBeGreaterThan(0);
    client.close();
  });

  it('rejects a frame that is not json', () => {
    const { client, socket } = createClient();
    const errors: string[] = [];
    client.on((event) => {
      if (event.type === 'rejected') {
        errors.push(event.error.message);
      }
    });
    client.connect();
    socket.open();
    socket.fire('message', { data: 'not json' });

    expect(errors).toContain('Gateway 帧不是合法 JSON');
    client.close();
  });

  it('accepts a non-string WebSocket payload that stringifies to JSON', () => {
    const { client, socket } = createClient();
    const frames: unknown[] = [];
    client.on((event) => {
      if (event.type === 'frame') frames.push(event.frame);
    });
    client.connect();
    socket.open();
    socket.fire('message', { data: { toString: () => JSON.stringify(dispatchFrame) } });

    expect(frames).toHaveLength(1);
    client.close();
  });

  it('tracks the last acknowledged sequence', () => {
    const { client, socket } = createClient();
    client.connect();
    socket.open();
    client.ack({ messageId: 'msg_1', sequence: 1043, taskId: 'tsk_1', disposition: 'accepted' });

    expect(client.lastAckedSequence).toBe(1043);
    const ack = socket.sentMessages().find((message) => message['type'] === 'task.ack');
    expect(ack?.['payload']).toMatchObject({ disposition: 'accepted', task_id: 'tsk_1' });
    client.close();
  });

  it('includes an acknowledgement error and never moves the cursor backwards', () => {
    const { client, socket } = createClient();
    client.connect();
    socket.open();
    client.ack({ messageId: 'msg_2', sequence: 9, taskId: 'tsk_2', disposition: 'accepted' });
    client.ack({
      messageId: 'msg_1',
      sequence: 8,
      taskId: 'tsk_1',
      disposition: 'rejected',
      error: {
        code: 'DEPENDENCY_UNAVAILABLE',
        message: '不可用',
        retryable: false,
        request_id: 'msg_1',
      },
    });

    expect(client.lastAckedSequence).toBe(9);
    const ack = socket
      .sentMessages()
      .find((message) => (message['payload'] as { message_id?: string }).message_id === 'msg_1');
    expect(ack?.['payload']).toMatchObject({ error: { code: 'DEPENDENCY_UNAVAILABLE' } });
    client.close();
  });

  it('sends a heartbeat on a timer', async () => {
    const { client, socket } = createClient({ heartbeatMs: 20 });
    client.connect();
    socket.open();
    await new Promise((resolve) => setTimeout(resolve, 60));

    const beats = socket.sentMessages().filter((message) => message['type'] === 'client.heartbeat');
    expect(beats.length).toBeGreaterThan(0);
    client.close();
  });

  it('stops sending after close', () => {
    const { client, socket } = createClient({ heartbeatMs: 20 });
    client.connect();
    socket.open();
    const before = socket.sent.length;
    client.close();
    expect(socket.sent.length).toBe(before);
  });

  it('does not reconnect after the client has been closed', () => {
    const { client, connected } = createClient();
    client.close();
    client.connect();
    expect(connected).toHaveLength(0);
  });
});

describe('GatewayClient reconnection', () => {
  it('reconnects after the socket closes', async () => {
    const sockets: FakeSocket[] = [];
    const client = new GatewayClient({
      url: 'ws://127.0.0.1:8787',
      token: 'demo-token',
      desktopVersion: '0.1.0',
      capabilities: [],
      reconnectBaseMs: 5,
      reconnectMaxMs: 10,
      socketFactory: () => {
        const socket = new FakeSocket();
        sockets.push(socket);
        return socket;
      },
    });

    client.connect();
    sockets[0]?.open();
    sockets[0]?.close();

    await new Promise((resolve) => setTimeout(resolve, 60));
    expect(sockets.length).toBeGreaterThan(1);
    client.close();
  });

  it('uses default reconnect timing when no override is supplied', () => {
    const { client, socket } = createClient();
    client.connect();
    socket.open();
    socket.close();
    client.close();
    expect(socket.readyState).toBe(3);
  });

  it('reports a socket that cannot be created', () => {
    const errors: string[] = [];
    const client = new GatewayClient({
      url: 'ws://127.0.0.1:8787',
      token: 'demo-token',
      desktopVersion: '0.1.0',
      capabilities: [],
      reconnectBaseMs: 1000,
      socketFactory: () => {
        throw new Error('refused');
      },
    });
    client.on((event) => {
      if (event.type === 'rejected') {
        errors.push(event.error.message);
      }
    });

    client.connect();
    expect(errors).toContain('refused');
    client.close();
  });

  it('does not send anything when the socket is not open', () => {
    const { client, socket } = createClient();
    client.connect();
    socket.readyState = 0;
    client.ack({ messageId: 'm', sequence: 1, taskId: 't', disposition: 'accepted' });
    expect(socket.sent).toHaveLength(0);
    client.close();
  });
});

describe('GatewayClient listener lifecycle', () => {
  it('stops delivering events after unsubscribe', () => {
    const { client, socket } = createClient();
    const seen: string[] = [];
    const unsubscribe = client.on((event) => {
      if (event.type === 'frame') {
        seen.push('frame');
      }
    });
    client.connect();
    socket.open();
    socket.receive(dispatchFrame);
    unsubscribe();
    socket.receive(dispatchFrame);

    expect(seen).toHaveLength(1);
    client.close();
  });

  it('reports a socket error', () => {
    const { client, socket } = createClient();
    const errors: string[] = [];
    client.on((event) => {
      if (event.type === 'rejected') {
        errors.push(event.error.message);
      }
    });
    client.connect();
    socket.open();
    socket.fire('error', new Error('boom'));

    expect(errors).toContain('Gateway 连接异常');
    client.close();
  });
});
