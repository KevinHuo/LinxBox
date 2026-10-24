# Issue、Commit 与 PR 规范

代码质量要求见 [CODING_STANDARDS.md](CODING_STANDARDS.md)，Agent 工作要求见 [AGENTS.md](AGENTS.md)。

## 开始开发

首个 MVP 的当前实现范围和验收标准由仓库内 [docs/architecture.md](docs/architecture.md) 及其引用契约定义；[Proposal #1](https://github.com/KevinHuo/LinxBox/issues/1) 保留原始讨论和追踪信息。后续任务拆成独立 Task Issue，并关联对应 Proposal 与适用的仓库契约。

Task 或 Bug Issue 是单次开发的交付契约。在开始业务实现或 Bug 修复前，开发 Issue 必须明确：

1. 关联的 Proposal。
2. [OWNERS.md](OWNERS.md) 中的主模块、受影响模块和负责人。
3. 本次目标、范围和非目标。
4. 适用的接口契约、协议文档或 ADR；不适用时明确注明。
5. 可检查的验收条件和验证方式。
6. 适用的 Execution Spec、具体个人 DRI、必要 Reviewer 和 Integration Owner；`Spec: N/A` 时说明理由。

改变协议、状态权威、持久化、失败语义、运行时职责、安全边界或跨模块行为时，必须按 [质量门禁](docs/quality-gates.md)从[模板](docs/specs/template.md)创建 bounded Spec。`Proposed` 不授权实现；实现中发现开发 Issue 与已接受契约冲突时，先修改仓库契约、Spec 或 ADR，不通过代码静默改变行为，也不擅自修改 GitHub Issue。

从最新默认分支创建工作分支，围绕任务开发、自查和提交 PR。文档修正等小改动可以直接提交 PR，写明任务和验收目标。

## Commit 与 PR 标题

统一格式：`type(scope): 简述`。

| type | 用途 |
| --- | --- |
| feat | 新功能，统一用 feat，不用 feature |
| fix | Bug 修复及配套回归测试 |
| refactor | 不改变外部行为的重构 |
| perf | 性能优化 |
| test | 仅测试 |
| docs | 仅文档或注释 |
| style | 不影响语义的格式调整 |
| build | 构建和依赖 |
| ci | CI 与自动检查 |
| chore | 其他工程维护 |
| revert | 回退已有提交 |

type 和 scope 小写，半角冒号后一个空格，描述具体。scope 优先使用 `device`、`linx`、`gateway`、`desktop`、`codex`、`contracts` 或 `quality`；纯项目维护使用 `project`。

示例：

- `chore(project): 初始化工程与开发规范`
- `feat(gateway): 支持创建可恢复的编码任务`
- `fix(codex): 避免续接任务创建新 thread`

不兼容变化使用 `type(scope)!: 简述`，正文写明 `BREAKING CHANGE:`、影响及迁移；回退说明原提交和原因。

## Issue

使用 [Issue 模板](.github/ISSUE_TEMPLATE/)创建条目。

| 类型 | 标题格式 | 必须说明 |
| --- | --- | --- |
| 提案 | `[Proposal] 主题` | 目标、交付范围、任务拆分、整体验收条件 |
| 开发任务 | `[Task] 任务名称` | 关联 Proposal、主模块与受影响模块、目标、范围与非目标、契约或 ADR、依赖和可检查的验收条件 |
| 缺陷 | `[Bug] 问题简述` | 关联 Proposal、主模块与受影响模块、行为契约、复现步骤、预期/实际结果、运行环境及必要的脱敏日志 |

一条 Issue 聚焦一个问题，需求变化和相关讨论在对应 Issue 中同步。Issue 前缀表达条目类型，PR 的 feat、fix、chore 等表达实际改动类型，两者不要求一一对应。

Proposal 中维护关联任务或子 Issue。开发任务完成后关闭对应 Issue；Proposal 在全部任务完成并通过 MVP 整体验收后关闭。

## PR

一个 PR 聚焦一个可独立评审的任务，使用 [PR 模板](.github/pull_request_template.md)。正文必须包含：

1. 任务：业务实现和 Bug 修复必须关联已有开发 Issue；仅不改变行为的小型文档或项目维护可在 PR 中直接写明任务和验收目标。
2. 契约对应：列出适用的 Proposal、契约或 ADR，以及与开发 Issue 验收条件的对应关系。
3. 模块影响：列出主模块、受影响模块和已请求的负责人评审。
4. 改动：解决的问题、行为变化及主要实现。
5. 验证：实际执行的命令/场景、结果及覆盖率；未验证项如实说明。
6. 分级证据：文档/schema、单元/覆盖率、契约、集成、安全/故障、模拟和真实 E2E 分开报告。
7. 启用与回滚：安全默认、迁移/发布顺序、目标环境门禁和回滚。
8. 限制：已知问题、未验证项和必要的兼容影响。

完整解决已有任务使用 `Closes #任务编号`，部分相关使用 `Refs #任务编号`；引用提案使用 `Refs #1`，避免合并单项任务时关闭整个 Proposal。

满足 CODING_STANDARDS.md 的检查要求，并由至少一名非作者 reviewer 批准后合并。未完成用 Draft；AI 不自批自合。

工作分支建议 `type/issue编号-short-description`，例如 `chore/2-bootstrap`。建议 Squash merge，最终标题保持规范。CI 和合并保护落实必要检查；项目尚未接入时不能声称已生效。

## 初始化检查

纯文档初始化检查文件编码、Markdown 相对链接、Issue 模板元信息、忽略规则及 `git diff --check`，无需业务构建、单元测试或覆盖率检查，不添加空测试和占位 CI。

开发语言和工具链确定后，将真实的安装、格式、静态检查、构建、测试及覆盖率命令补入 README.md，并在首批业务代码中接入 CI。
