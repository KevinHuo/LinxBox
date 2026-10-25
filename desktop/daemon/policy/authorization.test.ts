import { mkdir, mkdtemp, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterAll, beforeAll, describe, expect, it } from 'vitest';

import type { ProjectGrant } from '../project-registry/registry.js';
import {
  authorizeOperation,
  checkRiskLevel,
  isPathInsideRoot,
  toProjectRelativePath,
} from './authorization.js';
import type { AuthorizationRequest, ProjectGrantSource } from './authorization.js';

let workspace = '';
let projectRoot = '';
let siblingRoot = '';

const GRANT: ProjectGrant = {
  projectId: 'prj_01H',
  realpath: '',
  displayName: '商城前端',
  capabilities: ['read', 'write', 'test'],
  grantVersion: 3,
  revokedAt: null,
};

function sourceOf(grant: ProjectGrant | undefined): ProjectGrantSource {
  return {
    resolve: (projectId) =>
      grant !== undefined && grant.projectId === projectId && grant.revokedAt === null
        ? grant
        : undefined,
    get: (projectId) => (grant !== undefined && grant.projectId === projectId ? grant : undefined),
  };
}

function request(overrides: Partial<AuthorizationRequest> = {}): AuthorizationRequest {
  return {
    projectId: 'prj_01H',
    grantVersion: 3,
    requiredCapabilities: ['read'],
    targetPath: 'src/app.ts',
    riskLevel: 'R0',
    ...overrides,
  };
}

beforeAll(async () => {
  workspace = await mkdtemp(join(tmpdir(), 'linxbox-policy-'));
  projectRoot = join(workspace, 'proj');
  siblingRoot = join(workspace, 'proj-other');
  await mkdir(join(projectRoot, 'src'), { recursive: true });
  await mkdir(siblingRoot, { recursive: true });
  await writeFile(join(projectRoot, 'src', 'app.ts'), 'export const a = 1;\n', 'utf8');
  await writeFile(join(siblingRoot, 'secret.txt'), 'secret\n', 'utf8');
  await symlink(siblingRoot, join(projectRoot, 'escape'), 'dir');
  await symlink(join(siblingRoot, 'secret.txt'), join(projectRoot, 'src', 'linked.txt'), 'file');
});

afterAll(async () => {
  await rm(workspace, { recursive: true, force: true });
});

describe('isPathInsideRoot', () => {
  it('根目录自身在边界内', () => {
    expect(isPathInsideRoot('/a/proj', '/a/proj')).toBe(true);
  });

  it('子目录在边界内', () => {
    expect(isPathInsideRoot('/a/proj', '/a/proj/src/app.ts')).toBe(true);
  });

  it('同名前缀的兄弟目录不在边界内', () => {
    expect(isPathInsideRoot('/a/proj', '/a/proj-other/secret.txt')).toBe(false);
  });

  it('根目录是 / 时一切绝对路径都在边界内', () => {
    expect(isPathInsideRoot('/', '/etc/hosts')).toBe(true);
  });
});

describe('toProjectRelativePath', () => {
  it('把根目录内的绝对路径转成相对路径', () => {
    expect(toProjectRelativePath('/a/proj', '/a/proj/src/app.ts')).toBe('src/app.ts');
  });

  it('保留已经相对路径', () => {
    expect(toProjectRelativePath('/a/proj', 'src/app.ts')).toBe('src/app.ts');
  });

  it('拒绝越界路径', () => {
    expect(toProjectRelativePath('/a/proj', '/a/proj-other/secret.txt')).toBeUndefined();
  });

  it('拒绝包含 .. 的路径', () => {
    expect(toProjectRelativePath('/a/proj', '../proj-other/secret.txt')).toBeUndefined();
  });

  it('拒绝空路径与根目录本身', () => {
    expect(toProjectRelativePath('/a/proj', '')).toBeUndefined();
    expect(toProjectRelativePath('/a/proj', '/a/proj')).toBeUndefined();
  });
});

