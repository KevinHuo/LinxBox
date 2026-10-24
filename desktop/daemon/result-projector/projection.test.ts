import { describe, expect, it } from 'vitest';

import type { LocalExecutionFacts } from './projection.js';
import {
  ProjectionBoundaryError,
  buildTaskResult,
  containsAbsolutePath,
  containsPathTraversal,
  isAbsolutePathLike,
  looksLikeCredential,
  projectCloudSummary,
  projectDeviceContent,
  projectLocalDetail,
  sanitizeVoiceSummary,
  truncateVoiceSummary,
} from './projection.js';

const ROOT = '/Users/dev/projects/shop';

const BASE_FACTS: LocalExecutionFacts = {
  taskId: 'tsk_01H',
  status: 'succeeded',
  summary: '修复登录表单空值错误',
  projectRoot: ROOT,
  changes: [
    { path: `${ROOT}/src/login.ts`, description: '提交前收窄可空字段' },
    { path: 'src/login.test.ts', description: '补充空值提交用例' },
  ],
  verifications: [
    { command: 'npm test -- login', outcome: 'passed', summary: '12 tests passed', executed: true },
  ],
  risks: [],
  needsUserAction: null,
  voiceSummary: '登录错误已修复，相关测试通过。',
  diff: '--- a/src/login.ts\n+++ b/src/login.ts\n@@\n-const v = x\n+const v = x ?? null',
  stdout: 'raw stdout with /Users/dev/secret path',
  stderr: 'raw stderr',
  providerReasoning: '内部推理内容',
  environment: { SECRET_TOKEN: 'value' },
};

describe('projectLocalDetail', () => {
  it('保留完整 diff、原始输出、绝对路径与内部推理', () => {
    const detail = projectLocalDetail(BASE_FACTS);
    expect(detail.kind).toBe('local_detail');
    expect(detail.diff).toContain('+++ b/src/login.ts');
    expect(detail.verification[0]?.stdout).toContain('raw stdout');
    expect(detail.verification[0]?.stderr).toBe('raw stderr');
    expect(detail.providerReasoning).toBe('内部推理内容');
    expect(detail.environment).toEqual({ SECRET_TOKEN: 'value' });
    expect(detail.changes[0]?.absolutePath).toBe(`${ROOT}/src/login.ts`);
    expect(detail.changes[0]?.relativePath).toBe('src/login.ts');
  });

  it('越界路径在本地详情里没有相对路径', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      changes: [{ path: '/etc/passwd', description: '越界' }],
    };
    const detail = projectLocalDetail(facts);
    expect(detail.changes[0]?.relativePath).toBeNull();
  });

  it('缺失的可选字段归一化为 null', () => {
    const facts: LocalExecutionFacts = {
      taskId: 'tsk_01H',
      status: 'failed',
      summary: '失败',
      projectRoot: ROOT,
      changes: [],
      verifications: [],
      risks: [],
      needsUserAction: '请手动确认',
      voiceSummary: '执行失败',
    };
    const detail = projectLocalDetail(facts);
    expect(detail.diff).toBeNull();
    expect(detail.providerReasoning).toBeNull();
    expect(detail.environment).toBeNull();
    expect(detail.needsUserAction).toEqual({ summary: '请手动确认' });
  });
});

