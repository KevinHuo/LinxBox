# LinxBox 契约速查

冻结事实索引。**字段语义以仓库文档为准**（`docs/contracts/`、`docs/architecture.md`），本文件用于快速定位和自检，冲突时以仓库为准。

## 契约文件地图

| 契约 | 边界 | 标识 | 文件 |
| --- | --- | --- | --- |
| MCP 工具 | 灵矽 AI ↔ Gateway | `linxbox.mcp/v1` | `docs/contracts/mcp.md` |
| Device/Linx 展示与输入 | AI Passport/ESP32 ↔ 灵矽 Presentation Bridge | `device-presentation/v1` | `docs/contracts/device-linx.md` |
| Gateway/Desktop | Gateway ↔ Desktop Daemon（WSS） | `protocol_version: 1.0` | `docs/contracts/gateway-desktop.md` |
| 领域状态与结果 | Gateway、Desktop、UI、回执、设备投影 | `task-domain/v1`、`task-result-v1` | `docs/contracts/domain.md` |

已接受 ADR：`docs/adr/0001`（预配置设备—客户端绑定）、`0002`（Codex app-server 本地执行）、`0003`（TypeScript pnpm monorepo + Electron Desktop）。

OpenAI Codex app-server 协议不由 LinxBox 重新定义；`codex-adapter` 以官方文档和锁定版本为依据映射到上述稳定契约。

## 已知的未冻结点

这些是契约里并存两种读法或明确标注 TBD 的地方。碰到时把两种读法和影响面一起交维护者裁定，不要自己选一个写进方案。

| 事项 | 现状 | 影响 |
| --- | --- | --- |
| 续接语义 | `docs/contracts/mcp.md` §7："续接产生新 turn，但保留 task lineage"（同一任务追加 turn）；`docs/contracts/gateway-desktop.md` §5：`task.dispatch.continuation.parent_task_id`，"Gateway 只传父任务 ID"（新任务指向父任务） | 直接决定"一个任务的 N 轮"是哪个集合；两者读法可并存但语义不同，需 Reviewer 裁定并同步两处 |
| "最近八轮" | `docs/contracts/device-linx.md` §3 的八轮是 **Device 端有界缓存**；服务端保留与分页上限在 SPEC-0001 §7 标为 `TBD` | 任何"云端可查 N 轮"的需求都要先冻结服务端保留期，≥N 轮才有意义 |
| 公共 `TurnId` | `device-presentation/v1` 与 `gateway-desktop.md` 跨边界使用 `turn_id`，但 `docs/contracts/domain.md` §1 的值对象清单只有 `ProviderTurnId`（Desktop 本地） | 公共 turn 标识缺一个值对象定义，是契约缺口 |
| `speaker` 枚举 | `device-presentation/v1` §3 的 `content.page` 只列了 `agent` | 需要区分用户与 AI 两方的需求要显式扩展枚举并写兼容说明 |
| 服务端保留期与删除 | `docs/contracts/README.md` 要求每个契约变更给出保留与清理策略；Gateway 侧保留期尚未定值 | 任何新增云端存储都要同时定保留期、清理周期与删除语义 |
| 进度类字段的约束缺口 | `docs/contracts/mcp.md` §9 只约束 `_meta.linx.task.voice_summary`，没有约束同一节示例里给灵矽播报用的 `params.message`；`docs/contracts/gateway-desktop.md` §7 的 `task.*` 事件共用 payload 里 `summary` 没有字段级规则（没写产生方、内容级别、长度、越界时的降级动作） | 摘要类内容在各出口的约束强度不一致，实现方可以合法地"原样转发"。定规则时按字段的全部出口一起定 |

## 六个 MCP 控制工具

| 工具 | 语义 | 幂等键 |
| --- | --- | --- |
| `list_projects` | 当前设备绑定 Desktop 的项目别名与在线状态 | — |
| `list_agents` | 可用执行能力，不暴露本地凭据 | — |
| `start_task` | 创建并执行任务，通过 progress 实时返回阶段 | 必填 |
| `get_task_status` | 查询持久化状态，用于主动查询和断线恢复 | — |
| `continue_task` | 重新授权后续接已有任务 | 必填 |
| `stop_task` | 请求取消，不等于已经停止 | 必填 |