describe('checkRiskLevel', () => {
  it('R0 与 R1 随 grant 放行', () => {
    expect(checkRiskLevel('R0', false)).toEqual({ allowed: true, riskLevel: 'R0' });
    expect(checkRiskLevel('R1', false)).toEqual({ allowed: true, riskLevel: 'R1' });
  });

  it('R2 没有可信审批界面时要求审批', () => {
    const decision = checkRiskLevel('R2', false);
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('APPROVAL_REQUIRED');
  });

  it('R2 存在可信审批界面时放行', () => {
    expect(checkRiskLevel('R2', true)).toEqual({ allowed: true, riskLevel: 'R2' });
  });

  it('R3 永远拒绝', () => {
    const decision = checkRiskLevel('R3', true);
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });

  it('未知风险等级拒绝', () => {
    const decision = checkRiskLevel('R9' as 'R0', false);
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });
});

describe('authorizeOperation', () => {
  it('授权、版本、能力、路径与风险全部成立时放行', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(request(), sourceOf(grant));
    expect(decision).toEqual({ allowed: true, riskLevel: 'R0' });
  });

  it('未知项目被拒绝', async () => {
    const decision = await authorizeOperation(request(), sourceOf(undefined));
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('PERMISSION_DENIED');
  });

  it('已撤销的授权被拒绝', async () => {
    const grant = { ...GRANT, realpath: projectRoot, revokedAt: '2026-10-24T01:00:00Z' };
    const decision = await authorizeOperation(request(), sourceOf(grant));
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('PERMISSION_DENIED');
  });

  it('grant 版本不一致时返回 CONFLICT', async () => {
    const grant = { ...GRANT, realpath: projectRoot, grantVersion: 4 };
    const decision = await authorizeOperation(request({ grantVersion: 3 }), sourceOf(grant));
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('CONFLICT');
  });

  it('缺少能力时被拒绝', async () => {
    const grant = { ...GRANT, realpath: projectRoot, capabilities: ['read'] as const };
    const decision = await authorizeOperation(
      request({ requiredCapabilities: ['read', 'write'] }),
      sourceOf(grant),
    );
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('PERMISSION_DENIED');
  });

  it('绝对路径注入被拒绝', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(
      request({ targetPath: '/etc/passwd' }),
      sourceOf(grant),
    );
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });

  it('.. 穿越被拒绝', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(
      request({ targetPath: '../proj-other/secret.txt' }),
      sourceOf(grant),
    );
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });

  it('symlink 逃逸出根目录被拒绝', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(
      request({ targetPath: 'src/linked.txt' }),
      sourceOf(grant),
    );
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });

  it('同名前缀的兄弟目录不被当作根目录内部', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(
      request({ targetPath: 'escape/secret.txt' }),
      sourceOf(grant),
    );
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });

  it('尚不存在但父目录已逃逸的路径被拒绝', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(
      request({ targetPath: 'escape/new-file.txt' }),
      sourceOf(grant),
    );
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });

  it('尚不存在但仍在根目录内的路径被放行', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(
      request({ targetPath: 'src/brand-new.ts' }),
      sourceOf(grant),
    );
    expect(decision).toEqual({ allowed: true, riskLevel: 'R0' });
  });

  it('空目标路径被拒绝', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(request({ targetPath: '' }), sourceOf(grant));
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });

  it('目标是文件下面的子路径时不可解析并被拒绝', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(
      request({ targetPath: 'src/app.ts/child.txt' }),
      sourceOf(grant),
    );
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });

  it('目标与父目录都不存在时被拒绝', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(
      request({ targetPath: 'missing-dir/file.txt' }),
      sourceOf(grant),
    );
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });

  it('授权根目录不可解析时被拒绝', async () => {
    const grant = { ...GRANT, realpath: join(workspace, 'gone') };
    const decision = await authorizeOperation(request(), sourceOf(grant));
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });

  it('R2 在没有可信审批界面时返回 APPROVAL_REQUIRED', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(request({ riskLevel: 'R2' }), sourceOf(grant));
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('APPROVAL_REQUIRED');
  });

  it('R2 在有可信审批界面时放行', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(request({ riskLevel: 'R2' }), sourceOf(grant), true);
    expect(decision).toEqual({ allowed: true, riskLevel: 'R2' });
  });

  it('R3 即使有可信审批界面也拒绝', async () => {
    const grant = { ...GRANT, realpath: projectRoot };
    const decision = await authorizeOperation(request({ riskLevel: 'R3' }), sourceOf(grant), true);
    expect(decision.allowed).toBe(false);
    if (decision.allowed) {
      throw new Error('expected denial');
    }
    expect(decision.code).toBe('POLICY_DENIED');
  });
});
