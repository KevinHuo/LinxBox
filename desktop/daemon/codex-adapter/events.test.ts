import { describe, expect, it } from 'vitest';

import { extractProviderErrorMessage, mapNotification, resolveProviderOutcome } from './events.js';

describe('resolveProviderOutcome', () => {
  it('keeps a proven success', () => {
    expect(resolveProviderOutcome('completed')).toBe('completed');
  });

  it('keeps a proven failure', () => {
    expect(resolveProviderOutcome('failed')).toBe('failed');
  });

  it('keeps an explicit interruption', () => {
    expect(resolveProviderOutcome('interrupted')).toBe('interrupted');
  });

  it('turns an unknown status into interrupted', () => {
    expect(resolveProviderOutcome('inProgress')).toBe('interrupted');
    expect(resolveProviderOutcome('')).toBe('interrupted');
    expect(resolveProviderOutcome(undefined)).toBe('interrupted');
    expect(resolveProviderOutcome(null)).toBe('interrupted');
  });
});

describe('extractProviderErrorMessage', () => {
  it('unwraps the nested provider payload', () => {
    const raw = '{"type":"error","status":400,"error":{"message":"model not found"}}';
    expect(extractProviderErrorMessage(raw)).toBe('model not found');
  });

  it('returns a plain message unchanged', () => {
    expect(extractProviderErrorMessage('boom')).toBe('boom');
  });

  it('returns null for empty input', () => {
    expect(extractProviderErrorMessage('')).toBeNull();
    expect(extractProviderErrorMessage('   ')).toBeNull();
    expect(extractProviderErrorMessage(undefined)).toBeNull();
    expect(extractProviderErrorMessage(42)).toBeNull();
  });

  it('truncates a long message', () => {
    const result = extractProviderErrorMessage('a'.repeat(1000));
    expect(result?.length).toBeLessThanOrEqual(401);
  });

  it('unwraps a flat provider error string', () => {
    expect(extractProviderErrorMessage('{"error":"flat failure"}')).toBe('flat failure');
  });

  it('keeps the payload when a nested error message is not text', () => {
    expect(extractProviderErrorMessage('{"error":{"message":42}}')).toContain('"message":42');
    expect(extractProviderErrorMessage('{"error":42}')).toContain('"error":42');
  });

  it('returns null when the unwrapped provider error is only whitespace', () => {
    expect(extractProviderErrorMessage('{"error":"   "}')).toBeNull();
  });
});

describe('mapNotification', () => {
  it('maps turn/started', () => {
    const event = mapNotification('turn/started', {
      threadId: 'th_1',
      turn: { id: 'tn_1', status: 'inProgress' },
    });
    expect(event).toEqual({ type: 'turn-started', threadId: 'th_1', turnId: 'tn_1' });
  });

  it('ignores turn/started without a turn id', () => {
    expect(mapNotification('turn/started', { turn: {} })).toBeNull();
  });

  it('maps a commentary agent message to progress', () => {
    const event = mapNotification('item/completed', {
      threadId: 'th_1',
      turnId: 'tn_1',
      item: { type: 'agentMessage', phase: 'commentary', text: '正在看代码' },
    });
    expect(event?.type).toBe('progress');
  });

  it('maps a final answer agent message to final', () => {
    const event = mapNotification('item/completed', {
      item: { type: 'agentMessage', phase: 'final_answer', text: '答案是 42' },
    });
    expect(event?.type).toBe('final');
    if (event?.type === 'final') {
      expect(event.text).toBe('答案是 42');
    }
  });

  it('maps an agent message without a phase to final', () => {
    expect(
      mapNotification('item/completed', { item: { type: 'agentMessage', text: 'x' } })?.type,
    ).toBe('final');
  });

  it('ignores an agent message with an unknown phase', () => {
    expect(
      mapNotification('item/completed', {
        item: { type: 'agentMessage', phase: 'thinking', text: 'x' },
      }),
    ).toBeNull();
  });

  it('ignores a non agent message item', () => {
    expect(
      mapNotification('item/completed', { item: { type: 'commandExecution', text: 'ls' } }),
    ).toBeNull();
  });

  it('ignores an empty agent message', () => {
    expect(
      mapNotification('item/completed', { item: { type: 'agentMessage', text: '' } }),
    ).toBeNull();
  });

  it('maps turn/completed to a terminal event', () => {
    const event = mapNotification('turn/completed', {
      threadId: 'th_1',
      turnId: 'tn_1',
      turn: { id: 'tn_1', status: 'completed' },
    });
    expect(event?.type).toBe('terminal');
    if (event?.type === 'terminal') {
      expect(event.outcome).toBe('completed');
    }
  });

  it('maps an unprovable turn completion to interrupted', () => {
    const event = mapNotification('turn/completed', { turn: { id: 'tn_1', status: 'weird' } });
    if (event?.type === 'terminal') {
      expect(event.outcome).toBe('interrupted');
    }
  });

  it('extracts the error message of a failed turn', () => {
    const event = mapNotification('turn/completed', {
      turn: { id: 'tn_1', status: 'failed', error: { message: 'boom' } },
    });
    if (event?.type === 'terminal') {
      expect(event.error).toBe('boom');
    }
  });

  it('maps the error notification', () => {
    const event = mapNotification('error', {
      threadId: 'th_1',
      error: { message: 'usage limit exceeded' },
    });
    expect(event?.type).toBe('provider-error');
    if (event?.type === 'provider-error') {
      expect(event.message).toBe('usage limit exceeded');
    }
  });

  it('maps the warning notification', () => {
    const event = mapNotification('warning', { message: 'config ignored' });
    expect(event?.type).toBe('warning');
  });

  it('maps the account notification', () => {
    const event = mapNotification('account/updated', { authMode: 'chatgpt', planType: 'plus' });
    expect(event?.type).toBe('account');
    if (event?.type === 'account') {
      expect(event.authMode).toBe('chatgpt');
      expect(event.planType).toBe('plus');
    }
  });

  it('ignores an unknown notification', () => {
    expect(mapNotification('mcpServer/startupStatus/updated', { name: 'x' })).toBeNull();
  });
});

describe('mapNotification edge cases', () => {
  it('converges a turn/completed without a turn to interrupted', () => {
    const event = mapNotification('turn/completed', {});
    expect(event?.type).toBe('terminal');
    if (event?.type === 'terminal') {
      expect(event.outcome).toBe('interrupted');
    }
  });

  it('ignores a turn/completed whose turn has no id', () => {
    expect(mapNotification('turn/completed', { turn: { status: 'completed' } })).toBeNull();
  });

  it('ignores a malformed item/completed', () => {
    expect(mapNotification('item/completed', 'not-an-object')).toBeNull();
  });

  it('falls back to a generic message when the provider error has no text', () => {
    const event = mapNotification('error', {});
    if (event?.type === 'provider-error') {
      expect(event.message).toContain('未知错误');
    }
  });

  it('ignores a warning without a message', () => {
    expect(mapNotification('warning', {})).toBeNull();
  });

  it('ignores an account notification without fields', () => {
    const event = mapNotification('account/updated', {});
    expect(event?.type).toBe('account');
    if (event?.type === 'account') {
      expect(event.authMode).toBeNull();
      expect(event.planType).toBeNull();
    }
  });

  it('ignores a malformed account notification', () => {
    expect(mapNotification('account/updated', 'nope')).toBeNull();
  });

  it('ignores a turn/started without a thread', () => {
    const event = mapNotification('turn/started', { turn: { id: 'tn_1' } });
    expect(event).toEqual({ type: 'turn-started', threadId: '', turnId: 'tn_1' });
  });
});
