# LinxBox

通过 AI Passport 和灵矽 AI 发起语音编码任务，由 Cloud MCP Gateway 将任务路由到用户的 LinxBox Desktop，Desktop 再通过 Codex app-server 在已授权的本地项目中执行。

## 首个 MVP

使用 AI Passport 和灵矽 AI 创建、查询、续接和停止本地编码任务。硬件端可浏览任务、摘要、分页回复、最近历史和受限图片，确认下一轮输入并查看分阶段回执；Desktop 负责项目授权以及完整 diff、验证和本地细节审阅。

交互链路：**AI Passport/ESP32 → 灵矽 AI → Cloud MCP Gateway → LinxBox Desktop → Codex app-server → 本地项目**。

运行范围为预绑定的 AI Passport/ESP32、一台 macOS 电脑、一个 Desktop client token 和本机 Codex 认证。Desktop 只主动建立出站 WSS；Gateway 不访问本地文件系统，Codex 凭据、绝对路径、完整 diff 和原始日志默认不离开电脑。

本阶段聚焦六个 MCP 控制工具、独立的 Device Presentation 通道、Gateway/Desktop 可恢复投递、本地项目授权、Codex app-server 生命周期和真实端到端验收。产品账号、自助配对、完整远程 IDE、语音审批 push/发布/部署不在 MVP 内。

## 工程状态

目前已冻结产品范围、逻辑模块、pnpm/TypeScript/Electron/PostgreSQL/SQLite 技术基线、代码树、Desktop UI DOM/组件树、MCP 契约、Gateway/Desktop 协议、任务状态机和 Codex app-server 适配边界，但尚未提供可运行实现。具体框架、adapter 库、UI 渲染库和测试运行器由各 bootstrap Task/ADR 一次性选定并锁版。

后续业务代码需同时提供可复现的安装、配置、格式检查、静态检查、构建、测试及覆盖率命令，并接入 CI。本阶段只有文档及协作模板，没有业务构建或测试命令，也未启用 CI 或合并保护。

## 模块职责

| 模块 | 职责 |
| --- | --- |
| `device` 设备接入 | 三键任务阅读、图片、输入确认、回执、恢复和真机验收 |
| `linx` 灵矽 AI | 意图澄清、MCP 控制、Presentation Bridge 和设备路由 |
| `gateway` Cloud MCP Gateway | 入口认证、Task/Outbox、Desktop WSS、设备投影、路由和审计 |
| `desktop` Desktop UI/Daemon | 本地项目授权、去重、策略、任务所有权、UI 和恢复 |
| `codex` app-server 适配 | app-server 进程、thread/turn、事件映射、取消和终态 |
| `contracts` 契约与领域 | MCP、Gateway/Desktop、状态机、结果 schema 和 fixtures |
| `quality` 集成与质量 | 端到端验收、契约测试、CI 和使用文档 |

详细边界、主负责人和跨模块评审规则见 [OWNERS.md](OWNERS.md)。物理代码目录由首个 bootstrap Task 按已接受的目录树创建；客户端操作细节与 MCP 工具契约分离。

## 参与开发

- [AGENTS.md](AGENTS.md)：Claude Code、Codex 等 Agent 的工作要求。
- [CODING_STANDARDS.md](CODING_STANDARDS.md)：代码组织、输入与错误处理、测试及覆盖率要求。
- [CONTRIBUTING.md](CONTRIBUTING.md)：Issue、分支、Commit、PR 和审查流程。
- [OWNERS.md](OWNERS.md)：模块边界、负责人矩阵和跨模块协作规则。
- [Issue 模板](.github/ISSUE_TEMPLATE/)及 [PR 模板](.github/pull_request_template.md)：后续协作入口。

## 需求与契约

- 当前 MVP 的产品范围、架构与整体验收以 [最终技术架构](docs/architecture.md) 及其引用契约为准。[Proposal #1](https://github.com/KevinHuo/LinxBox/issues/1) 保留原始讨论，不作为当前实现契约，本次规范调整不修改其内容。
- 每次业务实现或 Bug 修复以一个明确的 Task/Bug Issue 为交付契约；开发 Issue 必须关联对应 Proposal，列出适用的接口契约或 ADR，并写明范围、非目标和验收条件。
- 开发前必须读取 [模块规范](docs/modules.md)、[端到端串联](docs/end-to-end.md)、[代码与 Desktop UI 结构](docs/code-structure.md)、[契约索引](docs/contracts/README.md)和[质量门禁](docs/quality-gates.md)，不得重新发明已冻结的边界。
- [AGENTS.md](AGENTS.md) 定义 AI 如何定位当前任务、读取契约及处理来源冲突。

初始化任务由 [Issue #2](https://github.com/KevinHuo/LinxBox/issues/2) 跟踪。初始化完成后，优先开展关键链路验证。

开发规范参考 [llgo](https://github.com/xgo-dev/llgo) 的可执行验证、测试和 CI 实践，但不声称与其规则完全一致。借鉴项与 LinxBox 自有决策的边界见 [CODING_STANDARDS.md](CODING_STANDARDS.md#参考实践与本项目决策)。
