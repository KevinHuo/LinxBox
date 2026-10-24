# Issue、Commit 与 PR 规范

代码质量要求见 [CODING_STANDARDS.md](CODING_STANDARDS.md)，Agent 工作要求见 [AGENTS.md](AGENTS.md)。

## 开始开发

首个 MVP 的范围和验收标准由 [Proposal #1](https://github.com/KevinHuo/LinxBox/issues/1) 跟踪。后续任务拆成独立 Issue，并关联 Proposal；工程不重复保存提案正文或内部启动材料。

从最新默认分支创建工作分支，围绕任务开发、自查和提交 PR。文档修正等小改动可以直接提交 PR，写明任务和验收目标。

开发 Issue 需写明 DRI、主模块、受影响模块和必要 reviewer。当前项目分工见 [README.md](README.md)；未明确的职责不得自行指派。跨模块协议、状态机、持久化、安全边界或失败语义使用独立设计 Issue/PR 评审，不夹带在协作规范或无关实现 PR 中。设计稿不等于实现完成，也不替代真实集成证据。

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

type 和 scope 小写，半角冒号后一个空格，描述具体。scope 使用实际模块，例如 voice、mcp、app、project，不维护复杂白名单。

示例：

- `chore(project): 初始化工程与开发规范`
- `feat(mcp): 支持向当前 ChatGPT 会话发送问题`
- `fix(app): 避免追问进入新会话`

不兼容变化使用 `type(scope)!: 简述`，正文写明 `BREAKING CHANGE:`、影响及迁移；回退说明原提交和原因。

## Issue

使用 [Issue 模板](.github/ISSUE_TEMPLATE/)创建条目。

| 类型 | 标题格式 | 必须说明 |
| --- | --- | --- |
| 提案 | `[Proposal] 主题` | 目标、交付范围、任务拆分、整体验收条件 |
| 开发任务 | `[Task] 任务名称` | 关联 Proposal、目标、范围、依赖和可检查的验收条件 |
| 缺陷 | `[Bug] 问题简述` | 复现步骤、预期/实际结果、运行环境及必要的脱敏日志 |

一条 Issue 聚焦一个问题，需求变化和相关讨论在对应 Issue 中同步。Issue 前缀表达条目类型，PR 的 feat、fix、chore 等表达实际改动类型，两者不要求一一对应。

Proposal 中维护关联任务或子 Issue。开发任务完成后关闭对应 Issue；Proposal 在全部任务完成并通过 MVP 整体验收后关闭。

## PR

一个 PR 聚焦一个可独立评审的任务，使用 [PR 模板](.github/pull_request_template.md)。正文只需五项：

1. 任务：关联已有 Issue；没有 Issue 时直接写明任务和验收目标。
2. 责任边界：DRI、主模块、受影响模块和必要 reviewer。
3. 改动：解决的问题、行为变化及主要实现。
4. 验证：实际执行的命令/场景、结果及覆盖率；未验证项如实说明。
5. 限制：已知问题，必要的兼容影响及回退。

完整解决已有任务使用 `Closes #任务编号`，部分相关使用 `Refs #任务编号`；引用提案使用 `Refs #1`，避免合并单项任务时关闭整个 Proposal。

满足 CODING_STANDARDS.md 的检查要求，并由至少一名非作者 reviewer 批准后合并。未完成用 Draft；AI 不自批自合。

工作分支建议 `type/issue编号-short-description`，例如 `chore/2-bootstrap`。建议 Squash merge，最终标题保持规范。CI 和合并保护落实必要检查；项目尚未接入时不能声称已生效。

## 初始化检查

纯文档初始化检查文件编码、Markdown 相对链接、Issue 模板元信息、忽略规则及 `git diff --check`，无需业务构建、单元测试或覆盖率检查，不添加空测试和占位 CI。

开发语言和工具链确定后，将真实的安装、格式、静态检查、构建、测试及覆盖率命令补入 README.md，并在首批业务代码中接入 CI。
