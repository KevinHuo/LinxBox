# AI 开发与质量门禁

> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24

本规范参考 [`qbox/miku-rta` 的 AGENTS.md](https://github.com/qbox/miku-rta/blob/main/AGENTS.md)、[Spec 流程](https://github.com/qbox/miku-rta/blob/main/docs/specs/README.md)、[Spec 模板](https://github.com/qbox/miku-rta/blob/main/docs/specs/template.zh-CN.md)、[PR 模板](https://github.com/qbox/miku-rta/blob/main/.github/pull_request_template.md)和 [CI](https://github.com/qbox/miku-rta/blob/main/.github/workflows/ci.yml)。借鉴的是权威分层、bounded Spec、具名责任、证据矩阵和发布门禁；不照搬其 Go/Mage、组织角色、工单前缀或基础设施。

## 1. 权威层级

1. `docs/architecture.md`：部署、事实源和跨模块边界；
2. `docs/contracts/` 与 Accepted ADR：公共行为、不变量和技术决策；
3. `docs/modules.md`：模块责任和禁止项；
4. Accepted execution Spec：一个可实施 slice；
5. Task Issue：执行跟踪，不能静默改上层契约；
6. 代码与测试：已实现事实，不反向覆盖规范。

GitHub Issue 不因内容更旧而自动修改；发现差异时在交付说明中指出，只有用户明确要求时才更新远端 Issue。

## 2. 哪些变更必须有 Spec

新增或改变公共/内部协议、状态权威、持久化 schema、幂等/恢复语义、运行时职责、部署边界、安全权限、跨模块行为或发布门禁时必须使用 [`docs/specs/template.md`](specs/template.md)。小型文档、测试和不改变行为的重构可写 `Spec: N/A` 并解释。

一个 Spec 只覆盖一个主模块中的一个 bounded slice。跨模块 Spec 必须列 producer、consumer、一个 DRI、必要 Reviewer 和 Integration Owner。

## 3. Spec 状态

| 状态 | 含义 | 是否允许实现 |
| --- | --- | --- |
| `Proposed` | 评审输入，TBD/阻塞项明确 | 否 |
| `Accepted` | DRI、决策、契约和必要评审已冻结 | 仅契约范围内 |
| `Implementing` | 已接受范围正在实现 | 是 |
| `Implemented` | 实现及约定验收证据完整 | 已完成 |
| `Superseded` | 被具名文档替代 | 否 |

作者自审、代码写完、状态字段修改或 PR 合并都不能单独使 Spec Accepted/Implemented。合并授权也不等于功能启用或发布授权。

## 4. 合并门禁

- Spec/Task/PR 的目标、非目标和验收条件一致；
- 公共 schema 有正反例 fixture、producer/consumer contract tests 和兼容说明；
- 格式、lint/typecheck、构建、单元测试、增量及关键模块覆盖率通过；
- 安全边界包含越权、路径逃逸、重放、乱序、payload 上限和脱敏负例；
- 模拟、真实集成、未执行项分别报告；
- 文档链接、示例、生成来源、许可证和敏感信息检查通过；
- 必要 DRI、模块 Reviewer 和跨边界 Reviewer 已记录。

## 5. 功能启用与发布门禁

默认关闭的新通道不能因代码合并自动启用。启用前需要目标环境兼容性、迁移/回滚、真实依赖、容量水位、指标/告警/runbook、安全审查和 owner 签字。Device 链路还必须有真实灵矽与真机屏幕/按键/资源/soak 证据。

## 6. 证据分级

报告必须分别列出 `docs/schema`、`unit`、`contract`、`integration`、`fault/security`、`simulated-e2e`、`real-e2e`、`device-soak`。任何较低等级不能替代较高等级；生成固件不等于刷写成功，桌面截图不等于真机可读，mock app-server 不等于真实 Codex 可用。
