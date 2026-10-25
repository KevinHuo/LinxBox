import { describe, expect, it } from 'vitest';

import * as daemon from './index.js';

describe('daemon 公开接口', () => {
  it('导出 delivery 处理能力', () => {
    expect(typeof daemon.decideDelivery).toBe('function');
    expect(typeof daemon.hashPayload).toBe('function');
    expect(typeof daemon.canonicalJson).toBe('function');
    expect(typeof daemon.toDeliveryRecord).toBe('function');
  });

  it('导出任务状态机能力', () => {
    expect(typeof daemon.canTransition).toBe('function');
    expect(typeof daemon.transition).toBe('function');
    expect(typeof daemon.isTerminal).toBe('function');
    expect(typeof daemon.toDeviceState).toBe('function');
    expect(typeof daemon.localStateForProviderOutcome).toBe('function');
    expect(typeof daemon.resolveCancelRequested).toBe('function');
  });

  it('导出授权与项目注册表能力', () => {
    expect(typeof daemon.authorizeOperation).toBe('function');
    expect(typeof daemon.checkRiskLevel).toBe('function');
    expect(typeof daemon.ProjectRegistry).toBe('function');
    expect(typeof daemon.resolveProjectRoot).toBe('function');
  });

  it('导出三级投影能力', () => {
    expect(typeof daemon.projectLocalDetail).toBe('function');
    expect(typeof daemon.projectCloudSummary).toBe('function');
    expect(typeof daemon.projectDeviceContent).toBe('function');
    expect(typeof daemon.buildTaskResult).toBe('function');
  });

  it('导出入站帧校验与本地 schema 能力', () => {
    expect(typeof daemon.parseInboundFrame).toBe('function');
    expect(typeof daemon.buildHelloFrame).toBe('function');
    expect(typeof daemon.GatewayClient).toBe('function');
    expect(typeof daemon.TaskDispatcher).toBe('function');
    expect(typeof daemon.migrationsNeeded).toBe('function');
    expect(daemon.SCHEMA_VERSION).toBeGreaterThan(0);
  });
});
