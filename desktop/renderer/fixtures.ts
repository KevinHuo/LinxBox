/**
 * Local mock data for the renderer.
 *
 * Used only when the page is opened outside Electron, so the information
 * architecture can be reviewed in a plain browser. Nothing here is a claim
 * about a real Gateway, a real project or a real task.
 */
import type { ConnectionFacts, DiagnosticsFacts, ProjectFacts, TaskFacts } from './view-model.js';

export const mockConnection: ConnectionFacts = {
  configured: false,
  connected: false,
  gatewayUrl: null,
  tokenHint: null,
  computerName: null,
  activeProfile: 'online',
  profiles: {
    local: {
      gatewayUrl: 'ws://127.0.0.1:8787/v1/clients/connect',
      tokenHint: 'demo…oken',
    },
    online: {
      gatewayUrl: 'ws://124.220.162.10:8787/v1/clients/connect',
      tokenHint: null,
    },
  },
};

export const mockProjects: readonly ProjectFacts[] = [
  {
    projectId: 'prj_local_1',
    displayName: '商城前端',
    rootPath: '/Users/demo/Projects/storefront',
    capabilities: ['read', 'write', 'test'],
    grantVersion: 1,
    enabled: true,
  },
  {
    projectId: 'prj_local_2',
    displayName: '订单服务',
    rootPath: '/Users/demo/Projects/order-api',
    capabilities: ['read'],
    grantVersion: 2,
    enabled: false,
  },
];

export const mockTasks: readonly TaskFacts[] = [
  {
    taskId: 'tsk_84b2',
    title: '修复登录页空值错误',
    projectId: 'prj_local_1',
    state: 'running',
    stage: 'testing',
    updatedAt: '今天 14:29',
    needsAttention: false,
  },
  {
    taskId: 'tsk_612a',
    title: '解释缓存失效策略',
    projectId: 'prj_local_1',
    state: 'succeeded',
    stage: 'finished',
    updatedAt: '今天 13:08',
    needsAttention: false,
  },
  {
    taskId: 'tsk_2c11',
    title: '补充订单状态测试',
    projectId: 'prj_local_2',
    state: 'interrupted',
    stage: 'finished',
    updatedAt: '昨天 18:22',
    needsAttention: true,
  },
];

export const mockDiagnostics: DiagnosticsFacts = {
  desktopVersion: '0.1.0',
  protocolVersions: ['1.0'],
  databasePath: null,
  connected: false,
  lastError: null,
};
