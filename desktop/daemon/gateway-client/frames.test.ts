import { describe, expect, it } from 'vitest';

import {
  buildAgentSessionsSyncFrame,
  buildAckFrame,
  buildDeviceBindFrame,
  buildHelloFrame,
  buildHeartbeatFrame,
  buildTaskEventFrame,
  parseInboundFrame,
  publicError,
} from './frames.js';

const dispatchFrame = {
  protocol_version: '1.0',
  message_id: 'msg_1',
  sequence: 1043,
  type: 'task.dispatch',
  sent_at: '2026-10-24T01:00:00Z',
  expires_at: '2026-10-24T01:30:00Z',
  payload: {
    task_id: 'tsk_1',
    project_id: 'prj_1',
    agent: 'codex',
    capabilities: ['read', 'write', 'test'],
    prompt: '修复登录页错误',
    acceptance_criteria: ['空值提交不再抛错'],
    policy_version: '2026-10-24',
    continuation: null,
  },
};

describe('parseInboundFrame', () => {
  it('accepts a contract-compatible binding result and rejects unknown error codes', () => {
    const valid = parseInboundFrame({
      ...dispatchFrame,
      type: 'binding.result',
      payload: {
        reply_to_message_id: 'msg_bind',
        ok: true,
        data: { device_id: 'AA:BB:CC:DD:EE:FF', desktop_client_id: 'desktop-1' },
      },
    });
    expect(valid.ok).toBe(true);
    const invalid = parseInboundFrame({
      ...dispatchFrame,
      type: 'binding.result',
      payload: { reply_to_message_id: 'msg_bind', ok: false, error: { code: 'NOPE', message: 'x' } },
    });
    expect(invalid.ok).toBe(false);
  });
  it('accepts a valid task.dispatch', () => {
    const result = parseInboundFrame(dispatchFrame);
    expect(result.ok).toBe(true);
    if (result.ok && result.frame.type === 'task.dispatch') {
      expect(result.frame.payload.task_id).toBe('tsk_1');
      expect(result.frame.payload.project_id).toBe('prj_1');
    }
  });

  it('accepts a task.cancel', () => {
    const result = parseInboundFrame({
      ...dispatchFrame,
      type: 'task.cancel',
      payload: { task_id: 'tsk_1', reason: 'user_requested' },
    });
    expect(result.ok).toBe(true);
  });

  it('accepts contract-defined project and history request frames', () => {
    expect(parseInboundFrame({
      ...dispatchFrame,
      type: 'project.upsert',
      payload: {
        project_id: 'prj_1', display_name: 'LinxBox', capabilities: ['read'],
        availability: 'online', grant_version: 1,
      },
    }).ok).toBe(true);
    expect(parseInboundFrame({
      ...dispatchFrame,
      type: 'messages.list',
      payload: { agent_session_id: 'ses_1' },
    }).ok).toBe(true);
    expect(parseInboundFrame({
      ...dispatchFrame,
      type: 'message.content.get',
      payload: { agent_session_id: 'ses_1', message_id: 'msg_1', offset: 0 },
    }).ok).toBe(true);
  });

  it('accepts a device-selected existing Codex session continuation', () => {
    const result = parseInboundFrame({
      ...dispatchFrame,
      payload: {
        ...dispatchFrame.payload,
        continuation: { agent_session_id: 'codex_th_1', thread_id: 'th_1' },
      },
    });
    expect(result.ok).toBe(true);
  });

  it('accepts an agent session sync result', () => {
    const result = parseInboundFrame({
      ...dispatchFrame,
      type: 'agent_sessions.sync.result',
      payload: { reply_to_message_id: 'sync_1', ok: true },
    });
    expect(result.ok).toBe(true);
  });

  it('rejects the legacy PublicError shape in a session sync result', () => {
    const result = parseInboundFrame({
      ...dispatchFrame,
      type: 'agent_sessions.sync.result',
      payload: {
        reply_to_message_id: 'sync_1',
        ok: false,
        error: {
          code: 'INVALID_ARGUMENT',
          message: '目录非法',
          retryable: false,
          request_id: 'sync_1',
        },
      },
    });
    expect(result.ok).toBe(false);
  });

  it('rejects a frame that is not an object', () => {
    expect(parseInboundFrame('nope').ok).toBe(false);
    expect(parseInboundFrame(null).ok).toBe(false);
    expect(parseInboundFrame([1, 2]).ok).toBe(false);
  });

  it('rejects a frame with a missing envelope field', () => {
    const { sent_at: _sentAt, ...withoutSentAt } = dispatchFrame;
    expect(parseInboundFrame(withoutSentAt).ok).toBe(false);
  });

  it('rejects an incompatible protocol version', () => {
    const result = parseInboundFrame({ ...dispatchFrame, protocol_version: '2.0' });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.code).toBe('INTERNAL');
    }
  });

  it('rejects an unknown message type instead of ignoring it', () => {
    const result = parseInboundFrame({ ...dispatchFrame, type: 'task.launch_missiles' });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.message).toContain('task.launch_missiles');
    }
  });

  it('rejects a dispatch payload with an unknown field', () => {
    const result = parseInboundFrame({
      ...dispatchFrame,
      payload: { ...dispatchFrame.payload, root_path: '/Users/victim' },
    });
    expect(result.ok).toBe(false);
  });

  it('rejects a dispatch payload with a missing field', () => {
    const { prompt: _prompt, ...withoutPrompt } = dispatchFrame.payload;
    expect(parseInboundFrame({ ...dispatchFrame, payload: withoutPrompt }).ok).toBe(false);
  });

  it('rejects a dispatch payload with an unknown capability', () => {
    expect(
      parseInboundFrame({
        ...dispatchFrame,
        payload: { ...dispatchFrame.payload, capabilities: ['deploy'] },
      }).ok,
    ).toBe(false);
  });

  it('rejects a non-object payload', () => {
    expect(parseInboundFrame({ ...dispatchFrame, payload: 'text' }).ok).toBe(false);
  });
});

