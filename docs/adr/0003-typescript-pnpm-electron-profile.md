# ADR-0003: MVP 使用 TypeScript pnpm monorepo 与 Electron Desktop

> Status: accepted  
> Date: 2026-10-24

## 背景

Gateway、Desktop、公共契约和测试若各自选择语言与 DTO，会让 AI 重复定义状态、错误和协议。Desktop 还需要同时承载安全 UI、本地持久化、出站 WSS 和 Codex app-server 子进程，因此必须先固定运行时与进程边界。

## 决策

- 仓库采用 TypeScript strict monorepo，由 pnpm workspace 管理 `apps/*`、`packages/*` 和 `plugins/*`。**目录布局部分已由 [ADR-0004](0004-shared-contracts-as-plain-data.md) 修订**：workspace 成员只保留 `src/*`，MVP 不建 `packages/` 与 `plugins/`。
- Node.js 使用开始实现时的 active LTS，并在仓库、CI 和 Desktop 构建中锁定同一版本。
- Gateway 采用模块化单体并以 PostgreSQL 保存云端事实。
- Desktop 采用 Electron，将 sandboxed Renderer、Main 和 Daemon 分离；SQLite 保存本地执行事实。
- 公共运行时输入由共享 JSON Schema 严格校验。协议 schema、领域类型和 fixtures 放在共享 package，不在 producer/consumer 内复制。**承载方式已由 [ADR-0004](0004-shared-contracts-as-plain-data.md) 修订**：MVP 阶段契约的权威表述是 `docs/contracts/` 的文档，schema 与 fixtures 作为实现产物在实现 Spec 中落位；两端用同一组 fixtures 做双向一致性校验，"不在 producer/consumer 内复制"的意图不变。
- Codex app-server 只能由 Desktop Daemon 通过本地 stdio 调用。
- HTTP、数据库 adapter、UI 渲染、schema 校验和测试库由对应 bootstrap Task 比较后一次性选定并锁版，不得在业务模块中提前形成事实标准。

完整目录、依赖方向和 UI 组件树见 [`docs/code-structure.md`](../code-structure.md)。

## 后果

- Gateway、Desktop 和契约能够共享类型、schema 和 contract fixtures。
- Electron Renderer 不获得 Node、token、文件系统或子进程权限；所有能力通过最小化 IPC facade 暴露。
- 首批代码 Task 必须先创建 workspace、锁定工具版本并提供统一检查命令，之后才能开始依赖具体框架的业务实现。
- 若更换语言、workspace 管理器、Desktop 容器或数据存储，必须新增 ADR、迁移计划和跨模块验收。
