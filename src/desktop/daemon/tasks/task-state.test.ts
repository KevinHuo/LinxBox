import { describe, expect, it } from 'vitest';

import { TASK_TRANSITIONS } from '../../contracts/index.js';
import type { TaskState } from '../../contracts/index.js';
import {
  assertTransitionTableInvariants,
  canTransition,
  isKnownTaskState,
  isTerminal,
  localStateForProviderOutcome,
  resolveCancelRequested,
  toDeviceState,
  transition,
} from './task-state.js';

describe('isKnownTaskState', () => {
  it('识别契约定义的状态', () => {
    expect(isKnownTaskState('running')).toBe(true);
  });

  it('拒绝未知状态', () => {
    expect(isKnownTaskState('teleporting')).toBe(false);
  });
});

describe('isTerminal', () => {
  it('终态为真', () => {
    expect(isTerminal('succeeded')).toBe(true);
    expect(isTerminal('interrupted')).toBe(true);
  });

  it('活动态为假', () => {
    expect(isTerminal('running')).toBe(false);
  });

  it('未知状态不假设已经结束', () => {
    expect(isTerminal('teleporting')).toBe(false);
  });
});

describe('canTransition', () => {
  it('允许契约表内的转换', () => {
    expect(canTransition('queued', 'dispatched')).toBe(true);
    expect(canTransition('running', 'succeeded')).toBe(true);
  });

  it('拒绝非法转换', () => {
    expect(canTransition('requested', 'running')).toBe(false);
    expect(canTransition('succeeded', 'running')).toBe(false);
  });

  it('终态没有任何出边', () => {
    expect(canTransition('failed', 'queued')).toBe(false);
    expect(canTransition('cancelled', 'running')).toBe(false);
    expect(canTransition('expired', 'dispatched')).toBe(false);
    expect(canTransition('rejected', 'queued')).toBe(false);
  });

  it('未知状态不可转换', () => {
    expect(canTransition('teleporting', 'running')).toBe(false);
    expect(canTransition('running', 'teleporting')).toBe(false);
  });

  it('cancel_requested 只能收敛到 cancelled 或 interrupted', () => {
    expect(canTransition('cancel_requested', 'cancelled')).toBe(true);
    expect(canTransition('cancel_requested', 'interrupted')).toBe(true);
    expect(canTransition('cancel_requested', 'running')).toBe(false);
    expect(canTransition('cancel_requested', 'succeeded')).toBe(false);
  });
});

describe('transition', () => {
  it('合法转换返回目标状态', () => {
    const result = transition('running', 'succeeded');
    expect(result).toEqual({ ok: true, state: 'succeeded' });
  });

  it('非法转换返回 CONFLICT', () => {
    const result = transition('requested', 'running');
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.code).toBe('CONFLICT');
  });

  it('从终态转换返回 CONFLICT', () => {
    const result = transition('succeeded', 'running');
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.code).toBe('CONFLICT');
  });

  it('未知起点返回 INVALID_ARGUMENT', () => {
    const result = transition('teleporting', 'running');
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.code).toBe('INVALID_ARGUMENT');
  });

  it('未知目标返回 INVALID_ARGUMENT', () => {
    const result = transition('running', 'teleporting');
    expect(result.ok).toBe(false);
    if (result.ok) {
      throw new Error('expected failure');
    }
    expect(result.code).toBe('INVALID_ARGUMENT');
  });
});

describe('toDeviceState', () => {
  it('按投影表映射设备状态', () => {
    expect(toDeviceState('requested')).toBe('queued');
    expect(toDeviceState('dispatched')).toBe('queued');
    expect(toDeviceState('waiting_for_client')).toBe('needs_attention');
    expect(toDeviceState('approval_required')).toBe('needs_attention');
    expect(toDeviceState('running')).toBe('running');
    expect(toDeviceState('cancel_requested')).toBe('running');
    expect(toDeviceState('succeeded')).toBe('completed');
    expect(toDeviceState('rejected')).toBe('failed');
    expect(toDeviceState('expired')).toBe('failed');
    expect(toDeviceState('failed')).toBe('failed');
    expect(toDeviceState('cancelled')).toBe('failed');
    expect(toDeviceState('interrupted')).toBe('failed');
  });

  it('未知状态没有设备投影', () => {
    expect(toDeviceState('teleporting')).toBeUndefined();
  });
});

describe('localStateForProviderOutcome', () => {
  it('provider 明确成功才能进入 succeeded', () => {
    expect(localStateForProviderOutcome('succeeded')).toBe('succeeded');
  });

  it('provider 明确失败才进入 failed', () => {
    expect(localStateForProviderOutcome('failed')).toBe('failed');
  });

  it('崩溃、断线、超时与未知都收敛到 interrupted', () => {
    expect(localStateForProviderOutcome('crashed')).toBe('interrupted');
    expect(localStateForProviderOutcome('disconnected')).toBe('interrupted');
    expect(localStateForProviderOutcome('timeout')).toBe('interrupted');
    expect(localStateForProviderOutcome('unknown')).toBe('interrupted');
  });

  it('未知结局不会被当成 failed', () => {
    expect(localStateForProviderOutcome('unknown')).not.toBe('failed');
  });
});

describe('resolveCancelRequested', () => {
  it('provider 明确终结时收敛到 cancelled', () => {
    expect(resolveCancelRequested('succeeded')).toBe('cancelled');
    expect(resolveCancelRequested('failed')).toBe('cancelled');
  });

  it('无法证明时收敛到 interrupted', () => {
    expect(resolveCancelRequested('crashed')).toBe('interrupted');
    expect(resolveCancelRequested('disconnected')).toBe('interrupted');
    expect(resolveCancelRequested('timeout')).toBe('interrupted');
    expect(resolveCancelRequested('unknown')).toBe('interrupted');
  });
});

describe('assertTransitionTableInvariants', () => {
  it('契约转换表满足不变量', () => {
    expect(() => assertTransitionTableInvariants()).not.toThrow();
  });

  it('终态出现出边时守卫生效', () => {
    const targets = TASK_TRANSITIONS.succeeded as unknown as TaskState[];
    const original = [...targets];
    try {
      targets.push('running');
      expect(() => assertTransitionTableInvariants()).toThrow(/终态/);
    } finally {
      targets.splice(0, targets.length, ...original);
    }
  });

  it('cancel_requested 收敛到非法目标时守卫生效', () => {
    const targets = TASK_TRANSITIONS.cancel_requested as unknown as TaskState[];
    const original = [...targets];
    try {
      targets.push('running');
      expect(() => assertTransitionTableInvariants()).toThrow(/cancel_requested/);
    } finally {
      targets.splice(0, targets.length, ...original);
    }
  });
});
