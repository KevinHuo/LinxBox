# 代码与 Desktop UI 结构

> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24

## 1. 技术基线

- TypeScript strict monorepo，使用 `pnpm-workspace.yaml` 管理 `apps/*`、`packages/*` 和 `plugins/*`；`packageManager` 字段和 lockfile 锁定 pnpm 版本。
- Node.js 使用开始实现时的 active LTS，在 `.node-version`、CI 和 Desktop 构建中锁定同一版本，不跟随本机漂移。
- Gateway 是单进程模块化单体，使用 PostgreSQL 保存云端事实。
- Desktop 使用 Electron，Renderer、Main 和 Daemon 分离；Renderer 启用 sandbox 和 context isolation、禁用 Node integration，只通过最小化 `contextBridge` API 访问 Main/Daemon；SQLite 保存本地执行事实。
- 运行时输入使用共享 JSON Schema 严格校验；具体校验库在首个契约 Task 中锁定。
- Codex app-server 只由 Desktop Daemon 通过 stdio 访问。

HTTP 框架、PostgreSQL/SQLite adapter、UI 渲染库、schema 工具和测试运行器在各自的 bootstrap Task/ADR 中一次性选定并锁版；在该 Task 前不开始依赖其具体 API 的业务实现。

## 2. 仓库目录树

```text
LinxBox/
├── .minimax/                           # AI 助手工作规范与工具脚本（非产品代码，不受模块 DRI 约束）
│   └── skills/
│       └── linxbox-contracts/          # 契约执行 skill：SKILL.md + references/ + scripts/
├── apps/
│   ├── gateway/
│   │   └── src/
│   │       ├── composition/       # 唯一组装根
│   │       ├── mcp-transport/     # MCP DTO/progress/result
│   │       ├── identity/          # client/token/binding
│   │       ├── projects/          # 云端项目投影
│   │       ├── tasks/             # use case + state machine
│   │       ├── presentation/      # 设备安全任务/内容/回执投影
│   │       ├── broker/            # Desktop WSS
│   │       ├── outbox/            # Delivery worker
│   │       ├── policy/            # 云端策略
│   │       └── audit/             # 脱敏审计
│   ├── desktop/
│   │   └── src/
│   │       ├── main/              # Electron 主进程与安全 IPC
│   │       ├── renderer/          # 纯 UI，不直接访问 Node
│   │       └── daemon/
│   │           ├── composition/
│   │           ├── gateway-client/
│   │           ├── project-registry/
│   │           ├── policy/
│   │           ├── tasks/
│   │           ├── result-projector/        # local-detail/cloud-summary/device-content 三级投影
│   │           ├── local-store/
│   │           └── codex-adapter/
│   └── admin-cli/                      # client/binding/token 运维
├── packages/
│   ├── domain/                         # ID、Task、Policy、领域事件
│   ├── contracts-mcp/                  # 公共 MCP schema
│   ├── contracts-desktop/              # Gateway/Desktop schema
│   ├── contracts-device/               # Device/Linx presentation schema
│   ├── contracts-result/               # task-result-v1
│   ├── ports/                          # store/broker/agent/clock/ids
│   ├── codex-app-server/               # app-server adapter + contract tests
│   ├── observability/                  # 脱敏事件和 metrics
│   └── test-contracts/                 # 跨实现 fixtures
├── plugins/
│   └── linx-codex/                     # 可选，不在控制主路径
├── docs/
│   ├── contracts/
│   ├── modules/
│   ├── adr/
│   └── specs/
└── tests/
    ├── fixtures/
    │   └── device/                      # Issue #4 UI/状态机与联网契约 fixtures
    ├── integration/
    ├── security/
    └── e2e/
```

目录骨架已按本节创建，目录内当前为空（仅 `.gitkeep`），不含任何实现代码。`pnpm-workspace.yaml`、各包 `package.json`、`tsconfig.json`、lockfile 与 CI 属于工具链 bootstrap，由 [`SPEC-0001`](./specs/0001-contracts-schema-and-fixtures.md) 的 Slice 1 在选型锁定后补入；在此之前不创建，避免未锁定的依赖与版本进入仓库。

## 3. 分层规则

每个业务子模块内部使用相同方向：

```text
transport / UI
      │
      v
application use case
      │
      v
domain + ports
      ^
      │
adapter implementation
```

- Domain 只包含类型、不变量和纯状态转换。
- Application 组合 domain 和 ports，不直接导入 transport、DB 或 Electron。
- Adapter 实现 ports，不定义新的业务规则。
- Composition root 是唯一可以组装具体 adapter 的位置。
- Renderer 不持有 token、绝对路径或子进程权限；只能调用有类型、最小化的 IPC facade。

## 4. Desktop UI DOM/组件树

```text
AppShell
├── ConnectionBanner
├── Navigation
└── Router
    ├── OnboardingPage
    │   ├── GatewayTokenForm
    │   ├── CodexAuthStatus
    │   └── AppServerCompatibilityCheck
    ├── ProjectsPage
    │   ├── ProjectList
    │   ├── AddProjectDialog
    │   └── ProjectGrantEditor
    ├── TasksPage
    │   ├── TaskList
    │   └── TaskDetail
    │       ├── TaskStageTimeline
    │       ├── SummaryPanel
    │       ├── ChangedFilesPanel
    │       ├── VerificationPanel
    │       ├── RiskNotice
    │       └── CancelTaskButton
    └── SettingsPage
        ├── GatewayConnectionSettings
        ├── LocalDataControls
        └── VersionAndSecurityStatus
```

UI 状态只源自 Daemon 的已持久化事实投影。不以临时 toast、进程文本或 app-server 原始事件推断任务终态。

MVP 没有可信审批 UI；`approval_required` 显示为已拒绝/需要未来客户端升级，不提供语音确认按钮。

## 5. 模块交付约定

- 一个 Task 只能有一个主模块，但可列出多个受影响模块。
- 修改 `packages/contracts-*` 时必须同时修改 schema fixtures 和受影响的 producer/consumer 测试。
- 跨模块的重复 DTO、状态字符串或 ID 别名不得私自复制，必须来自契约包。
- 固件和灵矽实现可位于各自工程；LinxBox 仍拥有跨边界 schema、sanitized fixtures 和 E2E 证据，不复制外部工程源码。
- 任何子目录如需更细规则，可新增就近 `AGENTS.md`，但不得放宽根目录规则。
