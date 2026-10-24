# 模块规范

> Status: accepted  
> Version: 2.1  
> Last updated: 2026-10-24

本文件写仓库内的三个可部署单元：Gateway、Desktop、admin-cli。跨模块完整链路见[架构文档](architecture.md) §1 与 §8，字段级语义见[契约索引](contracts/README.md)，目录与分层见[文档索引](README.md) 与[架构文档](architecture.md) §4.1。

| 单元 | 一句话职责 |
| --- | --- |
| `gateway` | 身份绑定、任务事实、可靠投递和脱敏设备投影 |
| `desktop` | 本地授权、执行事实、结果整理和用户审阅 |
| `admin-cli` | Desktop client、绑定与 token 的运维 |

```text
gateway <== Gateway/Desktop WSS ==> desktop <== local stdio ==> Codex app-server
```

依赖规则：

- `gateway` 与 `desktop` 之间不建立代码依赖，只通过 [Gateway/Desktop 契约](contracts/gateway-desktop.md) 通信；两端各自实现 TypeScript 类型，用同一组 fixtures 双向校验。
- Domain 只包含类型、不变量和纯状态转换；Adapter 实现 ports，不定义业务规则；composition root 是唯一组装具体 adapter 的位置。
- 一个 Task 只能有一个主模块，跨边界变更要列 producer、consumer 与 Integration Owner。
- 公共状态字符串、DTO、错误码和 ID 别名一律来自契约，不在各自分支里各造一份。

## gateway 单元

| 子模块 | 责任与权威数据 |
| --- | --- |
| `composition` | 唯一组装根 |
| `mcp-transport` | MCP 认证、DTO、progress 和工具结果；不拥有业务状态 |
| `identity` | token hash、device-client binding、禁用和轮换 |
| `projects` | 绑定范围内的项目别名和 availability 投影 |
| `tasks` | Task、Turn lineage、TaskEvent 和幂等写入 |
| `presentation` | 任务卡片、回复页、图片描述和回执的脱敏设备投影 |
| `broker` | Desktop WSS presence、sequence、ACK 和重放 |
| `outbox` | Task/Presentation Delivery 的事务投递事实 |
| `policy` | 身份、绑定、能力、内容分级和风险预检 |
| `audit` | 不含代码、路径、正文和凭据的脱敏审计 |

规则：

- 写操作以已验证身份和 idempotency key 去重；Task 与首个 Delivery 同事务提交；
- Device 不能提交 client ID、本地路径或 provider ID；续接只携带父 Task ID；
- Presentation 只包含 Desktop 明确生成的 device-safe 内容：标题、短摘要、分页正文、受限图片衍生物和回执；
- 禁传字段与内容分级见[契约索引](contracts/README.md) 的单一事实源；
- 内容保存期限、最多八轮设备历史、页数/字节上限和删除必须由 schema/配置固定，不能无限增长；
- ACK、receipt、turn terminal 和 task terminal 是不同事实。

门禁：依赖 contracts、PostgreSQL 和 WSS transport，不依赖本地文件系统或 Codex 凭据。身份隔离、幂等、Outbox、ACK/重放、乱序、限流、内容上限、删除和脱敏必须有单元、契约及真实存储集成测试。

## desktop 单元

| 子模块 | 目录 | 责任 |
| --- | --- | --- |
| `ui` | `src/desktop/main`、`src/desktop/renderer` | token、连接、项目授权、任务、完整 diff、验证、风险和取消 |
| `gateway-client` | `src/desktop/daemon/gateway-client` | 出站 WSS、hello、heartbeat、双向 sequence/ACK、outbox 重放和恢复 |
| `project-registry` | `src/desktop/daemon/project-registry` | 本地路径、不透明 project ID、grant 和撤销 |
| `policy` | `src/desktop/daemon/policy` | realpath、能力交集、风险和本地最终拒绝 |
| `tasks` | `src/desktop/daemon/tasks` | 去重、执行所有权、状态、取消和崩溃恢复 |
| `result-projector` | `src/desktop/daemon/result-projector` | 将本地结果投影为 `local-detail`、`cloud-summary`、`device-content` 三级 |
| `codex-adapter` | `src/desktop/daemon/codex-adapter` | Codex app-server 生命周期到领域事件的唯一转换层 |
| `local-store` | `src/desktop/daemon/local-store` | project、双向 delivery/outbox、task/provider ownership、完整本地结果 |

结果分级：

1. `local-detail`：完整 diff、命令输出和本地路径，只供 Desktop 用户审阅；
2. `cloud-summary`：相对路径摘要、验证结果和脱敏错误；
3. `device-content`：无源码/路径/凭据的标题、摘要、分页用户可见回复和设备图片衍生物。

每级都由确定性 projector 产生；不能把 app-server 原始事件直接上传。用户要求"完整回复"表示完整的 device-safe 用户可见回复，不表示完整内部日志、推理或代码。

`codex-adapter` 是 OpenAI app-server 协议和领域事件之间的唯一转换层，必须实现：通过本地 stdio 启动固定兼容范围的 app-server；新任务 `thread/start`、同项目续接 `thread/resume`、请求执行 `turn/start`；用 `turn/interrupt` 请求中断并等 `turn/completed` 确认终态；只有 `turn/completed` 且 status 为 `completed` 才映射成功；token 更新或进程重启后重新初始化并恢复持久化 thread ownership。不得把实验事件、内部推理、原始 stderr、provider ID 或未经清洗的 command/file event 暴露给 Gateway 或 Device。

不变量与门禁：

- 保存 `task_id <-> public turn_id <-> provider thread/turn ID <-> project_id`，provider ID 只在本地；
- 接到 device command 后重新检查 binding、grant、目标 task/turn/image 和风险；
- 同一 `message_id/command_id` 至多产生一次 turn；
- 路径逃逸、跨项目续接、无可信审批的 R2/R3、app-server 崩溃和恢复不确定性全部 fail closed；
- UI、Daemon、SQLite 和真实 app-server 分别提供单元、集成、崩溃恢复和目标 macOS 验收证据；app-server 升级先通过旧 fixture、新 fixture 和真实验收。

## admin-cli 单元

运维命令行工具，跑在有数据库访问权的运维机上，不在任何用户数据路径上。

必须支持：

- 创建 Desktop client，签发一次性展示的 `client_token`；Gateway 只保存 token hash；
- 预配置 `linx_device_id → client_id` 绑定（ADR-0001 的运维预配置）；
- 轮换 token、禁用 client、解绑。

MVP 不实现产品账号登录、配对码或扫码，换机、解绑和 token 轮换由运维通过本工具完成。

## 其他归属

本仓库之外或跨仓库的部分，其规范各自独立，不在本文展开：

| 归属 | 规范位置 |
| --- | --- |
| `device`（AI Passport / ESP32 固件工程） | [Device/Linx 契约](contracts/device-linx.md)；离线演示与真机验收见 [Issue #4](https://github.com/KevinHuo/LinxBox/issues/4) |
| `linx`（灵矽 AI） | [灵矽 Agent 行为契约](agent-behavior.md) 与 [MCP 工具契约](contracts/mcp.md) |
| `codex`（app-server 协议） | Desktop 的 `codex-adapter` 子模块；[ADR-0002](adr/0002-codex-app-server-local-execution.md) |
| `contracts`（schema 与状态机） | [契约索引](contracts/README.md) |
| `quality`（门禁与证据） | [质量门禁](process/quality-gates.md) |
