/**
 * 三级确定性投影 —— local-detail / cloud-summary / device-content。
 *
 * 契约来源：`docs/contracts/gateway-desktop.md` §9、`docs/contracts/domain.md` §7
 * 与 SPEC-0003 §5/§10。
 *
 * 隐私边界不是靠字符串黑名单实现的：
 * - 云端与设备载荷都从**显式白名单字段**构造，禁止内容根本没有进入载荷的通道；
 * - `changes[].path` 一律转换为相对授权根目录的路径，越界路径直接丢弃；
 * - `voice_summary` 是唯一的自由文本，做路径/代码/diff/凭据/堆栈清洗后截断；
 * - 最后一道防御性断言扫描已构造的云端载荷，发现绝对路径或凭据形状就**大声失败**，
 *   而不是发出违反边界的载荷。
 */

import {
  TASK_RESULT_CONTRACT_VERSION,
  TASK_RESULT_STATUSES,
  VERIFICATION_OUTCOMES,
  VOICE_SUMMARY_MAX_LENGTH,
} from '../../contracts/index.js';
import type {
  TaskResult,
  TaskResultChange,
  TaskResultRisk,
  TaskResultStatus,
  TaskResultUserAction,
  TaskResultVerification,
  VerificationOutcome,
} from '../../contracts/index.js';
import { toProjectRelativePath } from '../policy/authorization.js';

/** 本地执行事实：投影的唯一输入。 */
export interface LocalExecutionFacts {
  readonly taskId: string;
  readonly status: TaskResultStatus;
  readonly summary: string;
  /** 已授权的项目根目录（绝对路径，只在本地使用）。 */
  readonly projectRoot: string;
  readonly changes: readonly LocalChangeFact[];
  readonly verifications: readonly LocalVerificationFact[];
  readonly risks: readonly LocalRiskFact[];
  readonly needsUserAction: string | null;
  readonly voiceSummary: string;
  /** 完整 diff，只进本地详情。 */
  readonly diff?: string | undefined;
  readonly stdout?: string | undefined;
  readonly stderr?: string | undefined;
  /** 模型内部推理，只进本地详情。 */
  readonly providerReasoning?: string | undefined;
  /** 环境变量，只进本地详情。 */
  readonly environment?: Readonly<Record<string, string>> | undefined;
}

/** 一处文件改动。 */
export interface LocalChangeFact {
  readonly path: string;
  readonly description: string;
}

/** 一条验证命令。 */
export interface LocalVerificationFact {
  readonly command: string;
  readonly outcome: VerificationOutcome;
  readonly summary: string;
  /** 该验证是否真的执行过。 */
  readonly executed: boolean;
}

/** 一条未解决的风险。 */
export interface LocalRiskFact {
  readonly code: string;
  readonly summary: string;
}

/** 本地详情中的改动条目。 */
export interface LocalDetailChange {
  readonly absolutePath: string;
  readonly relativePath: string | null;
  readonly description: string;
}

/** 本地详情中的验证条目。 */
export interface LocalDetailVerification {
  readonly command: string;
  readonly outcome: VerificationOutcome;
  readonly summary: string;
  readonly executed: boolean;
  readonly stdout: string | null;
  readonly stderr: string | null;
}

/** 完整本地视图：含 diff、原始输出与绝对路径，只留在本机。 */
export interface LocalDetailProjection {
  readonly kind: 'local_detail';
  readonly taskId: string;
  readonly status: TaskResultStatus;
  readonly summary: string;
  readonly projectRoot: string;
  readonly changes: readonly LocalDetailChange[];
  readonly diff: string | null;
  readonly verification: readonly LocalDetailVerification[];
  readonly risks: readonly TaskResultRisk[];
  readonly needsUserAction: TaskResultUserAction | null;
  readonly providerReasoning: string | null;
  readonly environment: Readonly<Record<string, string>> | null;
}

/** 允许上传的云端摘要。 */
export interface CloudSummaryProjection {
  readonly kind: 'cloud_summary';
  readonly taskId: string;
  readonly status: TaskResultStatus;
  readonly summary: string;
  readonly changes: readonly TaskResultChange[];
  readonly verification: readonly TaskResultVerification[];
  readonly risks: readonly TaskResultRisk[];
  readonly needsUserAction: TaskResultUserAction | null;
  readonly voiceSummary: string;
}