共通字段：`request_id`（必填，跟踪用，不提供幂等性）、`idempotency_key`（写工具必填，同身份同工具稳定重放原结果）、`task_id`、`project_id`（只能用 Gateway 返回的值）。

工具分两类，契约里只按"写工具必填 `idempotency_key`"区分过：

| 类别 | 判定 | 行为 |
| --- | --- | --- |
| 写工具 | 会创建或改变 Task/Turn/Receipt 状态 | 需要 `idempotency_key`；`start_task` 走 `notifications/progress`；产生 Delivery |
| 只读工具 | 只查询已持久化事实 | 不需要 `idempotency_key`；不发 progress；不产生 Delivery/Receipt；不向 Desktop 发 WSS 消息 |

新增工具时先判定类别，并把判定结论写进契约章节——现有契约里没有这句显式表述。

信任边界：`installation_id`、`device_id`、`session_id`、`client_id` 由 Gateway transport 从已验证连接与 Header 注入，**不得出现在工具 arguments 里**；arguments 也禁止本地路径、cwd 和运行时配置。所有 ID 不透明，调用方不得解析前缀或猜测值。

## 公共错误码

MCP 侧（JSON-RPC error，`error.data` 为 `PublicError`）：

```text
UNAUTHENTICATED  PERMISSION_DENIED  INVALID_ARGUMENT  NOT_FOUND
CONFLICT  RESOURCE_EXHAUSTED  DEPENDENCY_UNAVAILABLE
APPROVAL_REQUIRED  POLICY_DENIED  INTERNAL
```

Device 侧：

```text
OFFLINE  STALE_REVISION  TARGET_NOT_FOUND  DUPLICATE  QUEUE_FULL
CONTENT_UNAVAILABLE  PERMISSION_DENIED  POLICY_DENIED  INTERNAL
```

`PublicError` = `{ code, message, retryable, requestId, details? }`。`details` 不允许出现 token、cookie、绝对路径、原始 prompt、代码、命令输出、环境变量或 provider 内部错误对象。

## 任务状态机

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

终态：`rejected | expired | succeeded | failed | cancelled | interrupted`。

不变量：

- 终态不转回活动态。
- `acknowledged != accepted != running != succeeded`。
- 无法证明成功或失败时用 `interrupted`。
- `approval_required` 不是语音审批通道；MVP 无可信 UI 时转为拒绝或取消。
- 续接是新 turn / 新执行记录，必须重新授权。
- progress 丢失不改变持久化事实。

阶段（`stage`，用户可见投影，不驱动状态转换）：
`validating | waiting_for_client | preparing | analyzing | editing | testing | summarizing | waiting_for_approval | finished`

设备状态投影：`requested/queued/dispatched → queued`；`waiting_for_client` 与 `approval_required → needs_attention`；`accepted/running/cancel_requested → running`；`succeeded → completed`；`rejected/expired/failed/cancelled/interrupted → failed`。

## 回执状态机（不属于 Task 状态机）

```text
sending -> desktop_received -> queued -> agent_accepted -> running
        -> turn_completed | failed | cancelled | interrupted
```

`sending` 不证明 Gateway 收到；`desktop_received` 不证明 Agent 接受；`turn_completed` 不证明 Task `succeeded`。

## sequence / revision / 幂等键

| 键 | 作用域 | 规则 |
| --- | --- | --- |
| `sequence` | Gateway 对单个 client | 单调递增；重连从 `last_acked_sequence + 1` 重放 |
| `event_sequence` | 单 Task | 单调递增；最终事件只出现一次；重放使用相同 `event_id` |
| `receipt_sequence` | 单 `command_id` | 单调递增，按序去重 |
| `revision` | 展示投影 | 不连续时 Device 请求 snapshot，不猜测中间状态 |
| `progress` | 单 `progressToken` | 单调递增序号，不是百分比 |
| `message_id` | 全局 | 唯一；Desktop 先持久化再 ACK；重复返回 `duplicate`，不产生第二个 Codex turn |
| `command_id` | 硬件指令 | 每 Task 最多一条未终结 command；重试复用原 `command_id` |
| `draft_id` | 草稿 | `draft.confirm` 必须引用同一 draft/command/target |

## Gateway/Desktop 消息类型

