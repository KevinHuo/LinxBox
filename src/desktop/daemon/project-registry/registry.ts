/**
 * Project Registry —— `project_id -> realpath + grant + grant_version` 的本地权威。
 *
 * 契约来源：SPEC-0003 §5/§10 与 `docs/contracts/gateway-desktop.md` §4。
 *
 * 绝对路径只存在于本模块的返回值里；本模块不提供任何包含 realpath 的对外 DTO，
 * `toPublicProjectView` 产出的展示视图刻意不含 realpath。
 */

import { realpath, stat } from 'node:fs/promises';

import { PROJECT_CAPABILITIES } from '../../contracts/index.js';
import type { ProjectCapability } from '../../contracts/index.js';

/** 本地项目授权记录。 */
export interface ProjectGrant {
  readonly projectId: string;
  /** 授权根目录的 canonical realpath，只在本地使用。 */
  readonly realpath: string;
  readonly displayName: string;
  readonly capabilities: readonly ProjectCapability[];
  /** 本地单调递增的 grant 版本。 */
  readonly grantVersion: number;
  /** 撤销时间；`null` 表示未撤销。 */
  readonly revokedAt: string | null;
}

/** 注册项目时的输入。 */
export interface RegisterProjectInput {
  readonly projectId: string;
  readonly rootPath: string;
  readonly displayName: string;
  readonly capabilities: readonly ProjectCapability[];
}

/** 对外可展示的项目视图（不含任何本机路径）。 */
export interface PublicProjectView {
  readonly project_id: string;
  readonly display_name: string;
  readonly capabilities: readonly ProjectCapability[];
  readonly availability: 'online' | 'offline';
  readonly grant_version: number;
}

/** 项目根目录无法使用时抛出的错误。 */
export class ProjectRegistryError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ProjectRegistryError';
  }
}

/**
 * 把项目根目录解析为 canonical realpath。
 *
 * 目录不存在或不是目录时抛错——授权只能建立在真实存在的目录上。
 */
export async function resolveProjectRoot(rootPath: string): Promise<string> {
  let canonical: string;
  try {
    canonical = await realpath(rootPath);
  } catch {
    throw new ProjectRegistryError('项目根目录不存在或不可解析');
  }
  const stats = await stat(canonical);
  if (!stats.isDirectory()) {
    throw new ProjectRegistryError('项目根目录不是目录');
  }
  return canonical;
}

/**
 * 内存态项目注册表。
 *
 * `grantVersion` 是本地的单调计数：首次注册为 1，之后每次注册或撤销都自增。
 * Gateway 侧的 `grant_version` 由组装层映射到该本地版本，本模块不解释远端语义。
 */
export class ProjectRegistry {
  private readonly grants = new Map<string, ProjectGrant>();

  /**
   * 注册或更新一个项目授权。
   *
   * 同一 `project_id` 再次注册会更新内容并自增 grant 版本；重新注册视为一次新的显式授权，
   * 因此会清除既有的撤销标记。
   */
  async register(input: RegisterProjectInput): Promise<ProjectGrant> {
    const canonical = await resolveProjectRoot(input.rootPath);
    const existing = this.grants.get(input.projectId);
    const grant: ProjectGrant = {
      projectId: input.projectId,
      realpath: canonical,
      displayName: input.displayName,
      capabilities: [...input.capabilities],
      grantVersion: existing === undefined ? 1 : existing.grantVersion + 1,
      revokedAt: null,
    };
    this.grants.set(input.projectId, grant);
    return grant;
  }

  /**
   * 撤销一个项目授权。
   *
   * 撤销会写入 `revokedAt` 并自增 grant 版本；未知项目返回 `undefined`。
   */
  revoke(projectId: string, revokedAt: string): ProjectGrant | undefined {
    const existing = this.grants.get(projectId);
    if (existing === undefined) {
      return undefined;
    }
    const grant: ProjectGrant = {
      ...existing,
      grantVersion: existing.grantVersion + 1,
      revokedAt,
    };
    this.grants.set(projectId, grant);
    return grant;
  }

  /** 解析一个有效的授权；未知或已撤销的项目返回 `undefined`。 */
  resolve(projectId: string): ProjectGrant | undefined {
    const grant = this.grants.get(projectId);
    if (grant === undefined || grant.revokedAt !== null) {
      return undefined;
    }
    return grant;
  }

  /** 读取原始记录，包含已撤销的项目。 */
  get(projectId: string): ProjectGrant | undefined {
    return this.grants.get(projectId);
  }

  /** 列出全部记录（含已撤销）。 */
  list(): readonly ProjectGrant[] {
    return [...this.grants.values()];
  }
}

/**
 * 生成不含本机路径的项目展示视图。
 *
 * `project.upsert` 的 payload 永远不携带绝对路径，因此该视图刻意不含 realpath。
 */
export function toPublicProjectView(grant: ProjectGrant): PublicProjectView {
  return {
    project_id: grant.projectId,
    display_name: grant.displayName,
    capabilities: [...grant.capabilities],
    availability: grant.revokedAt === null ? 'online' : 'offline',
    grant_version: grant.grantVersion,
  };
}

/** 能力名是否属于契约定义的项目能力集合。 */
export function isProjectCapability(value: string): value is ProjectCapability {
  return (PROJECT_CAPABILITIES as readonly string[]).includes(value);
}
