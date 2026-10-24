import { describe, expect, it } from 'vitest';

import {
  isProtocolVersionCompatible,
  isSupportedProtocolVersion,
  parseInboundFrame,
} from './envelope.js';

function dispatchPayload(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    task_id: 'tsk_01H',
    project_id: 'prj_01H',
    agent: 'codex',
    capabilities: ['read', 'write', 'test'],
    prompt: '修复登录页错误并运行测试',
    acceptance_criteria: ['空值提交不再抛错'],
    policy_version: '2026-10-24',
    continuation: null,
    ...overrides,
  };
}

function dispatchFrame(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    protocol_version: '1.0',
    message_id: 'msg_01H',
    sequence: 1043,
    type: 'task.dispatch',
    sent_at: '2026-10-24T01:00:00Z',
    expires_at: '2026-10-24T01:30:00Z',
    payload: dispatchPayload(),
    ...overrides,
  };
}

describe('parseInboundFrame', () => {
  it('接受合法的 task.dispatch', () => {
    const result = parseInboundFrame(dispatchFrame());
    expect(result.ok).toBe(true);
    if (!result.ok) {
      throw new Error('expected success');
    }
    expect(result.frame.type).toBe('task.dispatch');
    expect(result.frame.sequence).toBe(1043);
    expect(result.frame.expires_at).toBe('2026-10-24T01:30:00Z');
  });

  it('没有 expires_at 时该字段整体缺省', () => {
    const frame = dispatchFrame();
    delete frame['expires_at'];
    const result = parseInboundFrame(frame);
    expect(result.ok).toBe(true);
    if (!result.ok) {
      throw new Error('expected success');
    }
    expect('expires_at' in result.frame).toBe(false);
  });

  it('接受 task.cancel', () => {
    const result = parseInboundFrame({
      protocol_version: '1.0',
      message_id: 'msg_02H',
      sequence: 1044,
      type: 'task.cancel',
      sent_at: '2026-10-24T01:00:00Z',
      payload: { task_id: 'tsk_01H', reason: 'user_requested' },
    });
    expect(result.ok).toBe(true);
  });

  it('接受 project.upsert', () => {
    const result = parseInboundFrame({
      protocol_version: '1.0',
      message_id: 'msg_03H',
      sequence: 1,
      type: 'project.upsert',
      sent_at: '2026-10-24T01:00:00Z',
      payload: {
        project_id: 'prj_01H',
        display_name: '商城前端',
        capabilities: ['read', 'write', 'test'],
        availability: 'online',
        grant_version: 3,
      },
    });
    expect(result.ok).toBe(true);
  });

  it('接受 project.revoke', () => {
    const result = parseInboundFrame({
      protocol_version: '1.0',
      message_id: 'msg_04H',
      sequence: 2,
      type: 'project.revoke',
      sent_at: '2026-10-24T01:00:00Z',
      payload: {
        project_id: 'prj_01H',
        grant_version: 4,
        revoked_at: '2026-10-24T01:00:00Z',
      },
    });
    expect(result.ok).toBe(true);
  });

  it('未知消息类型被拒绝而不是忽略', () => {
    const result = parseInboundFrame(dispatchFrame({ type: 'task.exfiltrate' }));
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.code).toBe('INVALID_ARGUMENT');
    expect(result.error.details?.['reason']).toBe('unknown_message_type');
    expect(result.error.request_id).toBe('msg_01H');
  });

  it('未知顶层字段被拒绝', () => {
    const result = parseInboundFrame(dispatchFrame({ extra_field: 'nope' }));
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.details?.['reason']).toBe('unknown_field');
  });

  it('未知 payload 字段被拒绝', () => {
    const result = parseInboundFrame(
      dispatchFrame({ payload: dispatchPayload({ root_path: '/Users/dev' }) }),
    );
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.details?.['reason']).toBe('unknown_field');
  });

  it('major 不同的协议版本被拒绝', () => {
    const result = parseInboundFrame(dispatchFrame({ protocol_version: '2.0' }));
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.code).toBe('INVALID_ARGUMENT');
    expect(result.error.details?.['reason']).toBe('protocol_version_incompatible');
  });

  it('同 major 的其它版本同样不被本构建精确支持', () => {
    const result = parseInboundFrame(dispatchFrame({ protocol_version: '1.7' }));
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.details?.['reason']).toBe('protocol_version_incompatible');
  });

  it('缺少 message_id 被拒绝', () => {
    const frame = dispatchFrame();
    delete frame['message_id'];
    const result = parseInboundFrame(frame);
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.details?.['reason']).toBe('malformed_frame');
    expect(result.error.request_id).toBe('unknown');
  });

  it('负数 sequence 被拒绝', () => {
    const result = parseInboundFrame(dispatchFrame({ sequence: -1 }));
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.details?.['reason']).toBe('malformed_frame');
  });

  it('非整数 sequence 被拒绝', () => {
    const result = parseInboundFrame(dispatchFrame({ sequence: 1.5 }));
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.details?.['reason']).toBe('malformed_frame');
  });

  it('格式非法的 sent_at 被拒绝', () => {
    const result = parseInboundFrame(dispatchFrame({ sent_at: 'yesterday' }));
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.details?.['reason']).toBe('malformed_frame');
  });

  it('payload 缺少必填字段被拒绝', () => {
    const payload = dispatchPayload();
    delete payload['project_id'];
    const result = parseInboundFrame(dispatchFrame({ payload }));
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.details?.['reason']).toBe('malformed_frame');
  });

  it('payload 里的未知能力被拒绝', () => {
    const result = parseInboundFrame(
      dispatchFrame({ payload: dispatchPayload({ capabilities: ['deploy'] }) }),
    );
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.details?.['reason']).toBe('malformed_frame');
  });

  it('非对象输入被拒绝', () => {
    const result = parseInboundFrame('not a frame');
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.error.code).toBe('INVALID_ARGUMENT');
  });
});

describe('isSupportedProtocolVersion', () => {
  it('精确匹配本构建支持的版本', () => {
    expect(isSupportedProtocolVersion('1.0')).toBe(true);
    expect(isSupportedProtocolVersion('1.7')).toBe(false);
    expect(isSupportedProtocolVersion('2.0')).toBe(false);
  });
});

describe('isProtocolVersionCompatible', () => {
  it('同 major 视为兼容', () => {
    expect(isProtocolVersionCompatible('1.0')).toBe(true);
    expect(isProtocolVersionCompatible('1.7')).toBe(true);
  });

  it('不同 major 不兼容', () => {
    expect(isProtocolVersionCompatible('2.0')).toBe(false);
    expect(isProtocolVersionCompatible('0.9')).toBe(false);
  });

  it('格式非法的版本不兼容', () => {
    expect(isProtocolVersionCompatible('1')).toBe(false);
    expect(isProtocolVersionCompatible('v1.0')).toBe(false);
    expect(isProtocolVersionCompatible('')).toBe(false);
  });
});
