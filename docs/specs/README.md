# Execution Spec 索引

Execution Spec 把已接受架构拆成可由一个 DRI 推进、可独立评审和验收的 bounded slice，不创建第二套路线图。状态和门禁遵守 [`docs/quality-gates.md`](../quality-gates.md)。

| Spec | 状态 | 主模块 | DRI | 下一门禁 |
| --- | --- | --- | --- | --- |
| [SPEC-0001 契约实现：MCP、Device Presentation、Gateway/Desktop 与领域结果的 schema 与跨实现 fixtures](./0001-contracts-schema-and-fixtures.md) | `Proposed` | `contracts` | 待指派 | 指派 DRI / 必要 Reviewer / Integration Owner，并按其 §14 锁定工具链选型后进入 `Accepted` |
| [SPEC-0002 Device Presentation v1 真实传输与双向确认](./0002-device-presentation-real-transport.md) | `Proposed` | `device` | 待指派 | 冻结真实灵矽 wire、图片预算、DRI 和安全评审 |

首个 bootstrap Spec 覆盖实施路线的第 1 步“契约实现”。它保持 `Proposed`：按 [`../quality-gates.md`](../quality-gates.md) §3，`Proposed` 不授权实现，且 §14 的 D-1（责任人）尚未解除。新增 Spec 从[模板](./template.md)创建。

新增、接受、实施或废弃 Spec 时必须同步本表。未列入索引的草稿不授权实现。
