# LinxBox 设计文档索引

本目录是 LinxBox 的设计事实来源，分两组：

**一、产品与契约基线**（评审与实现都要看）

| 文档 | 作用 |
| --- | --- |
| [最终技术架构](./architecture.md) | 总体架构、端到端链路、身份、协议、Codex 集成、安全、可靠性和实施路线 |
| [边界契约索引](./contracts/README.md) | MCP、Device Presentation、Gateway/Desktop、领域状态与结果契约；单一事实源表 |
| [模块规范](./modules.md) | 仓库内三个可部署单元（Gateway、Desktop、admin-cli）的职责、依赖方向与交付门禁 |
| [灵矽 Agent 行为契约](./agent-behavior.md) | 灵矽 Agent、Gateway、Desktop 和 Codex 的行为边界、任务上下文与输出约束 |
| [决策记录](./adr/) | 已接受的架构决策（ADR-0001 ~ 0005） |

**仓库结构**

```text
LinxBox/
├── src/                            # 三个可部署单元
│   ├── gateway/                    # Cloud MCP Gateway
│   │   ├── composition/            # 唯一组装根
│   │   ├── mcp-transport/          # MCP DTO/progress/result
│   │   ├── identity/               # client/token/binding
│   │   ├── projects/               # 云端项目投影
│   │   ├── tasks/                  # use case + state machine
│   │   ├── presentation/           # 设备安全任务/内容/回执投影
│   │   ├── broker/                 # Desktop WSS
│   │   ├── outbox/                 # Delivery worker
│   │   ├── policy/                 # 云端策略
│   │   └── audit/                  # 脱敏审计
│   ├── desktop/                    # LinxBox Desktop
│   │   ├── main/                   # Electron 主进程与安全 IPC
│   │   ├── renderer/               # 纯 UI，不直接访问 Node
│   │   └── daemon/
│   │       ├── composition/
│   │       ├── gateway-client/
│   │       ├── project-registry/
│   │       ├── policy/
│   │       ├── tasks/
│   │       ├── result-projector/   # local-detail/cloud-summary/device-content 三级投影
│   │       ├── local-store/
│   │       └── codex-adapter/
│   └── admin-cli/                  # client/binding/token 运维
├── scripts/                        # 文档与契约一致性自检，不属于产品代码
│   └── check-docs.js
├── docs/                           # 契约与规范
│   ├── contracts/                  # 边界契约（对外）
│   ├── adr/                        # 已接受决策
│   └── process/                    # 执行规范：工程规范、质量门禁、Execution Spec
│       └── specs/                  # Execution Spec 流程、模板与各 Spec
└── tests/
    ├── fixtures/
    │   └── device/                # Issue #4 UI/状态机与联网契约 fixtures
    ├── integration/
    ├── security/
    └── e2e/
```

目录骨架已按上表创建，目录内当前为空（仅 `.gitkeep`）。`pnpm-workspace.yaml`、各包 `package.json`、`tsconfig.json`、lockfile 与 CI 属于工具链 bootstrap，由 [`SPEC-0001`](process/specs/0001-contracts-schema-and-fixtures.md) 的 Slice 1 在选型锁定后补入；在此之前不创建，避免未锁定的依赖与版本进入仓库。

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
