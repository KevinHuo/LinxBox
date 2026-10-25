/**
 * 执行前授权 —— grant、realpath 包含关系与 R0-R3 风险闸门。
 *
 * 契约来源：SPEC-0003 §5/§8/§10 与 `docs/contracts/domain.md` §6。
 *
 * 一项操作只有在下列条件全部成立时才被允许：
 * 1. 项目授权存在且未被撤销；
 * 2. 请求使用的 grant 版本与注册表当前版本一致（否则说明授权在脚下变了）；
 * 3. 请求需要的能力全部包含在已授予能力中；
 * 4. 目标路径解析后仍位于授权根目录内；
 * 5. 风险等级被放行（R0/R1 随 grant，R2 需要可信本地审批，R3 永久拒绝）。
 *
 * 模块只读取文件系统元数据（`realpath`/`stat`），不写入任何内容。
 */

import { realpath } from 'node:fs/promises';
import { dirname, isAbsolute, relative, resolve, sep } from 'node:path';

import { RISK_RULES } from '../../contracts/index.js';
import type { ProjectCapability, PublicErrorCode, RiskLevel } from '../../contracts/index.js';
import type { ProjectGrant } from '../project-registry/registry.js';

/** 授权查询来源。注册表满足该接口。 */
export interface ProjectGrantSource {
  /** 有效授权；未知或已撤销返回 `undefined`。 */
  resolve(projectId: string): ProjectGrant | undefined;
  /** 原始记录；未知返回 `undefined`，已撤销的记录仍可读取。 */
  get(projectId: string): ProjectGrant | undefined;
}

/** 一次待授权的操作。 */
export interface AuthorizationRequest {
  readonly projectId: string;
  /** 请求方观察到的 grant 版本。 */
  readonly grantVersion: number;
  readonly requiredCapabilities: readonly ProjectCapability[];
  /** 相对于授权根目录的目标路径。 */
  readonly targetPath: string;
  readonly riskLevel: RiskLevel;
  /** 用于人类可读原因的操作名。 */
  readonly operation?: string | undefined;
}

/** 授权判定结果。 */
export type AuthorizationDecision =
  | { readonly allowed: true; readonly riskLevel: RiskLevel }
  | { readonly allowed: false; readonly code: PublicErrorCode; readonly reason: string };

/**
 * 判断候选路径是否位于根目录内。
 *
 * 边界比较使用“根目录 + 分隔符”前缀，因此 `/a/proj` 不会接纳 `/a/proj-other`。
 */
export function isPathInsideRoot(root: string, candidate: string): boolean {
  if (candidate === root) {
    return true;
  }
  const prefix = root.endsWith(sep) ? root : `${root}${sep}`;
  return candidate.startsWith(prefix);
}

/**
 * 授权判定。
 *
 * `hasTrustedApprovalUi` 由调用方显式传入：R2 只有在存在可信本地审批界面时才可能被放行，
 * R3 无论如何都拒绝。
 */
export async function authorizeOperation(
  request: AuthorizationRequest,
  source: ProjectGrantSource,
  hasTrustedApprovalUi = false,
): Promise<AuthorizationDecision> {
  const record = source.get(request.projectId);
  if (record === undefined) {
    return deny('PERMISSION_DENIED', '项目未授权');
  }
  if (record.revokedAt !== null) {
    return deny('PERMISSION_DENIED', '项目授权已被撤销');
  }
  if (request.grantVersion !== record.grantVersion) {
    return deny('CONFLICT', '授权版本已变化');
  }

  for (const capability of request.requiredCapabilities) {
    if (!record.capabilities.includes(capability)) {
      return deny('PERMISSION_DENIED', `缺少项目能力: ${capability}`);
    }
  }

  const pathDecision = await checkTargetPath(record.realpath, request.targetPath);
  if (pathDecision !== undefined) {
    return deny('POLICY_DENIED', pathDecision);
  }

  return checkRiskLevel(request.riskLevel, hasTrustedApprovalUi);
}

