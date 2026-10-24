# 契约索引

> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24

| 契约 | 边界 | 版本 |
| --- | --- | --- |
| [MCP 工具](mcp.md) | 灵矽 AI <-> Gateway | `linxbox.mcp/v1` |
| [Device/Linx 展示与输入](device-linx.md) | AI Passport/ESP32 <-> 灵矽 Presentation Bridge | `device-presentation/v1` |
| [Gateway/Desktop](gateway-desktop.md) | Gateway <-> Desktop Daemon | `1.0` |
| [领域状态与结果](domain.md) | Gateway、Desktop、UI、回执和设备投影 | `task-domain/v1`、`task-result-v1` |

OpenAI Codex app-server 协议不由 LinxBox 重新定义。`codex-adapter` 必须以[OpenAI 官方 app-server 文档](https://developers.openai.com/codex/app-server/)和已锁定的 app-server 版本为依据，将其映射到本目录的稳定领域契约。

## 共享形态

本目录中的文档是 schema 落地前的语义事实源。JSON Schema 与正反例 fixtures 是后续实现产物，具体落位由 [`SPEC-0001`](../process/specs/0001-contracts-schema-and-fixtures.md) 在工具链选型时决定；不得预先假设仓库根存在 `contracts/`。Gateway 与 Desktop 各自实现 TypeScript 类型，并用同一组 schema/fixtures 做 producer/consumer 双向校验。见 [ADR-0004](../adr/0004-shared-contracts-as-plain-data.md)。

灵矽 MCP 客户端与 ESP32 固件工程消费 SPEC-0001 最终确定位置中的纯数据 schema/fixture，不需要安装本仓库运行时代码；在该 Spec 实现前，不声称已有可消费的 schema 包。

## 单一事实源

同一规则只定义一次，其他文档引用而不复述：

| 规则 | 唯一定义处 |
| --- | --- |
| 公共错误 code 集合 | [MCP 工具](mcp.md) §10；Device 侧见 [Device/Linx](device-linx.md) §7 |
| 任务状态机与不变量 | [领域状态与结果](domain.md) §2 |
| 风险能力分级 R0–R3 | [领域状态与结果](domain.md) §6 |
| 标识符值对象 | [领域状态与结果](domain.md) §1 |
| 结果分级三级 | [模块规范](../modules.md) §desktop 单元 §结果分级 |
| 禁传字段清单 | [Gateway/Desktop](gateway-desktop.md) §9 |
| ACK 与重放语义 | [Gateway/Desktop](gateway-desktop.md) §6 |

## 变更规则

- schema 文件是实现阶段的单一事实源；本文档集是 schema 落地前的字段和语义基线。
- 新增 optional 字段可在同一主版本演进；删除、改名、改变类型或语义必须升约主版本。
- 安全请求默认拒绝未知字段；仅显式声明可扩展的 metadata 容器允许未知键。
- 每个契约变更必须有 producer/consumer fixture、兼容性测试和迁移说明。
