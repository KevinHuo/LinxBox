import { describe, expect, it } from 'vitest';

import type { DeliveryFacts, DeliveryEvaluationContext } from './delivery.js';
import { canonicalJson, decideDelivery, hashPayload, toDeliveryRecord } from './delivery.js';

const BASE_FACTS: DeliveryFacts = {
  messageId: 'msg_01H',
  sequence: 1,
  type: 'task.dispatch',
  payload: { task_id: 'tsk_01H', prompt: '修复登录页错误' },
  sentAt: '2026-10-24T01:00:00Z',
};

const NOW = Date.parse('2026-10-24T01:00:00Z');

function context(overrides: Partial<DeliveryEvaluationContext> = {}): DeliveryEvaluationContext {
  return { now: NOW, ...overrides };
}

describe('canonicalJson', () => {
  it('按字典序排列对象键', () => {
    expect(canonicalJson({ b: 1, a: 2 })).toBe('{"a":2,"b":1}');
  });

  it('保持数组顺序', () => {
    expect(canonicalJson([3, 1, 2])).toBe('[3,1,2]');
  });

  it('丢弃对象中的 undefined、函数与符号', () => {
    const value = { keep: 1, skip: undefined, fn: (): number => 1, sym: Symbol('x') };
    expect(canonicalJson(value)).toBe('{"keep":1}');
  });

  it('把数组中的 undefined 变成 null', () => {
    expect(canonicalJson([undefined, 1])).toBe('[null,1]');
  });

  it('把非有限数字变成 null', () => {
    expect(canonicalJson({ n: Number.NaN, m: Number.POSITIVE_INFINITY })).toBe(
      '{"m":null,"n":null}',
    );
  });

  it('把 bigint 转成字符串', () => {
    expect(canonicalJson({ big: 10n })).toBe('{"big":"10"}');
  });

  it('顶层 undefined 归一化为 null', () => {
    expect(canonicalJson(undefined)).toBe('null');
  });

  it('顶层函数与符号归一化为 null', () => {
    expect(canonicalJson((): number => 1)).toBe('null');
    expect(canonicalJson(Symbol('x'))).toBe('null');
  });
});

describe('hashPayload', () => {
  it('与键顺序无关', () => {
    expect(hashPayload({ a: 1, b: [1, 2] })).toBe(hashPayload({ b: [1, 2], a: 1 }));
  });

  it('内容不同则哈希不同', () => {
    expect(hashPayload({ a: 1 })).not.toBe(hashPayload({ a: 2 }));
  });
});

