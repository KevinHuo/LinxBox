# 契约实现：MCP、Device Presentation、Gateway/Desktop 与领域结果的 schema 与跨实现 fixtures

| 字段 | 值 |
| --- | --- |
| Spec ID | `SPEC-0001` |
| 状态 | `Proposed` |
| 主模块 / Slice | `contracts` / 契约实现 slice |
| 上游架构/契约/ADR | [`docs/architecture.md`](../architecture.md) §7、§11、§12、§16、§17；[`docs/contracts/mcp.md`](../contracts/mcp.md)（`linxbox.mcp/v1`）、[`docs/contracts/device-linx.md`](../contracts/device-linx.md)（`device-presentation/v1`）、[`docs/contracts/gateway-desktop.md`](../contracts/gateway-desktop.md)（`1.0`）、[`docs/contracts/domain.md`](../contracts/domain.md)（`task-domain/v1`、`task-result-v1`）；[ADR-0002](../adr/0002-codex-app-server-local-execution.md)、[ADR-0003](../adr/0003-typescript-pnpm-electron-profile.md)、[ADR-0004](../adr/0004-shared-contracts-as-plain-data.md) |
| DRI | `待指派（必须为具体个人）` |
| 必要 Reviewer | `contracts` 模块备份评审人、`quality` 模块主负责人（均待指派） |
| Integration Owner | `待指派（Gateway / Desktop / Device 三侧接入方）` |
| 最近更新 | `2026-10-24` |

## 1. 定位、依赖和消费者