/**
 * 风险等级闸门。
 *
 * R0/R1 随 grant 放行；R2 需要可信本地审批界面，否则 `APPROVAL_REQUIRED`；
 * R3 永久拒绝，即使存在审批界面也不放行。
 */
export function checkRiskLevel(
  riskLevel: RiskLevel,
  hasTrustedApprovalUi: boolean,
): AuthorizationDecision {
  const rule = riskRuleFor(riskLevel);
  if (rule === undefined) {
    return deny('POLICY_DENIED', '未知的风险等级');
  }
  if (rule.allowed) {
    return { allowed: true, riskLevel };
  }
  if (riskLevel === 'R2') {
    return hasTrustedApprovalUi
      ? { allowed: true, riskLevel }
      : deny('APPROVAL_REQUIRED', rule.reason);
  }
  return deny('POLICY_DENIED', rule.reason);
}

function riskRuleFor(
  riskLevel: string,
): { readonly allowed: boolean; readonly reason: string } | undefined {
  if (!Object.prototype.hasOwnProperty.call(RISK_RULES, riskLevel)) {
    return undefined;
  }
  return RISK_RULES[riskLevel as RiskLevel];
}

/**
 * 校验目标路径是否仍在授权根目录内。
 *
 * 拒绝绝对路径注入与 `..` 穿越；存在时用 `realpath` 比较，展开 symlink 后仍越界即拒绝；
 * 目标尚不存在时解析其最近的已存在父目录，父目录越界同样拒绝。
 *
 * 返回 `undefined` 表示通过，否则返回拒绝原因。
 */
async function checkTargetPath(root: string, targetPath: string): Promise<string | undefined> {
  if (targetPath.length === 0) {
    return '目标路径为空';
  }
  if (isAbsolute(targetPath)) {
    return '目标路径必须是相对路径';
  }
  if (targetPath.split(/[\\/]+/).includes('..')) {
    return '目标路径不允许包含 .. 穿越';
  }

  let canonicalRoot: string;
  try {
    canonicalRoot = await realpath(root);
  } catch {
    return '授权根目录不可解析';
  }

  const candidate = resolve(canonicalRoot, targetPath);
  try {
    const canonicalCandidate = await realpath(candidate);
    if (!isPathInsideRoot(canonicalRoot, canonicalCandidate)) {
      return '目标路径解析后逃逸出授权根目录';
    }
    return undefined;
  } catch (error) {
    if (!isNotFound(error)) {
      return '目标路径不可解析';
    }
  }

  const parent = dirname(candidate);
  try {
    const canonicalParent = await realpath(parent);
    if (!isPathInsideRoot(canonicalRoot, canonicalParent)) {
      return '目标路径的父目录逃逸出授权根目录';
    }
    return undefined;
  } catch {
    return '目标路径的父目录不可解析';
  }
}

function isNotFound(error: unknown): boolean {
  return (
    typeof error === 'object' && error !== null && (error as { code?: unknown }).code === 'ENOENT'
  );
}

function deny(code: PublicErrorCode, reason: string): AuthorizationDecision {
  return { allowed: false, code, reason };
}

/**
 * 把目标路径转换为相对授权根目录的路径。
 *
 * 绝对路径在根目录内会被转换为相对路径；越界或含 `..` 的路径返回 `undefined`。
 */
export function toProjectRelativePath(root: string, targetPath: string): string | undefined {
  if (targetPath.length === 0) {
    return undefined;
  }
  if (targetPath.split(/[\\/]+/).includes('..')) {
    return undefined;
  }
  const canonicalRoot = resolve(root);
  const resolved = isAbsolute(targetPath)
    ? resolve(targetPath)
    : resolve(canonicalRoot, targetPath);
  const rel = relative(canonicalRoot, resolved);
  if (rel.length === 0 || rel === '..' || rel.startsWith(`..${sep}`)) {
    return undefined;
  }
  return rel.split(sep).join('/');
}