describe('decideDelivery', () => {
  it('首次收到的合法消息被接受', () => {
    const decision = decideDelivery(BASE_FACTS, context());
    expect(decision.disposition).toBe('accepted');
  });

  it('sequence 恰好是基线加一时被接受', () => {
    const decision = decideDelivery(BASE_FACTS, context({ lastAckedSequence: 0 }));
    expect(decision.disposition).toBe('accepted');
  });

  it('相同 message_id 与相同 payload 返回 duplicate 并保留原 disposition', () => {
    const known = {
      messageId: 'msg_01H',
      payloadHash: hashPayload(BASE_FACTS.payload),
      disposition: 'accepted' as const,
    };
    const decision = decideDelivery(BASE_FACTS, context({ knownDelivery: known }));
    expect(decision.disposition).toBe('duplicate');
    if (decision.disposition !== 'duplicate') {
      throw new Error('expected duplicate');
    }
    expect(decision.original.disposition).toBe('accepted');
  });

  it('重复的已过期消息保留 expired 而不是重新评估', () => {
    const known = {
      messageId: 'msg_01H',
      payloadHash: hashPayload(BASE_FACTS.payload),
      disposition: 'expired' as const,
    };
    const decision = decideDelivery(BASE_FACTS, context({ knownDelivery: known }));
    expect(decision.disposition).toBe('duplicate');
    if (decision.disposition !== 'duplicate') {
      throw new Error('expected duplicate');
    }
    expect(decision.original.disposition).toBe('expired');
  });

  it('相同 message_id 与不同 payload 判为冲突并 fail closed', () => {
    const known = {
      messageId: 'msg_01H',
      payloadHash: hashPayload({ task_id: 'other' }),
      disposition: 'accepted' as const,
    };
    const decision = decideDelivery(BASE_FACTS, context({ knownDelivery: known }));
    expect(decision.disposition).toBe('rejected');
    if (decision.disposition !== 'rejected') {
      throw new Error('expected rejected');
    }
    expect(decision.error.code).toBe('CONFLICT');
    expect(decision.error.details?.['reason']).toBe('payload_hash_conflict');
  });

  it('已知记录属于其它 message_id 时按首次收到处理', () => {
    const known = {
      messageId: 'msg_other',
      payloadHash: hashPayload({ task_id: 'other' }),
      disposition: 'accepted' as const,
    };
    const decision = decideDelivery(BASE_FACTS, context({ knownDelivery: known }));
    expect(decision.disposition).toBe('accepted');
  });

  it('已过期的消息判为 expired', () => {
    const facts: DeliveryFacts = { ...BASE_FACTS, expiresAt: '2026-10-24T00:59:59Z' };
    const decision = decideDelivery(facts, context());
    expect(decision.disposition).toBe('expired');
  });

  it('expires_at 正好等于当前时间时判为 expired', () => {
    const facts: DeliveryFacts = { ...BASE_FACTS, expiresAt: '2026-10-24T01:00:00Z' };
    const decision = decideDelivery(facts, context());
    expect(decision.disposition).toBe('expired');
  });

  it('expires_at 未到期时被接受', () => {
    const facts: DeliveryFacts = { ...BASE_FACTS, expiresAt: '2026-10-24T01:30:00Z' };
    const decision = decideDelivery(facts, context());
    expect(decision.disposition).toBe('accepted');
  });

  it('expires_at 非法时判为拒绝', () => {
    const facts: DeliveryFacts = { ...BASE_FACTS, expiresAt: 'not-a-date' };
    const decision = decideDelivery(facts, context());
    expect(decision.disposition).toBe('rejected');
    if (decision.disposition !== 'rejected') {
      throw new Error('expected rejected');
    }
    expect(decision.error.code).toBe('INVALID_ARGUMENT');
    expect(decision.error.details?.['reason']).toBe('malformed_expires_at');
  });

  it('sequence 未严格递增时判为拒绝', () => {
    const facts: DeliveryFacts = { ...BASE_FACTS, sequence: 4 };
    const decision = decideDelivery(facts, context({ lastAckedSequence: 4 }));
    expect(decision.disposition).toBe('rejected');
    if (decision.disposition !== 'rejected') {
      throw new Error('expected rejected');
    }
    expect(decision.error.code).toBe('INVALID_ARGUMENT');
    expect(decision.error.details?.['reason']).toBe('sequence_not_monotonic');
  });

  it('sequence 倒退时判为拒绝', () => {
    const facts: DeliveryFacts = { ...BASE_FACTS, sequence: 3 };
    const decision = decideDelivery(facts, context({ lastAckedSequence: 5 }));
    expect(decision.disposition).toBe('rejected');
    if (decision.disposition !== 'rejected') {
      throw new Error('expected rejected');
    }
    expect(decision.error.details?.['reason']).toBe('sequence_not_monotonic');
  });

  it('sequence 出现缺口时判为拒绝并给出缺口原因', () => {
    const facts: DeliveryFacts = { ...BASE_FACTS, sequence: 7 };
    const decision = decideDelivery(facts, context({ lastAckedSequence: 5 }));
    expect(decision.disposition).toBe('rejected');
    if (decision.disposition !== 'rejected') {
      throw new Error('expected rejected');
    }
    expect(decision.error.code).toBe('INVALID_ARGUMENT');
    expect(decision.error.details?.['reason']).toBe('sequence_gap');
    expect(decision.error.details?.['expected_sequence']).toBe(6);
    expect(decision.error.details?.['received_sequence']).toBe(7);
  });

  it('没有 ACK 基线时不猜测缺口', () => {
    const facts: DeliveryFacts = { ...BASE_FACTS, sequence: 99 };
    const decision = decideDelivery(facts, context());
    expect(decision.disposition).toBe('accepted');
  });
});

describe('toDeliveryRecord', () => {
  it('落盘 accepted 的记录', () => {
    const decision = decideDelivery(BASE_FACTS, context());
    const record = toDeliveryRecord(BASE_FACTS, decision, '2026-10-24T01:00:00Z');
    expect(record.disposition).toBe('accepted');
    expect(record.messageId).toBe('msg_01H');
    expect(record.payloadHash).toBe(hashPayload(BASE_FACTS.payload));
    expect(record.error).toBeUndefined();
  });

  it('重复消息保留首次落盘的 disposition', () => {
    const known = {
      messageId: 'msg_01H',
      payloadHash: hashPayload(BASE_FACTS.payload),
      disposition: 'expired' as const,
    };
    const decision = decideDelivery(BASE_FACTS, context({ knownDelivery: known }));
    const record = toDeliveryRecord(BASE_FACTS, decision, '2026-10-24T01:00:00Z');
    expect(record.disposition).toBe('expired');
  });

  it('拒绝的记录保留脱敏错误', () => {
    const facts: DeliveryFacts = { ...BASE_FACTS, sequence: 2 };
    const decision = decideDelivery(facts, context({ lastAckedSequence: 5 }));
    const record = toDeliveryRecord(facts, decision, '2026-10-24T01:00:00Z');
    expect(record.disposition).toBe('rejected');
    expect(record.error?.code).toBe('INVALID_ARGUMENT');
  });
});
