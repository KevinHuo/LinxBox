# LinxBox 条目与文档骨架

以下骨架对应仓库模板原文（`.github/ISSUE_TEMPLATE/`、`docs/specs/template.md`）。章节名不要改写，便于跨条目对照。

## Task Issue（`.github/ISSUE_TEMPLATE/task.md`）

```markdown
## 关联

- 项目提案：Proposal #N 标题（回填编号与链接）
- 历史背景：Proposal #1（保留原始讨论，不作为当前实现契约）
- 主模块：`contracts`
- 受影响模块：`quality`（说明为什么受影响）
- 负责人：OWNERS.md 中的模块名；未指派时写明待认领

## 目标与范围

（当前缺口是什么，为什么现在做。一句话复述目标。）

（编号列出交付内容，每项对应一个可验收产物。）

### 非目标

- （显式排除项，防止后续 PR 夹带）

## 依赖与运行条件

- （前置任务 / 前置 Issue）
- （运行前提，无依赖时注明）
- （需要谁评审）

## 验收标准

- [ ] （可勾选、可核对的行为或状态）
- [ ] （文档/结构类：逐项一致的检查）

## 验证方式

- （实际执行的命令或场景，以及结果如何判定）
- （不适用项与原因，例如纯文档阶段无构建/测试命令）
```

### 验收条件写法

不可核对 → 可核对：

| 不可核对 | 可核对 |
| --- | --- |
| 功能正常 | 两个 device ID 只能路由到各自绑定的 Desktop |
| 契约一致 | `linxbox.mcp/v1`、`device-presentation/v1`、`task-domain/v1`、`task-result-v1`、Gateway/Desktop `1.0` 在四处文档完全一致 |
| 没有死链 | 仓库内相对链接逐条打开无 404（给出检查命令与结论） |
| 错误处理完善 | 未知字段、越权项目、路径逃逸、重复幂等键、乱序 sequence 各有反例 fixture |

## Bug Issue（`.github/ISSUE_TEMPLATE/bug.md`）

```markdown
## 关联

- 关联 Proposal / 开发 Issue：
- 主模块 / 受影响模块：
- 行为契约：（引用的具体契约章节，如 `docs/contracts/gateway-desktop.md` §6）

## 复现步骤

1. （可复现的最小步骤）

## 预期结果

## 实际结果

## 运行环境

- （OS / 版本 / 环境；真机、mock、模拟分别写明）

## 脱敏日志

（贴日志时去掉 token、绝对路径、凭据、内部推理）
```

## Proposal（`.github/ISSUE_TEMPLATE/proposal.md`）

```markdown
## 背景与目标

（为什么现在做这件事。指明与历史 Proposal 的差异及其原因。）

## 本次交付范围

（冻结内容的能力清单 + 运行前提 + 必要边界。）

### 明确不在本次范围

- （显式排除项）

## 实现思路

（模块职责、关键链路、待验证的技术选择。）

## 任务拆分

- [x] 已关联执行项：Task #N 标题（回填编号与链接）
- [ ] （后续 slice）

## 验收标准

- [ ] （整体验收条件，可核对）

## 待确认事项

- （需要维护者决策的问题 + 不决策时的安全默认）
```

Proposal 写完后保持打开，任务全部完成并通过整体验收后再关闭。

## Execution Spec（`docs/specs/template.md`）

15 节全部保留，不能因为内容为空就删节：

| 节 | 内容要求 |
| --- | --- |
| 头部表格 | Spec ID、状态、主模块/Slice、上游架构/契约/ADR、DRI（具体个人）、必要 Reviewer、Integration Owner、最近更新 |
| 1 定位、依赖和消费者 | 上游契约、下游消费者、依赖 Spec/PR、被替代内容、接受阻塞项 |
| 2 当前实现事实 | 只写代码、测试、配置和真实运行证据已证明的事实，未来设计不写在这里 |
| 3 目标 / 4 非目标 | 与 Task 的目标和非目标一致 |
| 5 权威边界与不变量 | 引用的不变量原文 |
| 6 组件、接口与版本策略 | 未冻结字段写 `TBD`，不用伪代码掩盖决策缺口 |
| 7 状态机与数据模型 | 合法转换、幂等键、sequence/revision、TTL、上限、迁移、清理 |
| 8 失败、降级与恢复 | 超时、重复、乱序、部分成功、依赖/进程/节点故障、恢复收敛，明确 fail-open/closed |
| 9 健康、容量与可观测性 | Readiness/Liveness、指标与稳定 Reason Code、水位与 Runbook Owner |
| 10 安全与隐私 | 禁传字段、路径约束、风险分级负例 |
| 11 兼容、启用、发布与回滚 | 版本演进、启用门禁、回滚路径 |
| 12 测试与验收证据 | 按证据分级逐行填，未跑写 `Not run` |
| 13 实现拆分 | 每个 PR slice 一行，含 DRI、Reviewer、依赖、输出 |
| 14 待决策与风险 | 每条含安全默认、决策人、最晚时间、状态 |
| 15 实现与验收记录 | 实现 PR、必要批准、最终验证、未覆盖风险 |

新增 Spec 后同步 `docs/specs/README.md` 索引表（Spec、状态、主模块、DRI、下一门禁）。未列入索引的草稿不授权实现。

## PR（`.github/pull_request_template.md` + `CONTRIBUTING.md` 六项）

```markdown
## 任务

Closes #任务编号        （完整解决）
Refs #提案编号          （引用提案，避免合并单项任务时关闭整个 Proposal）

## 契约对应

（表格：交付物 → 上游契约/ADR → 与 Issue 验收条件的对应关系）

## 模块影响

- 主模块：
- 受影响模块：
- 已请求的负责人评审：

## 改动

（解决了什么问题、行为变化、主要实现。）

## 验证

（实际执行的命令/场景、结果、覆盖率口径；模拟与真实联调分开记录；
  不适用项写不适用及原因。）

## 限制

（已知问题、兼容影响、回退方式。）
```

## Commit 标题

```text
feat(gateway): 支持创建可恢复的编码任务
fix(codex): 避免续接任务创建新 thread
docs(contracts): 落地仓库内 MVP 契约基线
chore(project): 建立 apps/packages/plugins/tests 目录骨架
```

`type` 取 `feat|fix|refactor|perf|test|docs|style|build|ci|chore|revert`；scope 取 `device|linx|gateway|desktop|codex|contracts|quality|project`。不兼容变化用 `type(scope)!: 简述`，正文写 `BREAKING CHANGE:`、影响及迁移。

分支名 `type/issue编号-short-description`，例如 `docs/5-contract-baseline`。建议 Squash merge。