/** 设备安全内容：云端摘要的子集。 */
export interface DeviceContentProjection {
  readonly kind: 'device_content';
  readonly taskId: string;
  readonly status: TaskResultStatus;
  readonly summary: string;
  readonly voiceSummary: string;
}

/** 载荷越界时抛出：宁可失败，也不发出违反隐私边界的载荷。 */
export class ProjectionBoundaryError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ProjectionBoundaryError';
  }
}

const CODE_FENCE_PATTERN = /```[\s\S]*?```/g;
const DIFF_LINE_PATTERN = /^[ \t]*(?:diff --git|index |@@|\+\+\+|---)[^\n]*$/gm;
const STACK_FRAME_PATTERN = /^[ \t]*at\s+\S+[^\n]*$/gm;
const TRACEBACK_PATTERN = /Traceback \(most recent call last\)/g;
/**
 * 路径 token。
 *
 * 只认以 `/`、`./`、`../` 开头的 token，且前一个字符不能是字母、数字、下划线、`:` 或 `/`
 * ——这样 `src/login.ts`、`https://example.com` 不会被误判成绝对路径。
 */
const PATH_TOKEN_PATTERN = /(?<![A-Za-z0-9_:/])(?:\.\.?\/|\/)[^\s'"`),;]+/g;
/** 绝对路径 token 的检测规则（与清洗规则一致）。 */
const ABSOLUTE_PATH_DETECT = /(?<![A-Za-z0-9_:/])\/[^\s'"`),;]+/;
/** 路径穿越 token 的检测规则。 */
const PATH_TRAVERSAL_DETECT = /\.\.[\\/]/;
const CREDENTIAL_PATTERNS: readonly RegExp[] = [
  /\bsk-[A-Za-z0-9]{16,}\b/g,
  /\bgh[pousr]_[A-Za-z0-9]{20,}\b/g,
  /\bAKIA[0-9A-Z]{16}\b/g,
  /\bAIza[0-9A-Za-z_-]{35}\b/g,
  /\bxox[baprs]-[A-Za-z0-9-]{10,}\b/g,
  /\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b/g,
  /-----BEGIN [A-Z ]*PRIVATE KEY-----/g,
  /\b[Bb]earer\s+[A-Za-z0-9._~+/-]{10,}=*/g,
];

/**
 * 完整本地视图。
 *
 * 这是唯一允许包含完整 diff、原始 stdout/stderr、模型内部推理、环境变量与绝对路径的投影，
 * 它的产物只写进本地受保护的存储。
 */
export function projectLocalDetail(facts: LocalExecutionFacts): LocalDetailProjection {
  const parts = buildCloudParts(facts);
  return {
    kind: 'local_detail',
    taskId: facts.taskId,
    status: parts.status,
    summary: facts.summary,
    projectRoot: facts.projectRoot,
    changes: facts.changes.map((change) => ({
      absolutePath: change.path,
      relativePath: toProjectRelativePath(facts.projectRoot, change.path) ?? null,
      description: change.description,
    })),
    diff: facts.diff ?? null,
    verification: facts.verifications.map((verification) => ({
      command: verification.command,
      outcome: verification.outcome,
      summary: verification.summary,
      executed: verification.executed,
      stdout: facts.stdout ?? null,
      stderr: facts.stderr ?? null,
    })),
    risks: parts.risks,
    needsUserAction: parts.needsUserAction,
    providerReasoning: facts.providerReasoning ?? null,
    environment: facts.environment ?? null,
  };
}

/**
 * 云端摘要：允许上传的内容。
 *
 * 字段全部来自白名单，`changes[].path` 一定是相对路径，原始 diff/stdout/stderr/推理/环境变量
 * 没有任何进入通道。
 */
export function projectCloudSummary(facts: LocalExecutionFacts): CloudSummaryProjection {
  const parts = buildCloudParts(facts);
  const payload: CloudSummaryProjection = {
    kind: 'cloud_summary',
    taskId: facts.taskId,
    status: parts.status,
    summary: parts.summary,
    changes: parts.changes,
    verification: parts.verification,
    risks: parts.risks,
    needsUserAction: parts.needsUserAction,
    voiceSummary: parts.voiceSummary,
  };
  assertUploadable(payload);
  return payload;
}

/**
 * 设备安全内容：云端摘要的子集。
 *
 * 不含改动路径、验证命令与风险明细，只保留设备可以直接展示的短文本。
 */
export function projectDeviceContent(facts: LocalExecutionFacts): DeviceContentProjection {
  const cloud = projectCloudSummary(facts);
  return {
    kind: 'device_content',
    taskId: cloud.taskId,
    status: cloud.status,
    summary: cloud.summary,
    voiceSummary: cloud.voiceSummary,
  };
}

/**
 * 构造 `task-result/v1` 结果对象。
 *
 * 规则：
 * - `changes[].path` 相对授权根目录，越界或含 `..` 的路径被丢弃；
 * - `verification[].outcome` 只取契约三值，未真实执行的验证不可能是 `passed`；
 * - `voice_summary` 经过清洗并截断到 `VOICE_SUMMARY_MAX_LENGTH`；
 * - `status` 必须是契约状态；证据不足时不会声称 `succeeded`。
 */
export function buildTaskResult(facts: LocalExecutionFacts): TaskResult {
  const parts = buildCloudParts(facts);
  const result: TaskResult = {
    contract_version: TASK_RESULT_CONTRACT_VERSION,
    status: parts.status,
    summary: parts.summary,
    changes: parts.changes,
    verification: parts.verification,
    risks: parts.risks,
    needs_user_action: parts.needsUserAction,
    voice_summary: parts.voiceSummary,
  };
  assertUploadable(result);
  return result;
}

/**
 * 清洗自由文本：去掉代码块、diff、堆栈，遮蔽路径与凭据，并压缩空白。
 */
export function sanitizeVoiceSummary(text: string): string {
  let result = text.replace(CODE_FENCE_PATTERN, ' ');
  result = result.replace(DIFF_LINE_PATTERN, ' ');
  result = result.replace(STACK_FRAME_PATTERN, ' ');
  result = result.replace(TRACEBACK_PATTERN, ' ');
  result = result.replace(PATH_TOKEN_PATTERN, '[路径]');
  result = redactCredentials(result);
  result = result.replace(/\s+/g, ' ').trim();
  return truncateVoiceSummary(result);
}

/** 按字符（码位）确定性截断，长度不会超过 `VOICE_SUMMARY_MAX_LENGTH`。 */
export function truncateVoiceSummary(text: string): string {
  const characters = Array.from(text);
  if (characters.length <= VOICE_SUMMARY_MAX_LENGTH) {
    return text;
  }
  return `${characters.slice(0, VOICE_SUMMARY_MAX_LENGTH - 1).join('')}…`;
}

/** 字符串是否形似绝对路径。 */
export function isAbsolutePathLike(value: string): boolean {
  return value.length > 1 && value.startsWith('/') && value.includes('/');
}

/**
 * 字符串是否包含绝对路径。
 *
 * 除了“以 `/` 开头”的形状，也覆盖嵌入在文本中间的绝对路径，
 * 因为后者同样会泄露本机目录结构。
 */
export function containsAbsolutePath(value: string): boolean {
  if (isAbsolutePathLike(value)) {
    return true;
  }
  return ABSOLUTE_PATH_DETECT.test(value);
}

/** 字符串是否包含路径穿越 token。 */
export function containsPathTraversal(value: string): boolean {
  return PATH_TRAVERSAL_DETECT.test(value);
}

/** 字符串是否形似凭据。 */
export function looksLikeCredential(value: string): boolean {
  return CREDENTIAL_PATTERNS.some((pattern) => value.match(pattern) !== null);
}

/**
 * 防御性断言：扫描已构造载荷中的所有字符串。
 *
 * 命中绝对路径、路径穿越或凭据形状就抛出 `ProjectionBoundaryError`，让调用方 fail closed。
 */
export function assertUploadable(payload: unknown): void {
  const values: string[] = [];
  collectStrings(payload, values);
  for (const value of values) {
    if (containsPathTraversal(value)) {
      throw new ProjectionBoundaryError('云端载荷包含疑似路径穿越，已阻止上传');
    }
    if (containsAbsolutePath(value)) {
      throw new ProjectionBoundaryError('云端载荷包含疑似绝对路径，已阻止上传');
    }
    if (looksLikeCredential(value)) {
      throw new ProjectionBoundaryError('云端载荷包含疑似凭据，已阻止上传');
    }
  }
}

interface CloudParts {
  readonly status: TaskResultStatus;
  readonly summary: string;
  readonly changes: readonly TaskResultChange[];
  readonly verification: readonly TaskResultVerification[];
  readonly risks: readonly TaskResultRisk[];
  readonly needsUserAction: TaskResultUserAction | null;
  readonly voiceSummary: string;
  readonly droppedChanges: number;
  readonly downgradedVerification: boolean;
}

function buildCloudParts(facts: LocalExecutionFacts): CloudParts {
  const changes: TaskResultChange[] = [];
  let droppedChanges = 0;
  for (const change of facts.changes) {
    const relativePath = toProjectRelativePath(facts.projectRoot, change.path);
    if (relativePath === undefined) {
      droppedChanges += 1;
      continue;
    }
    changes.push({ path: relativePath, description: change.description });
  }

  const verification: TaskResultVerification[] = [];
  let downgradedVerification = false;
  for (const item of facts.verifications) {
    const outcome = resolveVerificationOutcome(item);
    if (outcome !== item.outcome) {
      downgradedVerification = true;
    }
    verification.push({ command: item.command, outcome, summary: item.summary });
  }

  const risks: TaskResultRisk[] = facts.risks.map((risk) => ({
    code: risk.code,
    summary: risk.summary,
  }));

  const needsUserAction: TaskResultUserAction | null =
    facts.needsUserAction === null ? null : { summary: facts.needsUserAction };

  const status = deriveStatus(facts.status, droppedChanges, downgradedVerification);

  return {
    status,
    summary: facts.summary,
    changes,
    verification,
    risks,
    needsUserAction,
    voiceSummary: sanitizeVoiceSummary(facts.voiceSummary),
    droppedChanges,
    downgradedVerification,
  };
}

/**
 * 验证结果收敛。
 *
 * 未真实执行的验证不可能是 `passed`（也不能声称 `failed`），一律降级为 `not_run`。
 */
function resolveVerificationOutcome(item: LocalVerificationFact): VerificationOutcome {
  if (!(VERIFICATION_OUTCOMES as readonly string[]).includes(item.outcome)) {
    return 'not_run';
  }
  if (!item.executed && item.outcome !== 'not_run') {
    return 'not_run';
  }
  return item.outcome;
}

/**
 * 结果状态收敛。
 *
 * `succeeded` 需要可证明的本地事实：声称成功的验证必须真的执行过，声称的改动必须都在
 * 授权根目录内。证据不足时收敛到 `interrupted`——不能用 `failed` 冒充“不知道”。
 */
function deriveStatus(
  claimed: TaskResultStatus,
  droppedChanges: number,
  downgradedVerification: boolean,
): TaskResultStatus {
  const base = (TASK_RESULT_STATUSES as readonly string[]).includes(claimed)
    ? claimed
    : 'interrupted';
  if (base === 'succeeded' && (droppedChanges > 0 || downgradedVerification)) {
    return 'interrupted';
  }
  return base;
}

function redactCredentials(text: string): string {
  let result = text;
  for (const pattern of CREDENTIAL_PATTERNS) {
    result = result.replace(pattern, '[凭据]');
  }
  return result;
}

function collectStrings(value: unknown, out: string[]): void {
  if (typeof value === 'string') {
    out.push(value);
    return;
  }
  if (Array.isArray(value)) {
    for (const item of value) {
      collectStrings(item, out);
    }
    return;
  }
  if (typeof value === 'object' && value !== null) {
    const source = value as Record<string, unknown>;
    for (const key of Object.keys(source)) {
      collectStrings(source[key], out);
    }
  }
}
