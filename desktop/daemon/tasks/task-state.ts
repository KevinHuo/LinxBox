/**
 * `task-domain/v1` 状态机 —— 领域状态、合法转换与设备投影。
 *
 * 契约来源：`docs/contracts/domain.md` §2/§4 与 `contracts/task-domain.ts`。
 * 本模块是纯函数：状态判定只依赖契约里的转换表，不读存储、不产生副作用。
 */

import {
  DEVICE_STATE_PROJECTION,
  TASK_STATES,
  TASK_TRANSITIONS,
  TERMINAL_TASK_STATES,
} from '../../contracts/index.js';
import type { DeviceTaskState, PublicErrorCode, TaskState } from '../../contracts/index.js';

const TASK_STATE_SET: ReadonlySet<string> = new Set<string>(TASK_STATES);
const TERMINAL_STATE_SET: ReadonlySet<string> = new Set<string>(TERMINAL_TASK_STATES);

/** 转换结果。失败分支只携带公共错误码与原因，不构造带 request_id 的完整错误。 */
export type TransitionResult =
  | { readonly ok: true; readonly state: TaskState }
  | { readonly ok: false; readonly code: PublicErrorCode; readonly reason: string };

/**
 * provider 终态。
 *
 * 只有 `succeeded` 是“明确成功”，`failed` 是“明确失败”，其余都是“无法证明”。
 */
export type ProviderTerminalStatus =
  'succeeded' | 'failed' | 'crashed' | 'disconnected' | 'timeout' | 'unknown';

/** 状态是否为契约定义的领域状态。 */
export function isKnownTaskState(state: string): state is TaskState {
  return TASK_STATE_SET.has(state);
}

/** 状态是否终态。未知状态返回 `false`（不假设它已经结束）。 */
export function isTerminal(state: string): boolean {
  return TERMINAL_STATE_SET.has(state);
}

/**
 * 转换是否合法。
 *
 * 终态没有任何出边，因此 `from` 为终态时恒为 `false`；契约的转换表本身也把终态作为合法
 * 目标（例如 `running -> succeeded`），那属于“正常终结”，不属于“从终态继续走”。
 */
export function canTransition(from: string, to: string): boolean {
  if (!isKnownTaskState(from) || !isKnownTaskState(to)) {
    return false;
  }
  if (isTerminal(from)) {
    return false;
  }
  if (from === 'cancel_requested' && to !== 'cancelled' && to !== 'interrupted') {
    return false;
  }
  return TASK_TRANSITIONS[from].includes(to);
}

/**
 * 执行一次状态转换。
 *
 * 非法转换返回 `CONFLICT`，未知状态返回 `INVALID_ARGUMENT`。
 */
export function transition(from: string, to: string): TransitionResult {
  if (!isKnownTaskState(from) || !isKnownTaskState(to)) {
    return {
      ok: false,
      code: 'INVALID_ARGUMENT',
      reason: `未知的任务状态: ${from} -> ${to}`,
    };
  }
  if (!canTransition(from, to)) {
    return {
      ok: false,
      code: 'CONFLICT',
      reason: `非法的任务状态转换: ${from} -> ${to}`,
    };
  }
  return { ok: true, state: to };
}

/**
 * 领域状态到设备状态的投影。
 *
 * 设备投影只用于展示，绝不反向修改领域状态；未知状态返回 `undefined`。
 */
export function toDeviceState(state: string): DeviceTaskState | undefined {
  if (!isKnownTaskState(state)) {
    return undefined;
  }
  return DEVICE_STATE_PROJECTION[state];
}

/**
 * provider 终态到本地领域状态的映射。
 *
 * 只有 provider 明确报告成功才允许进入 `succeeded`；provider 明确报告失败才允许进入 `failed`。
 * 崩溃、连接中断、超时以及任何无法证明的结局都收敛到 `interrupted`——不能用 `failed`
 * 冒充“不知道”，因为 `failed` 会向 Gateway 断言“已经证明失败”。
 */
export function localStateForProviderOutcome(status: ProviderTerminalStatus): TaskState {
  switch (status) {
    case 'succeeded':
      return 'succeeded';
    case 'failed':
      return 'failed';
    case 'crashed':
    case 'disconnected':
    case 'timeout':
    case 'unknown':
      return 'interrupted';
  }
}

/**
 * `cancel_requested` 的收尾。
 *
 * 取消请求本身不是终态证明：只有 provider 明确终结本轮时才能记为 `cancelled`；
 * 崩溃、断线、超时或无法证明时收敛到 `interrupted`。
 */
export function resolveCancelRequested(
  status: ProviderTerminalStatus,
): 'cancelled' | 'interrupted' {
  if (status === 'succeeded' || status === 'failed') {
    return 'cancelled';
  }
  return 'interrupted';
}

/**
 * 校验契约转换表的不变量。
 *
 * 断言内容：
 * - 每个终态没有任何出边（终态不转回活动态）；
 * - 终态不能作为“从终态出发”的目标；
 * - `cancel_requested` 只能收敛到 `cancelled` 或 `interrupted`。
 *
 * 注意：契约转换表允许终态作为目标（例如 `running -> succeeded`），那是“正常终结”，
 * 不是“从终态继续走”，因此这里只断言出边为空。
 */
export function assertTransitionTableInvariants(): void {
  for (const state of TASK_STATES) {
    const targets = TASK_TRANSITIONS[state];
    if (isTerminal(state) && targets.length > 0) {
      throw new Error(`终态 ${state} 不允许存在出边`);
    }
    if (state === 'cancel_requested') {
      const resolves = targets.every(
        (target) => target === 'cancelled' || target === 'interrupted',
      );
      if (!resolves) {
        throw new Error('cancel_requested 只能收敛到 cancelled 或 interrupted');
      }
    }
  }
}
