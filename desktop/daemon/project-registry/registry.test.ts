import { mkdir, mkdtemp, realpath, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterAll, beforeAll, describe, expect, it } from 'vitest';

import {
  ProjectRegistry,
  ProjectRegistryError,
  isProjectCapability,
  resolveProjectRoot,
  toPublicProjectView,
} from './registry.js';

let workspace = '';
let realRoot = '';
let linkRoot = '';
/** macOS 上 `/var` 是 `/private/var` 的符号链接，canonical 路径与传入路径不同。 */
let canonicalRealRoot = '';

beforeAll(async () => {
  workspace = await mkdtemp(join(tmpdir(), 'linxbox-registry-'));
  realRoot = join(workspace, 'real');
  linkRoot = join(workspace, 'link');
  await mkdir(realRoot, { recursive: true });
  await symlink(realRoot, linkRoot, 'dir');
  canonicalRealRoot = await realpath(realRoot);
});

afterAll(async () => {
  await rm(workspace, { recursive: true, force: true });
});

describe('resolveProjectRoot', () => {
  it('返回 canonical realpath', async () => {
    await expect(resolveProjectRoot(linkRoot)).resolves.toBe(canonicalRealRoot);
  });

  it('不存在的目录被拒绝', async () => {
    await expect(resolveProjectRoot(join(workspace, 'missing'))).rejects.toThrow(
      ProjectRegistryError,
    );
  });

  it('符号链接环被拒绝', async () => {
    const loopA = join(workspace, 'loop-a');
    const loopB = join(workspace, 'loop-b');
    await symlink(loopA, loopB, 'dir');
    await symlink(loopB, loopA, 'dir');
    await expect(resolveProjectRoot(loopA)).rejects.toThrow(ProjectRegistryError);
  });

  it('文件不是目录时被拒绝', async () => {
    const filePath = join(workspace, 'file.txt');
    await writeFile(filePath, 'x', 'utf8');
    await expect(resolveProjectRoot(filePath)).rejects.toThrow(ProjectRegistryError);
  });
});

describe('ProjectRegistry', () => {
  it('首次注册得到版本 1 与 canonical realpath', async () => {
    const registry = new ProjectRegistry();
    const grant = await registry.register({
      projectId: 'prj_01H',
      rootPath: linkRoot,
      displayName: '商城前端',
      capabilities: ['read', 'write'],
    });
    expect(grant.grantVersion).toBe(1);
    expect(grant.realpath).toBe(canonicalRealRoot);
    expect(grant.revokedAt).toBeNull();
    expect(grant.capabilities).toEqual(['read', 'write']);
  });

  it('重复注册会更新内容并自增版本', async () => {
    const registry = new ProjectRegistry();
    await registry.register({
      projectId: 'prj_01H',
      rootPath: realRoot,
      displayName: '商城前端',
      capabilities: ['read'],
    });
    const updated = await registry.register({
      projectId: 'prj_01H',
      rootPath: realRoot,
      displayName: '商城前端 2',
      capabilities: ['read', 'test'],
    });
    expect(updated.grantVersion).toBe(2);
    expect(updated.displayName).toBe('商城前端 2');
    expect(updated.capabilities).toEqual(['read', 'test']);
  });

  it('撤销会写入 revokedAt 并自增版本', async () => {
    const registry = new ProjectRegistry();
    await registry.register({
      projectId: 'prj_01H',
      rootPath: realRoot,
      displayName: '商城前端',
      capabilities: ['read'],
    });
    const revoked = registry.revoke('prj_01H', '2026-10-24T01:00:00Z');
    expect(revoked?.revokedAt).toBe('2026-10-24T01:00:00Z');
    expect(revoked?.grantVersion).toBe(2);
  });

  it('撤销未知项目返回 undefined', () => {
    const registry = new ProjectRegistry();
    expect(registry.revoke('prj_missing', '2026-10-24T01:00:00Z')).toBeUndefined();
  });

  it('已撤销的项目无法解析', async () => {
    const registry = new ProjectRegistry();
    await registry.register({
      projectId: 'prj_01H',
      rootPath: realRoot,
      displayName: '商城前端',
      capabilities: ['read'],
    });
    registry.revoke('prj_01H', '2026-10-24T01:00:00Z');
    expect(registry.resolve('prj_01H')).toBeUndefined();
    expect(registry.get('prj_01H')?.revokedAt).toBe('2026-10-24T01:00:00Z');
  });

  it('未知项目无法解析', () => {
    const registry = new ProjectRegistry();
    expect(registry.resolve('prj_missing')).toBeUndefined();
    expect(registry.get('prj_missing')).toBeUndefined();
  });

  it('重新注册视为新的显式授权并清除撤销标记', async () => {
    const registry = new ProjectRegistry();
    await registry.register({
      projectId: 'prj_01H',
      rootPath: realRoot,
      displayName: '商城前端',
      capabilities: ['read'],
    });
    registry.revoke('prj_01H', '2026-10-24T01:00:00Z');
    const reRegistered = await registry.register({
      projectId: 'prj_01H',
      rootPath: realRoot,
      displayName: '商城前端',
      capabilities: ['read'],
    });
    expect(reRegistered.revokedAt).toBeNull();
    expect(reRegistered.grantVersion).toBe(3);
    expect(registry.resolve('prj_01H')).toBeDefined();
  });

  it('list 返回全部记录', async () => {
    const registry = new ProjectRegistry();
    await registry.register({
      projectId: 'prj_a',
      rootPath: realRoot,
      displayName: 'A',
      capabilities: ['read'],
    });
    await registry.register({
      projectId: 'prj_b',
      rootPath: realRoot,
      displayName: 'B',
      capabilities: ['read'],
    });
    expect(registry.list()).toHaveLength(2);
  });
});

describe('toPublicProjectView', () => {
  it('不包含任何本机路径', async () => {
    const registry = new ProjectRegistry();
    const grant = await registry.register({
      projectId: 'prj_01H',
      rootPath: realRoot,
      displayName: '商城前端',
      capabilities: ['read', 'write'],
    });
    const view = toPublicProjectView(grant);
    expect(view.project_id).toBe('prj_01H');
    expect(view.display_name).toBe('商城前端');
    expect(view.availability).toBe('online');
    expect(view.grant_version).toBe(1);
    expect(JSON.stringify(view)).not.toContain(canonicalRealRoot);
  });

  it('已撤销的项目显示为离线', async () => {
    const registry = new ProjectRegistry();
    await registry.register({
      projectId: 'prj_01H',
      rootPath: realRoot,
      displayName: '商城前端',
      capabilities: ['read'],
    });
    const revoked = registry.revoke('prj_01H', '2026-10-24T01:00:00Z');
    expect(toPublicProjectView(revoked!).availability).toBe('offline');
  });
});

describe('isProjectCapability', () => {
  it('识别契约定义的能力', () => {
    expect(isProjectCapability('read')).toBe(true);
    expect(isProjectCapability('write')).toBe(true);
    expect(isProjectCapability('test')).toBe(true);
  });

  it('拒绝未知能力', () => {
    expect(isProjectCapability('deploy')).toBe(false);
  });
});
