# LinxBox 设计文档索引

本目录是 LinxBox 的设计事实来源。

| 文档 | 作用 |
| --- | --- |
| [最终技术架构](./architecture.md) | 总体架构、端到端链路、身份、协议、Codex 集成、安全、可靠性和实施路线 |
| [AI 设计规范](./ai-specification.md) | 灵矽 Agent、Gateway、Desktop 和 Codex 的行为与输出约束 |
| [工程设计规范](./engineering-guidelines.md) | 分层、契约、错误、进程安全、测试和变更纪律 |
| [模块索引与依赖](./modules/README.md) | 每个模块独立规范的入口、依赖和事实所有权 |
| [端到端串联](./end-to-end.md) | 语音、控制、展示与本地审阅四条链路的完整闭环 |
| [代码与 Desktop UI 结构](./code-structure.md) | monorepo 目录树、分层规则和 Desktop UI DOM/组件树 |
| [契约索引](./contracts/README.md) | MCP、Gateway/Desktop、领域状态和结果契约 |
| [AI 开发与质量门禁](./quality-gates.md) | Spec 状态、DRI/Reviewer、证据分级、合并和启用门禁 |
| [Execution Spec 索引](./specs/README.md) | 可实施 bounded slice 及其状态 |

已接受决策：

- [ADR-0001：MVP 使用网关预配置的设备—客户端绑定](./adr/0001-mvp-preconfigured-device-client-binding.md)
- [ADR-0002：Desktop 使用 Codex app-server 作为远程任务执行接口](./adr/0002-codex-app-server-local-execution.md)
- [ADR-0003：MVP 使用 TypeScript pnpm monorepo 与 Electron Desktop](./adr/0003-typescript-pnpm-electron-profile.md)

涉及公共 MCP、任务状态、身份授权、Desktop 协议、Linx progress 路由或 Codex 集成方式的变更，必须先更新契约和架构文档，并在影响不变量或技术路线时新增 ADR。