客户端→Gateway：`client.hello`、`client.heartbeat`、`task.ack`（`disposition: accepted | duplicate | expired | rejected`）。
Gateway→Desktop：`project.upsert`、`project.revoke`、`task.dispatch`、`task.cancel`、`protocol.upgrade_required`。
Desktop→Gateway 事件：`task.started`、`task.progress`、`task.approval_required`、`task.completed`、`task.failed`、`task.cancelled`、`task.interrupted`。
Desktop→Gateway 投影：`presentation.upsert`、`presentation.revoke`、`command.receipt`。

`protocol_version` 是 `major.minor`；不兼容 major 必须 fail closed。`expires_at` 过期或 `task.dispatch` 过期不执行。`continuation` 只传 `parent_task_id`，provider 标识不上传也不由调用方指定。

## 结果结构 `task-result-v1`

```json
{
  "contract_version": "task-result-v1",
  "status": "succeeded",
  "summary": "...",
  "changes": [{"path": "src/login.ts", "description": "..."}],
  "verification": [{"command": "npm test -- login", "outcome": "passed", "summary": "..."}],
  "risks": [],
  "needs_user_action": null,
  "voice_summary": "..."
}
```

- `status`：`succeeded | failed | cancelled | interrupted | rejected`。
- `changes[].path`：相对授权项目根，不允许 `..` 或绝对路径。
- `verification[].command`：脱敏命令，必须真实执行；`outcome` 为 `passed | failed | not_run`。
- `voice_summary`：最长 120 个汉字，不含路径、代码、diff、凭据或堆栈。
- `succeeded` 需同时满足：请求范围完成；所声称的修改可从本地事实证明；所声称的验证真实运行；provider 终态明确为成功。

## 隐私边界

禁止上传：绝对路径、完整 diff、原始 stdout/stderr、模型内部推理、Codex token、cookie、环境变量、未脱敏代码。

允许上传：相对路径的受限变更摘要、命令类别与通过/失败结果、阶段、脱敏错误、短摘要，以及有明确保留期的 device-safe 回复分页和设备图片衍生物。

Device 侧：`text` 是 Desktop 生成的完整 device-safe 用户可见回复的一个分页，不含源码、diff、绝对路径、命令原文、内部推理或日志；Device 最多保留最近八轮。图片只允许 `overview`/`top`/`bottom` 预生成 RGB565 兼容衍生物，不接收任意 URL 或原始本地图片路径。

## 风险能力分级

```text
R0 read/explain/search existing project data
R1 edit authorized project and run existing tests
R2 network/install/commit/external write/delete/migration
R3 push/publish/deploy/secrets/system settings/history rewrite
```

MVP 只允许本地 grant 包含的 R0/R1。R2 在无可信本地审批 UI 时拒绝。R3 禁止从语音入口批准。

## MCP 进度

灵矽支持 MCP 长任务时保持 `tools/call` 进行中，Gateway 发标准 `notifications/progress`：

- `progressToken → device_id/session_id/device_wss_connection` 由灵矽维护。
- `progress` 是同 token 内单调序号，无法估算百分比时省略 `total`。
- 首条通知包含已持久化的 `task_id`（`state=queued`）。
- Gateway 只发阶段变化和短摘要，不发原始日志或内部推理。
- 最终返回标准 `CallToolResult + structuredContent`。
- MCP 断线不取消任务，恢复后用 `get_task_status` 查询。
- 只有双方协商能力后才切换 MCP Tasks 扩展。

## 数据模型（云端 vs 本地）

| 实体 | 归属 |
| --- | --- |
| `DesktopClient`、`DeviceClientBinding`、`Project`、`Task`、`Delivery`、`TaskEvent`、`PresentationDocument`、`CommandReceipt` | Gateway（PostgreSQL） |
| `AgentSession`（provider、opaque_thread_id、project/task ownership） | 仅 Desktop 本地（SQLite） |

云端 `Project` 不保存本地绝对路径；Gateway 不保存 `AgentSession` 或 provider thread/turn 标识。

## MVP 非目标

产品账号、自助配对、扫码、多 Desktop 自动选择与迁移；完整远程 IDE/终端、任意文件浏览；语音批准 push/发布/部署/密钥；云端仓库执行器；UI 自动化控制官方 Codex Desktop；不保证 app-server thread 出现在官方 Codex Desktop 侧边栏。