describe('projectCloudSummary', () => {
  it('不包含 diff、原始输出、内部推理与环境变量', () => {
    const cloud = projectCloudSummary(BASE_FACTS);
    const serialised = JSON.stringify(cloud);
    expect(serialised).not.toContain('+++ b/src/login.ts');
    expect(serialised).not.toContain('raw stdout');
    expect(serialised).not.toContain('raw stderr');
    expect(serialised).not.toContain('内部推理内容');
    expect(serialised).not.toContain('SECRET_TOKEN');
  });

  it('changes 的路径是相对路径', () => {
    const cloud = projectCloudSummary(BASE_FACTS);
    expect(cloud.changes.map((change) => change.path)).toEqual([
      'src/login.ts',
      'src/login.test.ts',
    ]);
  });

  it('越界的改动被丢弃', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      changes: [{ path: '/etc/passwd', description: '越界' }],
    };
    const cloud = projectCloudSummary(facts);
    expect(cloud.changes).toHaveLength(0);
  });

  it('包含 .. 的改动被丢弃', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      changes: [{ path: '../other/secret.ts', description: '穿越' }],
    };
    const cloud = projectCloudSummary(facts);
    expect(cloud.changes).toHaveLength(0);
  });

  it('证据不足时不声称 succeeded', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      verifications: [
        { command: 'npm test', outcome: 'passed', summary: 'claimed', executed: false },
      ],
    };
    const cloud = projectCloudSummary(facts);
    expect(cloud.status).toBe('interrupted');
    expect(cloud.verification[0]?.outcome).toBe('not_run');
  });

  it('描述里出现绝对路径时大声失败', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      changes: [
        { path: `${ROOT}/src/login.ts`, description: '修改 /Users/dev/projects/shop/src/login.ts' },
      ],
    };
    expect(() => projectCloudSummary(facts)).toThrow(ProjectionBoundaryError);
  });

  it('以斜杠开头的路径同样被拦截', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      changes: [
        { path: `${ROOT}/src/login.ts`, description: '/Users/dev/projects/shop/src/login.ts' },
      ],
    };
    expect(() => projectCloudSummary(facts)).toThrow(ProjectionBoundaryError);
  });

  it('描述里出现路径穿越时大声失败', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      changes: [{ path: `${ROOT}/src/login.ts`, description: '参考 ../other-project 的实现' }],
    };
    expect(() => projectCloudSummary(facts)).toThrow(ProjectionBoundaryError);
  });

  it('命令里出现凭据时大声失败', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      verifications: [
        {
          command: 'npm config set token ghp_abcdefghijklmnopqrstuvwxyz012345',
          outcome: 'passed',
          summary: 'ok',
          executed: true,
        },
      ],
    };
    expect(() => projectCloudSummary(facts)).toThrow(ProjectionBoundaryError);
  });

  it('风险条目被投影成脱敏的风险列表', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      risks: [{ code: 'dependency_drift', summary: '锁文件与依赖声明不一致' }],
    };
    const cloud = projectCloudSummary(facts);
    expect(cloud.risks).toEqual([{ code: 'dependency_drift', summary: '锁文件与依赖声明不一致' }]);
  });

  it('验证存在但没有原始输出时归一化为 null', () => {
    const facts: LocalExecutionFacts = {
      taskId: 'tsk_01H',
      status: 'succeeded',
      summary: '完成',
      projectRoot: ROOT,
      changes: [],
      verifications: [
        { command: 'npm test', outcome: 'passed', summary: '1 test passed', executed: true },
      ],
      risks: [],
      needsUserAction: null,
      voiceSummary: '已完成',
    };
    const detail = projectLocalDetail(facts);
    expect(detail.verification[0]?.stdout).toBeNull();
    expect(detail.verification[0]?.stderr).toBeNull();
  });

  it('未真实执行的验证不能是 passed', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      verifications: [
        { command: 'npm test', outcome: 'passed', summary: 'claimed', executed: false },
      ],
    };
    const cloud = projectCloudSummary(facts);
    expect(cloud.verification[0]?.outcome).toBe('not_run');
  });

  it('未真实执行的失败验证降级为 not_run', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      verifications: [
        { command: 'npm test', outcome: 'failed', summary: 'claimed', executed: false },
      ],
    };
    const cloud = projectCloudSummary(facts);
    expect(cloud.verification[0]?.outcome).toBe('not_run');
  });

  it('非法 outcome 收敛为 not_run', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      verifications: [
        {
          command: 'npm test',
          outcome: 'flaky' as 'passed',
          summary: 'claimed',
          executed: true,
        },
      ],
    };
    const cloud = projectCloudSummary(facts);
    expect(cloud.verification[0]?.outcome).toBe('not_run');
  });
});

describe('projectDeviceContent', () => {
  it('是云端摘要的子集', () => {
    const cloud = projectCloudSummary(BASE_FACTS);
    const device = projectDeviceContent(BASE_FACTS);
    expect(device.kind).toBe('device_content');
    expect(device.taskId).toBe(cloud.taskId);
    expect(device.status).toBe(cloud.status);
    expect(device.summary).toBe(cloud.summary);
    expect(device.voiceSummary).toBe(cloud.voiceSummary);
  });

  it('不包含改动路径、命令与风险明细', () => {
    const device = projectDeviceContent(BASE_FACTS);
    const serialised = JSON.stringify(device);
    expect(serialised).not.toContain('src/login.ts');
    expect(serialised).not.toContain('npm test');
  });
});