describe('outbound frame builders', () => {
  it('builds device.bind and rejects malformed MAC addresses', () => {
    expect(buildDeviceBindFrame({
      messageId: 'msg_bind', sequence: 1, macAddress: 'AA:BB:CC:DD:EE:FF', sentAt: '2026-10-24T01:00:00Z',
    }).payload.mac_address).toBe('AA:BB:CC:DD:EE:FF');
    expect(() => buildDeviceBindFrame({
      messageId: 'msg_bind', sequence: 1, macAddress: 'not-a-mac', sentAt: '2026-10-24T01:00:00Z',
    })).toThrow();
  });
  it('builds a hello frame with the negotiated protocol version', () => {
    const frame = buildHelloFrame({
      messageId: 'msg_1',
      sequence: 1,
      desktopVersion: '0.1.0',
      lastAckedSequence: 42,
      capabilities: ['project_registry'],
      sentAt: '2026-10-24T01:00:00Z',
    });
    expect(frame.type).toBe('client.hello');
    expect(frame.payload.protocol_versions).toEqual(['1.0']);
    expect(frame.payload.last_acked_sequence).toBe(42);
  });

  it('builds an ack frame carrying the disposition', () => {
    const frame = buildAckFrame({
      messageId: 'msg_1',
      sequence: 7,
      taskId: 'tsk_1',
      disposition: 'duplicate',
      sentAt: '2026-10-24T01:00:00Z',
    });
    expect(frame.type).toBe('task.ack');
    expect(frame.payload.disposition).toBe('duplicate');
    expect(frame.payload.message_id).toBe('msg_1');
  });

  it('omits the error field when there is none', () => {
    const frame = buildAckFrame({
      messageId: 'msg_1',
      sequence: 7,
      taskId: 'tsk_1',
      disposition: 'accepted',
      sentAt: '2026-10-24T01:00:00Z',
    });
    expect('error' in frame.payload).toBe(false);
  });

  it('includes a public error when an acknowledgement is rejected', () => {
    const error = publicError('DEPENDENCY_UNAVAILABLE', '本机服务不可用', 'msg_1');
    const frame = buildAckFrame({
      messageId: 'msg_1',
      sequence: 7,
      taskId: 'tsk_1',
      disposition: 'rejected',
      sentAt: '2026-10-24T01:00:00Z',
      error,
    });
    expect(frame.payload.error).toEqual(error);
  });

  it('builds a heartbeat frame without task truth', () => {
    const frame = buildHeartbeatFrame({
      messageId: 'msg_2',
      sequence: 2,
      clientTime: '2026-10-24T01:00:00Z',
    });
    expect(frame.type).toBe('client.heartbeat');
    expect(frame.payload.client_time).toBe('2026-10-24T01:00:00Z');
  });

  it('includes an explicitly supplied heartbeat summary', () => {
    const frame = buildHeartbeatFrame({
      messageId: 'msg_2',
      sequence: 2,
      clientTime: '2026-10-24T01:00:00Z',
      summary: 'idle',
    });
    expect(frame.payload.summary).toBe('idle');
  });

  it('builds a task event frame with a monotonic event sequence', () => {
    const frame = buildTaskEventFrame({
      messageId: 'msg_3',
      sequence: 3,
      type: 'task.completed',
      payload: {
        task_id: 'tsk_1',
        event_id: 'evt_1',
        event_sequence: 1,
        occurred_at: '2026-10-24T01:00:00Z',
        state: 'succeeded',
        stage: 'finished',
        summary: '完成',
        result: null,
        error: null,
        thread_id: 'th_1',
        turn_id: 'tn_1',
      },
      sentAt: '2026-10-24T01:00:00Z',
    });
    expect(frame.type).toBe('task.completed');
    expect(frame.payload.event_sequence).toBe(1);
  });

  it('builds a full Codex session directory sync without local paths', () => {
    const frame = buildAgentSessionsSyncFrame({
      messageId: 'sync_1',
      sequence: 4,
      sentAt: '2026-10-25T00:00:00Z',
      items: [
        {
          agent_session_id: 'codex_th_1',
          thread_id: 'th_1',
          project_id: 'prj_1',
          project_name: 'LinxBox',
          agent: 'codex',
          title: '修复问题',
          status: 'idle',
          summary: '修复问题',
        },
      ],
    });
    expect(frame.type).toBe('agent_sessions.sync');
    expect(frame.payload.items[0]?.thread_id).toBe('th_1');
    expect(JSON.stringify(frame)).not.toContain('/Users/');
  });
});

