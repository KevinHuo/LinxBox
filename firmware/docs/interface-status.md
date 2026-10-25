# 语音确认与三端接口实现清单（历史审计快照）

> 版本提示：下文保留 2026-10-25 的历史审计，不能直接用于判定当前主线。后续主线已实现 Gateway 确认投递、任务事实与 unknown 恢复，Desktop 目录应答等待及成功 TaskResult 也已有实现。当前流程与限制以 [Gateway 确认执行说明](../../gateway/docs/execution.md)、[Desktop 说明](../../desktop/README.md)和[固件接入说明](gateway-client.md)为准；历史“未实现”项不代表当前仍未实现。

审计日期：2026-10-25。依据是本仓库的共享契约与运行源码，覆盖固件、Gateway、Desktop，以及灵矽 MCP 接入依赖。实际绑定的是另一台电脑，该电脑的客户端、Codex 和 Gateway 部署版本未经本次核对；以下不能作为远端运行状态报告。

本轮范围是补齐固件“说完按一次 OK，结束录音，并在原文就绪后发送 `request.confirm`”的现有协议接线，同时记录其他缺口。Gateway 调度、Desktop 修复、部署及烧录需要各自实现与验收；本清单不定义新协议、不代替跨模块设计评审。

已重新核对 [Issue #14](https://github.com/KevinHuo/LinxBox/issues/14)（2026-10-25 更新）。其中现状快照基于 `df045c8`，早于本分支的 Gateway 固件接入；不能据该快照将现有固件接口判定为全部未接入。本轮一次 OK 授权是用户对原设计二次确认交互的明确调整，沿用现有字段、固定目标和失败恢复语义。

共享字段以 [contracts/README.md](../../contracts/README.md)、[固件消息 Schema](../../contracts/firmware-gateway.schema.json)、[Desktop 消息 Schema](../../contracts/desktop-gateway.schema.json) 和 [MCP Schema](../../contracts/mcp.schema.json) 为准。状态说明如下：

- **已实现**：存在实际运行代码路径，不代表真实设备、灵矽、Gateway、另一台电脑的完整链路已通过验收。
- **部分实现**：可以接收或发送，但业务语义、结果、可靠性仍有缺口。
- **未实现**：只有契约、类型、校验器、占位返回或日志，不能完成对应业务。

## 固件

接口实现在 [linx_gateway_client.c](../main/linx_gateway_client.c)，设备与语音接线见 [main.c](../main/main.c)、[linx_device.c](../main/linx_device.c) 和 [Gateway 接入说明](gateway-client.md)。

| 接口或操作 | 状态 | 实现与依赖 |
| --- | --- | --- |
| `device.hello`、`device.bound` | 已实现 | 读取或接收绑定；MAC 是定位信息，连接仍须使用设备凭据。 |
| `agent_sessions.list`、`agent_session.get`、`agent_sessions.updated` | 已实现 | 目录分页、选中会话与通知刷新；依赖 Gateway 已保存的 Desktop 目录。 |
| `messages.list`、`message.content.get`、`message.available` | 已实现 | 读取历史与正文分块、按通知刷新；内容来自绑定 Desktop，仍受 Desktop 的历史兼容与分页限制。 |
| `request.prepare` | 已实现 | 录音前固定本轮 `request_id` 与目标，收到成功快照后才开始采集。 |
| `request.proposed`、`request.updated`、`request.get` | 已实现 | 读取并匹配本轮快照；中断后通过查询核对，不把读取成功当成任务已执行。 |
| `request.cancel` | 已实现 | 返回手势取消确认前请求；不能据此宣称撤回已执行任务。 |
| `request.confirm` | 本轮补齐固件接线 | OK 结束上传并记录本轮确认意图；原文仍为 `awaiting_text` 时等待，进入 `awaiting_confirmation` 后发送现有接口，不增加二次确认页面。Gateway 当前仍明确拒绝调度，见下节。 |
| 最终 ASR 与 MCP | 外部依赖待验收 | 固件不直接把识别文字投递给 Gateway；必须由灵矽本轮会话携带正确 `request_id` 调用 MCP。 |

`listen.stop` 结束语音上传，`request.confirm` 确认 Gateway 已保存的本轮文字，两者是不同连接上的操作。一次 OK 可以安排这两个步骤，但不能在原文到达之前把请求标为已确认、已投递或已执行。等待和播放阶段重复按 OK 不能产生另一条指令。

## Gateway

固件消息分发见 [transport/firmware.go](../../gateway/internal/transport/firmware.go)，Desktop 消息分发见 [transport/desktop.go](../../gateway/internal/transport/desktop.go)，持久化请求逻辑见 [service/request.go](../../gateway/internal/service/request.go)。

| 接口或操作 | 状态 | 当前行为与缺口 |
| --- | --- | --- |
| `device.hello`、`device.bind`、`binding.result`、`device.bound` | 已实现 | 校验认证主体与配对权限，保存绑定，向在线设备通知。 |
| `agent_sessions.sync`、`.sync.result`、`agent_sessions.list/get`、`agent_sessions.updated` | 已实现 | SQLite 原子替换目录、返回保存结果并通知固件；游标失效后重新读取首页。 |
| `messages.list`、`message.content.get`、`query.result` | 已实现路由 | 核实归属后转发到绑定 Desktop，关联原查询、连接代次与超时；不自行读取 Codex，也不保存正文。 |
| `message.available` | 已实现转发 | 校验 Desktop 的会话归属后通知在线设备；未实现离线消息队列。 |
| `request.prepare/get/cancel` | 已实现 | 保存固定目标和原文快照、确认前取消、等待文字 120 秒与等待确认 300 秒超时。 |
| `request.proposed`、`request.updated` | 部分实现 | 原文保存、取消和上述超时会推送；尚无任务执行事实驱动的状态更新。 |
| **`request.confirm`** | **未实现业务** | `transport/firmware.go` 的该分支固定返回 `DEPENDENCY_UNAVAILABLE`，说明“任务调度尚未实现，请求未执行”；不会发送任务。 |
| **`task.dispatch`** | **未实现** | 仅存在 Gateway → Desktop 契约和出站校验，没有任务创建、确认去重、投递持久化或发送业务。 |
| **`task.ack`** | **未实现业务** | 接受符合 Schema 的消息后仅记录日志；未处理 accepted、duplicate、expired、rejected。 |
| **`task.started/progress/approval_required/completed/failed/cancelled/interrupted`** | **未实现业务** | 七类事件均只记录日志；不保存事件或 TaskResult，不校验任务映射和事件顺序，不推进 Request。 |
| `task.cancel` | 未实现发送 | 只有出站契约和校验入口；首版设备没有执行中断入口。 |
| **`project.upsert/revoke`** | **未实现发送** | 只有 Gateway → Desktop 契约和校验入口，没有项目授权记录、版本管理与通知业务；现有会话目录不能替代完整授权策略。 |
| `client.hello/heartbeat` | 部分实现 | 仅记录消息日志，不保存客户端声明的能力与业务恢复状态。 |
| **执行状态 `unknown` 与恢复** | **未实现业务** | Schema 与忙碌索引允许该状态，但断线、重启和任务事实均未接入；`request.get` 只返回原快照。 |
| MCP `propose_task_reply` | 已实现 Gateway 接收 | 独立凭据、来源与逐 POST 上下文校验、原文保存及重复调用检查；唯一工具只接收文字，**不投递 Desktop 任务**。 |

已存在的持久化表包括绑定、会话目录和录音请求；[requests 表](../../gateway/internal/storage/migrations/002_requests.sql) 保存设备、Desktop、会话、thread、project、标题、原文、期限和状态，**没有 request → task 映射、投递记录或执行事件表**。不能仅将 `request.confirm` 的错误返回改成成功就称为接口完成。

按既有契约完成确认执行，至少还依赖以下工作，具体方案应通过独立跨模块设计 Issue/PR 评审：

1. 确认时重新校验绑定和固定目标，原子保存同一 request 对应的稳定 task 与投递身份；重复确认不得重复执行。
2. 从明确的授权来源取得 `task.dispatch` 必填的 `agent/capabilities/policy_version`，使用固定的 `{agent_session_id,thread_id}` 续接，不能通过 `continuation:null` 回退到新任务，也不能硬编码全部能力冒充授权。
3. 持久化并核对 ACK、`event_id/event_sequence`、任务与来源；ACK accepted/duplicate 不等于 running，只有 `task.started` 才推进运行。
4. 只有 `task.completed` 且 `TaskResult.status=succeeded` 才推进 completed；其他终态依真实事实处理。断线、重启后结果不确定应保留 unknown，不自动重投。

可以复用现有 `RequestService` 的绑定校验、事务、固定目标和提交后通知，以及 `RequestRepo.Busy` 的设备/会话串行约束。现有 `request.get` 仅用于读取服务端事实，不能代替缺失的执行事实。

## Desktop

Electron 已实例化 Gateway 连接，见 [main/index.ts](../../desktop/main/index.ts) 和 [main/gateway-connection.ts](../../desktop/main/gateway-connection.ts)；不能根据较旧说明将整个 Desktop 接入判定为未实现。

| 接口或操作 | 状态 | 当前行为与缺口 |
| --- | --- | --- |
| `client.hello/heartbeat`、`device.bind/binding.result` | 已实现运行路径 | 连接、心跳、绑定请求和应答匹配已接入；实际远端版本未知。 |
| `agent_sessions.sync/result` | 部分实现 | 能发布目录，但未等待匹配的保存应答；同步期间新变化未保证合并到下一份目录；枚举超时可能以空列表兜底，与“失败不能清空目录”的契约不符。 |
| `messages.list`、`query.result` | 部分实现 | 当前忽略 cursor，将轮次展平后截取 16 条，按原时间顺序返回，`next_cursor` 恒为 null；没有按最新到最旧、最多八轮及完整轮次边界分页，也未明确过滤全部中间消息。 |
| `message.content.get`、`query.result` | 已实现读取路径 | 从已发布会话获取正文并按 UTF-8 字节分块；仍依赖 Codex 历史读取能力。 |
| `message.available` | 已实现运行路径 | 终态后尝试读取最终回复并通知 Gateway；断线时没有可靠补发。 |
| `project.upsert/revoke` | 部分实现 | 可接收并更新内存元数据/可用性；Gateway 当前不发送。执行侧尚未完整校验 `capabilities/policy_version/agent`，默认目录与续接回退也可能掩盖授权目标缺失。 |
| `task.dispatch`、`task.ack` | 部分实现 | 可以启动或续接 Codex 并发送 ACK；运行路径去重只在内存且按 message ID，未贯通 task ID、内容冲突、过期和序号检查，重连/重启不能提供持久化去重保证。 |
| `task.started/progress/failed/interrupted` | 已实现发送路径 | Dispatcher 可生成并发送这些事实；Gateway 尚不处理，Desktop 断线也没有事件队列补发。 |
| **`task.completed`** | **部分实现** | 会发送完成事件，但 `result` 恒为 `null`；不足以满足 Gateway 根据 `TaskResult.status=succeeded` 判定 completed 的条件。 |
| `task.cancel` | 部分实现 | 会调用 Codex interrupt；ACK 的 task ID 使用 `unknown`，中断异常未明确反馈，终态通常走 interrupted。不能称完全未实现，也不能称可靠取消完成。 |
| `task.approval_required/cancelled` | 类型存在，缺少生产路径 | 当前 Dispatcher 不生成这两种事件，Codex server request 被拒绝；首版不开放审批或执行中断入口，本轮不扩展需求。 |
| **断线、去重与执行恢复** | **未形成可靠闭环** | `GatewayClient.send()` 断线直接返回，ACK 游标仍可能前进；无持久化出站队列。独立 delivery 判断与 SQLite schema 存在，但没有接入当前执行路径。 |

关键源码定位：

- [TaskDispatcher](../../desktop/daemon/gateway-client/dispatcher.ts)：`handled/taskSessions` 内存状态、任务启动、取消以及完成事件中的 `result:null`。
- [GatewayConnectionManager](../../desktop/main/gateway-connection.ts)：每次显式连接重建 Dispatcher，项目元数据、目录同步、历史查询和任务事件转发。
- [GatewayClient](../../desktop/daemon/gateway-client/client.ts)：断线发送直接返回、ACK 游标推进。
- [delivery.ts](../../desktop/daemon/tasks/delivery.ts) 与 [local-store/schema.ts](../../desktop/daemon/local-store/schema.ts)：已有独立去重模型与 schema；它们存在不代表当前执行链路已经持久化。
- [codex-session.ts](../../desktop/main/codex-session.ts)：启动/续接时另生成请求标识，原始 dispatch 的消息身份未完整贯通到底层 Codex 调用。

## `list_turns` 错误与真实链路的证据边界

用户设备出现过 `list_turns is not supported yet`。本仓库没有该错误字符串，单凭它不能判断已配对另一台电脑的客户端版本、内部实现，或认定当前 Mac 的代码有问题。

仓库 Gateway 历史路径经 `GatewayConnectionManager.handleHistoryQuery()` 调用 `readSession()`，后者请求 `thread/read` 并设置 `includeTurns:true`；见 [Codex client](../../desktop/daemon/codex-adapter/client.ts)。另一个 UI 方法 `readConversation()` 才显式请求 `thread/turns/list`。历史读取 catch 当前将各种异常统一包装为 `NOT_FOUND` 并保留错误文字，因此该错误可能经历史查询返回，但不能仅据提示反推远端调用了哪一个方法。

确认这个兼容问题需要另行核实已绑定电脑的实际版本和对应历史读取日志，不能通过修改当前 Mac 后便宣称远端已修复。历史读取失败也不能单独证明语音未发送或任务未执行。

灵矽 MCP 入口已实现，不代表平台已保证“每轮最终 ASR 原文固定调工具并透传 `X-Linx-Request-Id`”。详见 [Gateway MCP 接入说明](../../gateway/docs/mcp.md)。听到“已发送”只证明收到语音回复；必须结合对应 request、任务 ACK 与执行事件才能确认电脑接收和执行。

主机测试、TLS mock、构建和 Schema 校验只覆盖各自验证范围。真实硬件 + 灵矽 + 已部署 Gateway + 实际绑定 Desktop 的完整执行链路仍需独立验收；本清单不声称该链路已打通。现有首版契约没有 `task.status.get`，也不开放审批授权入口，不将这些列为本轮漏做接口。