describe('buildTaskResult', () => {
  it('产出符合 task-result-v1 的结果', () => {
    const result = buildTaskResult(BASE_FACTS);
    expect(result.contract_version).toBe('task-result-v1');
    expect(result.status).toBe('succeeded');
    expect(result.changes[0]?.path).toBe('src/login.ts');
    expect(result.verification[0]?.outcome).toBe('passed');
    expect(result.needs_user_action).toBeNull();
  });

  it('changes 的路径不含绝对路径与 ..', () => {
    const result = buildTaskResult(BASE_FACTS);
    for (const change of result.changes) {
      expect(change.path.startsWith('/')).toBe(false);
      expect(change.path).not.toContain('..');
    }
  });

  it('越界改动被丢弃且状态不再声称成功', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      changes: [{ path: '/etc/passwd', description: '越界' }],
    };
    const result = buildTaskResult(facts);
    expect(result.changes).toHaveLength(0);
    expect(result.status).toBe('interrupted');
  });

  it('未知状态收敛为 interrupted', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      status: 'exploded' as unknown as LocalExecutionFacts['status'],
    };
    const result = buildTaskResult(facts);
    expect(result.status).toBe('interrupted');
  });

  it('voice_summary 超过长度时被确定性截断', () => {
    const facts: LocalExecutionFacts = { ...BASE_FACTS, voiceSummary: '登'.repeat(200) };
    const result = buildTaskResult(facts);
    expect(Array.from(result.voice_summary).length).toBeLessThanOrEqual(120);
    expect(result.voice_summary.endsWith('…')).toBe(true);
  });

  it('voice_summary 内的路径被遮蔽', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      voiceSummary: '修改了 /Users/dev/projects/shop/src/login.ts',
    };
    const result = buildTaskResult(facts);
    expect(result.voice_summary).not.toContain('/Users/dev');
    expect(result.voice_summary).toContain('[路径]');
  });

  it('voice_summary 内的凭据被遮蔽', () => {
    const facts: LocalExecutionFacts = {
      ...BASE_FACTS,
      voiceSummary: 'token 是 ghp_abcdefghijklmnopqrstuvwxyz012345',
    };
    const result = buildTaskResult(facts);
    expect(result.voice_summary).not.toContain('ghp_abcdefghijklmnopqrstuvwxyz012345');
    expect(result.voice_summary).toContain('[凭据]');
  });
});

describe('sanitizeVoiceSummary', () => {
  it('去掉代码块', () => {
    expect(sanitizeVoiceSummary('前 ```const a = 1;``` 后')).toBe('前 后');
  });

  it('去掉 diff 行', () => {
    expect(sanitizeVoiceSummary('说明\n+++ b/src/login.ts\n结束')).toBe('说明 结束');
  });

  it('去掉堆栈帧', () => {
    expect(sanitizeVoiceSummary('说明\n    at foo (bar.ts:1:1)\n结束')).toBe('说明 结束');
  });

  it('去掉 Traceback 行', () => {
    expect(sanitizeVoiceSummary('Traceback (most recent call last)\n结束')).toBe('结束');
  });

  it('压缩空白', () => {
    expect(sanitizeVoiceSummary('  多   余空白  ')).toBe('多 余空白');
  });
});

describe('truncateVoiceSummary', () => {
  it('短文本原样返回', () => {
    expect(truncateVoiceSummary('短')).toBe('短');
  });

  it('长文本截断到上限', () => {
    const truncated = truncateVoiceSummary('字'.repeat(300));
    expect(Array.from(truncated).length).toBe(120);
  });
});

describe('isAbsolutePathLike', () => {
  it('识别绝对路径形状', () => {
    expect(isAbsolutePathLike('/a/b')).toBe(true);
  });

  it('不把相对路径当成绝对路径', () => {
    expect(isAbsolutePathLike('src/login.ts')).toBe(false);
    expect(isAbsolutePathLike('/')).toBe(false);
  });
});

describe('containsAbsolutePath', () => {
  it('识别开头的绝对路径', () => {
    expect(containsAbsolutePath('/Users/dev/x.ts')).toBe(true);
  });

  it('识别嵌入文本中间的绝对路径', () => {
    expect(containsAbsolutePath('修改 /Users/dev/x.ts')).toBe(true);
  });

  it('不把相对路径或 URL 当成绝对路径', () => {
    expect(containsAbsolutePath('src/login.ts')).toBe(false);
    expect(containsAbsolutePath('见 https://example.com/docs')).toBe(false);
  });
});

describe('containsPathTraversal', () => {
  it('识别路径穿越', () => {
    expect(containsPathTraversal('../secrets')).toBe(true);
    expect(containsPathTraversal('a..b')).toBe(false);
  });
});

describe('looksLikeCredential', () => {
  it('识别常见凭据形状', () => {
    expect(looksLikeCredential('ghp_abcdefghijklmnopqrstuvwxyz012345')).toBe(true);
    expect(looksLikeCredential('AKIAIOSFODNN7EXAMPLE')).toBe(true);
  });

  it('普通文本不是凭据', () => {
    expect(looksLikeCredential('npm test -- login')).toBe(false);
  });
});