- 上游契约：本 Spec 只实现 `docs/contracts/` 下四份已接受契约的字段与语义，不新增、不改写契约含义。
- 下游消费者：`src/gateway` 的 `mcp-transport`、`presentation`、`broker`、`tasks`；`src/desktop` 的 `daemon/gateway-client`、`tasks`、`codex-adapter`、`local-store`；`src/admin-cli`；`tests/fixtures/device`（Issue #4 离线演示 fixture）；producer/consumer 契约测试。
- 依赖 Spec/PR：[Task #5](https://github.com/KevinHuo/LinxBox/issues/5)（仓库内契约基线入库）。
- 被替代内容：无。这是本仓库首次把契约实现为 schema、类型与 fixtures。
- 接受阻塞项：
  1. DRI、必要 Reviewer、Integration Owner 尚未指派。
  2. JSON Schema 校验库、类型生成方式、测试运行器与覆盖率工具未锁定。按 [`docs/code-structure.md`](../code-structure.md) §1，这些由 bootstrap Task/ADR 一次性选定并锁版，锁版前不得开始依赖其 API 的实现。
  3. `device-presentation/v1` 的 wire encoding 未冻结，需按真实灵矽能力确认。

## 2. 当前实现事实

- 仓库当前没有业务代码：`src/` 与 `tests/` 只有空骨架，`scripts/` 是文档一致性自检工具。
- 没有 schema 文件、类型定义、fixtures、测试运行器或 CI。
- 已有事实仅限 `docs/` 下的语义基线文档，以及 Issue #4 提供的设备端 UI/状态机离线 fixture 需求。

因此本 Spec 的全部交付都是新建，不存在可复用的既有实现。

## 3. 目标

1. 把四份契约实现为严格校验的 JSON Schema 与对应 TypeScript 类型，作为实现阶段的单一事实源。
2. 建立跨实现 fixtures：同一份 fixture 同时被 Gateway producer、Desktop consumer 与 Device 消费方使用，字段级一致。
3. 把公共错误、标识符、状态枚举、幂等键与上限固化为可执行的校验，而不是只写在文档里。
4. 为公共 schema 提供正例与反例 fixture，支撑后续模块的 producer/consumer 契约测试。

## 4. 非目标

- 不实现 Gateway、Desktop、Codex adapter、MCP transport、Presentation Bridge 或 ESP32 固件逻辑。
- 不改变任何契约语义。发现契约缺陷时，先按 [`docs/quality-gates.md`](../quality-gates.md) §1 更新 `docs/contracts/`（必要时升版本），再改 schema。
- 不引入产品账号、自助配对、审批 UI、多 Desktop 或 MCP Tasks 扩展。
- 不做真实灵矽、ESP32、WSS、app-server 或目标操作系统的联调验收。
- 不新增业务运行时代码、依赖或 CI 流水线（CI 接入由后续 bootstrap slice 负责）。

## 5. 权威边界与不变量

- 权威顺序：`docs/architecture.md` → `docs/contracts/` 与 Accepted ADR → `docs/modules.md` → 本 Spec → Task Issue → 代码与测试。
- schema 是实现阶段的单一事实源；文档是 schema 落地前的字段与语义基线（[`docs/contracts/README.md`](../contracts/README.md) §变更规则）。
- 公共错误 code 集合固定为：`UNAUTHENTICATED`、`PERMISSION_DENIED`、`INVALID_ARGUMENT`、`NOT_FOUND`、`CONFLICT`、`RESOURCE_EXHAUSTED`、`DEPENDENCY_UNAVAILABLE`、`APPROVAL_REQUIRED`、`POLICY_DENIED`、`INTERNAL`；设备侧固定为 `OFFLINE`、`STALE_REVISION`、`TARGET_NOT_FOUND`、`DUPLICATE`、`QUEUE_FULL`、`CONTENT_UNAVAILABLE`、`PERMISSION_DENIED`、`POLICY_DENIED`、`INTERNAL`。
- 安全请求默认拒绝未知字段；只有显式声明可扩展的 metadata 容器允许未知键。
- 标识符是不同值对象：`DeviceId`、`ClientId`、`ProjectId`、`TaskId`、`MessageId`、`RequestId`、`ProviderThreadId`、`ProviderTurnId` 禁止互换或退化为裸 string。
- 公共 MCP 的 `arguments` 禁止出现 `installation_id`、`device_id`、`session_id`、`client_id`、本地路径、cwd 和运行时配置；这些由 Gateway transport 从已验证连接与 Header 注入。
- MCP 控制通道与 Device Presentation 通道相互隔离；展示消息不能绕过任务授权。

## 6. 组件、接口与版本策略

契约的权威表述是 [`docs/contracts/`](../contracts/) 的文档；下表是本 Spec 要产出的实现产物，落位由对应 Slice 决定，文档阶段不预建目录。

| 产物 | 内容 | 上游契约 |
| --- | --- | --- |
| MCP schema | 六个工具的请求/结果 schema、progress payload、`PublicError` | `linxbox.mcp/v1` |
| Device schema | snapshot/delta、content.page、image manifest、draft、receipt | `device-presentation/v1` |
| Gateway/Desktop schema | envelope、`client.*`、`project.*`、`task.*`、`presentation.*`、`command.receipt` | `1.0` |
| Result schema | `task-result-v1` 结构与字段规则 | `task-result-v1` |
| Domain 类型 | 标识符值对象、Task/Receipt 状态与转换、风险等级 | `task-domain/v1` |
| Fixtures | 跨实现正反例输入 | 以上全部 |

- 版本策略：新增 optional 字段在同一主版本内演进；删除、改名、改变类型或改变语义必须升约主版本，并在 fixture 与迁移说明中体现。
- `contract_version` 与 `protocol_version` 由 schema 常量导出，不允许散落硬编码字面量。
- 未冻结字段：`TBD`，不得用伪代码掩盖决策缺口。
  - `TBD`：JSON Schema 校验库与版本。
  - `TBD`：TypeScript 类型生成方式（手写 / 代码生成 / 双向校验）。
  - `TBD`：`device-presentation/v1` 的 wire encoding。

## 7. 状态机与数据模型

**Task 合法转换**（`task-domain/v1`）：

```text
requested -> rejected | queued
queued -> waiting_for_client | dispatched
waiting_for_client -> expired | dispatched
dispatched -> accepted
accepted -> running
running -> succeeded | failed | interrupted | approval_required
approval_required -> running | failed | cancelled
any non-terminal -> cancel_requested -> cancelled | interrupted
```

- 终态：`rejected | expired | succeeded | failed | cancelled | interrupted`；终态不转回活动态。
- `acknowledged != accepted != running != succeeded`；无法证明成功或失败时使用 `interrupted`。
- `approval_required` 不是语音审批通道，MVP 无可信 UI 时转为拒绝或取消。
- 续接是新 turn / 新执行记录，必须重新授权。

**Receipt 合法转换**（不属于 Task 状态机）：

```text
sending -> desktop_received -> queued -> agent_accepted -> running
        -> turn_completed | failed | cancelled | interrupted
```

- `sending` 不证明 Gateway 收到；`desktop_received` 不证明 Agent 接受；`turn_completed` 不证明 Task `succeeded`。

**幂等键与标识**：

| 键 | 作用域 | 规则 |
| --- | --- | --- |
| `request_id` | 单次请求 | 必填，不提供幂等性 |
| `idempotency_key` | 同一身份 + 工具 | 所有写工具必填；同一作用域重复 key 返回原 `task_id` |
| `message_id` | 全局 | 唯一；Desktop 先持久化再 ACK；对相同 `message_id` 返回 `duplicate`，不产生第二个 Codex turn |
| `command_id` | 硬件指令 | 每 Task 最多一条未终结 command；重试复用原 `command_id` |
| `draft_id` | 草稿 | `draft.confirm` 必须引用同一 draft/command/target |

**sequence / revision**：

| 序号 | 作用域 | 规则 |
| --- | --- | --- |
| `sequence` | Gateway 对单个 client | 单调递增；重连从 `last_acked_sequence + 1` 重放 |
| `event_sequence` | 单 Task | 单调递增；最终事件只出现一次；重放使用相同 `event_id` |
| `receipt_sequence` | 单 `command_id` | 单调递增，按序去重 |
| `revision` | 展示投影 | 不连续时 Device 请求 snapshot，不猜测中间状态 |
| `progress` | 单 `progressToken` | 单调递增序号，不是百分比 |

**TTL 与上限**：

- `expires_at`：仅限可过期命令；过期 `task.dispatch` 不执行。
- `retention_until`：`presentation.upsert` 的设备可见内容保留期。
- `voice_summary`：最长 120 个汉字，不含代码、绝对路径、命令输出或内部推理。
- Device 最多保留最近八轮；服务端保留与分页上限由本 Spec 在实现期固定（当前 `TBD`）。
- 任务数量、标题与摘要上限按设备内存预算固定（8 MB Flash、无 PSRAM）。
- 图片只允许 `overview` / `top` / `bottom` 预生成 RGB565 兼容衍生物；分块大小、压缩、重传与缓存淘汰 `TBD`。

## 8. 失败、降级与恢复

| 场景 | 行为 | fail 方向 |
| --- | --- | --- |
| 未知字段出现在安全请求 | 拒绝（`INVALID_ARGUMENT`） | fail closed |
| 缺少 `idempotency_key` 的写工具 | 拒绝 | fail closed |
| 重复 `idempotency_key` | 返回原 `task_id`，不产生第二次执行 | fail closed |
| 重复 `message_id` | ACK `disposition: duplicate` | fail closed |
| `sequence` 缺口 | 请求 snapshot，不猜测中间状态 | fail closed |
| `expires_at` 过期 | 不执行该命令 | fail closed |
| `protocol_version` major 不兼容 | `protocol.upgrade_required` 并关闭连接 | fail closed |
| `revision` 不连续 | Device 请求 snapshot | fail closed |
| 草稿确认超时 | 停留在确认态，不等同 confirm | fail closed |
| 恢复在线 | 只请求 snapshot 并重放未 ACK 读取请求；不自动重发未确认 draft 或失败 command | fail closed |
| 未知 `task_id` / `project_id` | `NOT_FOUND` | fail closed |
| 无权访问的项目 | `PERMISSION_DENIED` | fail closed |

- MCP 断线不取消 Task；恢复后通过 `get_task_status` 查询。
- progress 丢失不改变持久化事实。
- `stop_task` 结果只能声明已记录取消请求，不得在 Desktop/provider 真正终止前返回 `cancelled`。

## 9. 健康、容量与可观测性

- Readiness/Liveness：不适用。本 Spec 只产出 schema 与 fixtures，不引入运行时进程。
- Metrics / Reason Code / Trace：不适用。公共错误 code 集合是后续可观测性的稳定标签来源，本 Spec 保证其可枚举。
- 资源水位、Dashboard、Alert、Runbook Owner：`TBD`，随 Gateway/Desktop runtime slice 确定。
- 容量契约：本 Spec 固定 payload 上限与保留期（见 §7），作为后续容量水位与设备内存预算的输入。

## 10. 安全与隐私

- 禁止上传：绝对路径、完整 diff、原始 stdout/stderr、模型内部推理、Codex token、cookie、环境变量、未脱敏代码。
- 允许上传：相对路径的受限变更摘要、命令类别与通过/失败结果、阶段、脱敏错误、短摘要，以及有明确保留期的 device-safe 回复分页与设备图片衍生物。
- `PublicError.details` 不允许出现 token、cookie、绝对路径、原始 prompt、代码、命令输出、环境变量或 provider 内部错误对象。
- `task-result-v1`：`changes[].path` 必须相对授权项目根，不允许 `..` 或绝对路径；`verification[].command` 必须真实执行，`outcome` 只能是 `passed | failed | not_run`。
- `succeeded` 必须同时满足：请求范围完成；所声称的修改可从本地事实证明；所声称的验证真实运行；provider 终态明确为成功。
- 风险分级 `R0`/`R1`/`R2`/`R3` 由 schema 表达；MVP 只允许本地 grant 包含的 R0/R1，R2 在无可信本地审批 UI 时拒绝，R3 禁止从语音入口批准。
- 路径逃逸、prompt injection、密钥请求相关负例在 fixtures 中必须有反例。

## 11. 兼容、启用、发布与回滚

- 兼容：新增 optional 字段向后兼容；删除、改名、改类型、改语义必须升约主版本并提供迁移说明。
- 启用：schema 包默认只作为类型与校验入口，不自动启用任何功能通道。功能启用门禁按 [`docs/quality-gates.md`](../quality-gates.md) §5 单独执行。
- 发布：schema 变更先合入契约包并通过 producer/consumer 契约测试，再由消费模块跟进；顺序不得倒置。
- 回滚：回退 schema 版本与对应 fixture，重新执行契约测试；`contract_version` 变更必须回退所有消费方。

## 12. 测试与验收证据

| 类型 | 场景 | 命令或证据 | Owner | 状态 |
| --- | --- | --- | --- | --- |
| Docs / Schema | 四份契约的 schema 文件与字段级 fixture 全部存在且可校验 | 仓库内 schema 校验命令（随工具链锁定） | contracts DRI | Not run |
| Unit / Coverage | 状态机合法/非法转换、标识符类型约束、上限与长度约束 | 单测 + 覆盖率报告 | contracts DRI | Not run |
| Contract | producer（Gateway）与 consumer（Desktop、Device）共用同一 fixture 双向校验 | producer/consumer 契约测试 | contracts DRI + Integration Owner | Not run |
| Integration | schema 与真实 app-server / 灵矽 payload 的字段映射 | 随 Gateway/Desktop slice 提供 | integration owner | Not run |
| Fault / Security | 未知字段、越权项目、路径逃逸、重复幂等键、乱序 sequence、`details` 泄漏检查 | 安全负例 fixture + 校验测试 | quality 模块 | Not run |
| Simulated E2E | mock app-server 与模拟灵矽下的消息往返 | 随 Gateway/Desktop slice 提供 | integration owner | Not run |
| Real E2E / Soak | 真实灵矽、ESP32、WSS、app-server 验收 | 不在本 Spec 范围 | — | Not run |

## 13. 实现拆分

| 顺序 | PR Slice | DRI | Reviewer | 依赖 | 输出 |
| --- | --- | --- | --- | --- | --- |
| 1 | 工具链与骨架：pnpm workspace、schema 校验库、类型生成、测试运行器与覆盖率接入 | 待指派 | quality 模块主负责人 | ADR-0003、ADR-0004、工具链选型决策 | 可运行的 `src/*` 骨架与 CI 入口 |
| 2 | 领域类型与状态机：标识符值对象、Task/Receipt 转换、风险等级（实现于各应用，语义以 schema 为准） | 待指派 | contracts 备份评审人 | Slice 1 | 领域类型与状态机单测 |
| 3 | MCP 与 Result schema（落位由本 Slice 决定） | 待指派 | contracts 备份评审人 | Slice 2 | MCP 工具与结果 schema、错误 schema |
| 4 | Device 与 Gateway/Desktop schema（落位由本 Slice 决定） | 待指派 | contracts 备份评审人 | Slice 2 | 展示投影、envelope 与命令/事件 schema |
| 5 | 跨实现 fixtures 与 producer/consumer 契约测试（落位由本 Slice 决定） | 待指派 | quality 模块主负责人 | Slice 3、4 | 跨实现契约测试与安全负例 |

## 14. 待决策与风险

| ID | 问题 | 安全默认 | 决策人 | 最晚时间 | 状态 |
| --- | --- | --- | --- | --- | --- |
| D-1 | DRI、必要 Reviewer 与 Integration Owner 未指派 | 不接受 Spec，保持 `Proposed` | 仓库维护者 | 接受 Spec 前 | Open |
| D-2 | JSON Schema 校验库与版本 | fail closed，未知字段拒绝 | bootstrap Task/ADR 决策人 | Slice 1 实施前 | Open |
| D-3 | TypeScript 类型生成方式 | 不自动生成，避免手写与生成不一致 | bootstrap Task/ADR 决策人 | Slice 1 实施前 | Open |
| D-4 | 测试运行器与覆盖率工具 | 未达门禁不得合并 | bootstrap Task/ADR 决策人 | Slice 1 实施前 | Open |
| D-5 | `device-presentation/v1` wire encoding 未按真实灵矽能力冻结 | 不实现 wire 层，只实现语义对象 | `linx` 模块主负责人 | Device 接入 slice 前 | Open |
| D-6 | 设备内存预算下的任务数量、标题、摘要、分页与图片上限 | 取保守上限，不按理想值实现 | `device` 模块主负责人 | Slice 4 实施前 | Open |
| D-7 | `codex-adapter` 映射所依据的 app-server 版本未锁定 | 不映射原始事件为公共契约 | `codex` 模块主负责人 | Codex 接入 slice 前 | Open |

## 15. 实现与验收记录

- 实现 PR/审阅 commit：未开始。
- 必要批准：未开始（Spec 未 Accepted，无实现授权）。
- 最终验证：未执行。
- 未覆盖风险和仍关闭的发布门禁：本 Spec 状态为 `Proposed`，按 [`docs/quality-gates.md`](../quality-gates.md) §3 不授权实现；§5 的功能启用门禁与 §6 的真实链路证据要求全部仍然关闭。
