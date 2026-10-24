# 执行规范

本目录是**给执行者（人或 AI）看的工作规范**：怎么定 Issue、怎么写 Spec、怎么改契约、交付前过哪些门禁。产品与契约基线不在这里，在上一级目录。

| 文档 | 作用 |
| --- | --- |
| [工程设计规范](./engineering-guidelines.md) | 分层、契约、错误、进程安全、测试与变更纪律 |
| [质量门禁](./quality-gates.md) | 权威层级、Spec 状态、证据分级、合并与功能启用门禁 |
| [Execution Spec 索引](./specs/README.md) | 每个 bounded slice 的状态、主模块、DRI 与下一门禁；[模板](./specs/template.md) |

与上层的关系：

- 权威顺序是 `docs/architecture.md` → `docs/contracts/` 与已接受 ADR → `docs/modules.md` → 已接受 Execution Spec → 开发 Issue → 代码与测试，详见[质量门禁](./quality-gates.md) §1。
- 本目录的文档不新增产品行为；它们只规定"怎么按契约干活"。产品行为一律以上一层的 `architecture.md`、`contracts/`、`modules.md` 为准。
