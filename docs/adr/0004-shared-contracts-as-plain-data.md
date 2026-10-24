# ADR-0004: 去掉 `packages/` 与 `plugins/`，MVP 契约只以文档形态存在

> Status: accepted  
> Date: 2026-10-24  
> 修订：ADR-0003 的目录布局部分

## 背景

Gateway 与 Desktop 是两个独立进程、两套部署（云端 PostgreSQL / 本地 Electron + SQLite），但中间要传 `task.dispatch`、`task.ack`、`task.progress` 和 `presentation.upsert`。两边各写一份类型或 schema，改动不会在编译期暴露，只会在运行时对不上。ADR-0003 因此要求"协议 schema、领域类型和 fixtures 放在共享 package，不在 producer/consumer 内复制"。

把共享物做成 9 个 pnpm 包带来两个实际负担：

1. 仓库顶层出现 `apps/`、`packages/`、`plugins/` 三层，新成员要先理解布局才能定位代码；
2. 其中多数包在 MVP 内并不使用（`observability`、`ports`、`plugins/linx-codex` 属于后续阶段），空包与空目录反而暗示了尚不存在的结构。

复核后确认三点：

- 需要共享的是"schema 只有一份"这个事实，不依赖共享物放在哪个目录——pnpm 的包边界由每个目录自己的 `package.json` 与依赖集决定，与目录名叫不叫 `packages` 无关；
- 跨组织协作方（灵矽 MCP 客户端、ESP32 固件工程）拿不到也不需要 TS 包，它们需要的是 JSON Schema 文件与 fixture JSON，纯数据形态对它们更直接；
- MVP 阶段还没有任何实现代码，契约的权威表述是文档本身；为还不存在的 schema 文件预建目录，只会让仓库看起来比实际更复杂。

## 决策

- 取消仓库顶层的 `packages/` 与 `plugins/` 目录。
- **契约的权威表述是 [`docs/contracts/`](../contracts/) 的文档。** JSON Schema 与 fixtures 是实现产物，不是文档阶段的交付物；落位由对应实现 Spec 决定，在那之前不预建目录、不放占位文件。
- Gateway 与 Desktop 各自实现 TypeScript 类型；一致性由同一组 fixtures 的 producer/consumer 双向校验保证（见 [`docs/modules.md`](../modules.md) 的"类型从 schema 生成或双向一致性检查"）。是否用代码生成类型，由首个契约 Task 在选型时决定。
- pnpm workspace 保留，成员只有 `apps/*`；Gateway 与 Desktop 之间不建立代码依赖。
- 领域类型（ID 值对象、Task/Receipt 状态与转换）在各应用内实现，跨边界语义以 [`docs/contracts/domain.md`](../contracts/domain.md) 为准，不复制第二份状态机。
- `plugins/` 的能力属于产品化阶段，MVP 不预建目录。

## 后果

- 仓库顶层只剩 `apps/`、`docs/`、`tests/`（以及助手工作规范目录），MVP 内不出现空包或占位目录。
- 失去包依赖图对"契约层不依赖 transport"的强制力，改为靠目录约定、契约测试和评审保证。
- schema 变更不再自动同步两端类型；漂移会在契约测试或 CI 失败，而不是编译期失败。因此契约测试是必选项，不是可选项。
- 灵矽与固件工程在实现阶段直接消费 schema 与 fixture 文件，不需要安装本仓库的包。
- 若后续需要对外发布 SDK 或把共享物拆成独立包，新增 ADR，不在本决策下直接改目录。
