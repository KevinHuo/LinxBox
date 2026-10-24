# LinxBox 设计文档索引

本目录是 LinxBox 的设计事实来源，分两组：

**一、产品与契约基线**（评审与实现都要看）

| 文档 | 作用 |
| --- | --- |
| [最终技术架构](./architecture.md) | 总体架构、端到端链路、身份、协议、Codex 集成、安全、可靠性和实施路线 |
| [端到端串联](./end-to-end.md) | 语音、控制、展示与本地审阅四条链路的完整闭环与事实源划分 |
| [边界契约索引](./contracts/README.md) | MCP、Device Presentation、Gateway/Desktop、领域状态与结果契约；单一事实源表 |
| [模块规范](./modules.md) | 七个模块的职责、依赖方向、状态所有权与交付门禁 |
| [代码与 Desktop UI 结构](./code-structure.md) | 技术基线、目录树、分层规则和 Desktop UI DOM/组件树 |
| [灵矽 AI 行为规范](./ai-specification.md) | 灵矽 Agent、Gateway、Desktop 和 Codex 的行为与输出约束 |
| [决策记录](./adr/) | 已接受的架构决策（ADR-0001 ~ 0005） |

**二、执行规范**（给执行者与 AI 看）

| 目录 | 作用 |
| --- | --- |
| [process/](./process/README.md) | 工程设计规范、质量门禁、Execution Spec 流程与模板 |

已接受决策：

- [ADR-0001：MVP 使用网关预配置的设备—客户端绑定](./adr/0001-mvp-preconfigured-device-client-binding.md)
- [ADR-0002：Desktop 使用 Codex app-server 作为远程任务执行接口](./adr/0002-codex-app-server-local-execution.md)
- [ADR-0003：MVP 使用 TypeScript pnpm monorepo 与 Electron Desktop](./adr/0003-typescript-pnpm-electron-profile.md)
- [ADR-0004：去掉 `packages/` 与 `plugins/`，MVP 契约只以文档形态存在](./adr/0004-shared-contracts-as-plain-data.md)
- [ADR-0005：Hooks 只做被动观察，app-server 负责双向控制](./adr/0005-hooks-observation-app-server-control.md)

涉及公共 MCP、任务状态、身份授权、Desktop 协议、Linx progress 路由或 Codex 集成方式的变更，必须先更新契约和架构文档，并在影响不变量或技术路线时新增 ADR。