describe('publicError', () => {
  it('carries the contract fields', () => {
    const error = publicError('POLICY_DENIED', '项目未授权', 'req_1');
    expect(error.code).toBe('POLICY_DENIED');
    expect(error.retryable).toBe(false);
    expect(error.request_id).toBe('req_1');
  });

  it('attaches details only when given', () => {
    expect('details' in publicError('INVALID_ARGUMENT', 'x', 'r')).toBe(false);
    expect(publicError('INVALID_ARGUMENT', 'x', 'r', { reason: 'y' }).details).toEqual({
      reason: 'y',
    });
  });
});

describe('parseInboundFrame cancel payloads', () => {
  it('rejects a cancel payload with an unknown field', () => {
    const result = parseInboundFrame({
      protocol_version: '1.0',
      message_id: 'msg_1',
      sequence: 1,
      type: 'task.cancel',
      sent_at: '2026-10-24T01:00:00Z',
      payload: { task_id: 'tsk_1', reason: 'x', force: true },
    });
    expect(result.ok).toBe(false);
  });

  it('rejects a cancel payload without a reason', () => {
    const result = parseInboundFrame({
      protocol_version: '1.0',
      message_id: 'msg_1',
      sequence: 1,
      type: 'task.cancel',
      sent_at: '2026-10-24T01:00:00Z',
      payload: { task_id: 'tsk_1' },
    });
    expect(result.ok).toBe(false);
  });
});
