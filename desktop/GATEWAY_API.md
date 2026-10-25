# Gateway / Desktop API（当前实现整理）

本文整理 **LinxBox 当前仓库代码实际实现** 的 Gateway ↔ Desktop WebSocket API，供 Desktop 与
Gateway 联调使用。

> 本文是实现参考，不是新的共享契约，也不改变现有协议。协议版本仍为 `Gateway/Desktop 1.0`，
> 当前仓库中的 TypeScript 类型仍标记为 `Proposed`。代码声明但尚未接入运行链路的消息会明确标注，
> 不应按“已实现”使用。

## 1. 方向与角色

```text
Gateway  ── WebSocket 输入 ──>  LinxBox Desktop  ──> 本机 Codex app-server
Gateway  <─ WebSocket 输出 ──  LinxBox Desktop  <── Codex 状态与结果
```

- Desktop 永远作为 WebSocket 客户端主动连接 Gateway，不在本机监听端口。
- Gateway 只传 `project_id`，不得传本机绝对路径；Desktop 在本地解析已授权项目目录。
- 帧是单个、无换行要求的 JSON object。
- 当前协议版本固定为 `1.0`。

## 2. 连接

### 2.1 URL

- 默认路径：`/v1/clients/connect`
- 如果配置 URL 的 path 是空或 `/`，Desktop 自动替换为上述路径。
- 如果配置 URL 已带其他 path，Desktop 保留该 path。

示例：

```text
wss://gateway.example.com/v1/clients/connect
```

### 2.2 WebSocket subprotocol

当前实现不能在标准 WebSocket 构造器中设置 `Authorization` header，因此通过 subprotocol 发送协议标识和 token：

```text
Sec-WebSocket-Protocol: linxbox.v1, linxbox-token.<client_token>
```

token 不进入 URL、JSON 帧或日志。Gateway 应校验 `linxbox-token.<client_token>`；校验失败时建议以
WebSocket close code `4401` 关闭连接。

### 2.3 重连与心跳

- 连接成功后立即发送 `client.hello`。
- 默认每 15 秒发送一次 `client.heartbeat`。
- 断线后指数退避重连：默认从 1 秒开始，最大 30 秒，并附加最多 30% jitter。
- `last_acked_sequence` 当前只保存在内存，Desktop 进程重启后归零。

## 3. 公共 envelope

所有双向消息使用同一 envelope：

| 字段               | 类型    | 必填 | 当前语义                                           |
| ------------------ | ------- | ---- | -------------------------------------------------- |
| `protocol_version` | string  | 是   | 固定为 `"1.0"`                                     |
| `message_id`       | string  | 是   | 非空消息标识；入站去重以此为键                     |
| `sequence`         | integer | 是   | 非负序号                                           |
| `type`             | string  | 是   | 消息类型                                           |
| `sent_at`          | string  | 是   | ISO 8601 时间字符串                                |
| `expires_at`       | string  | 否   | 入站过期时间；当前运行链路会解析但尚未执行过期拒绝 |
| `payload`          | object  | 是   | 按 `type` 决定结构                                 |

```json
{
  "protocol_version": "1.0",
  "message_id": "msg_01H...",
  "sequence": 1043,
  "type": "task.dispatch",
  "sent_at": "2026-10-24T01:00:00Z",
  "expires_at": "2026-10-24T01:30:00Z",
  "payload": {}
}
```

## 4. Gateway → Desktop 输入

### 4.1 当前运行链路支持情况

| `type`                       | 类型中已声明 | 当前 WSS client 接收 | 当前行为                                                             |
| ---------------------------- | ------------ | -------------------- | -------------------------------------------------------------------- |
| `task.dispatch`              | 是           | **是**               | 校验后交给 `TaskDispatcher`，启动本机 Codex turn                     |
| `task.cancel`                | 是           | **是**               | 当前只返回 `accepted` disposition，尚未真正 interrupt Codex          |
| `agent_sessions.sync.result` | 是           | **是**               | 回答 Desktop 发出的会话目录全量同步；不进入任务 dispatcher           |
| `project.upsert`             | 是           | 否                   | 另一套严格 validator 可解析，但当前 `GatewayClient` 会当未知类型拒绝 |
| `project.revoke`             | 是           | 否                   | 另一套严格 validator 可解析，但当前 `GatewayClient` 会当未知类型拒绝 |

Gateway 当前联调时可下发 `task.dispatch`、`task.cancel`，并回答
`agent_sessions.sync.result`。

### 4.2 `task.dispatch`

payload：

| 字段                  | 类型           | 必填 | 约束/语义                                                                        |
| --------------------- | -------------- | ---- | -------------------------------------------------------------------------------- |
| `task_id`             | string         | 是   | 非空任务 ID                                                                      |
| `project_id`          | string         | 是   | 非空项目 ID；由 Desktop 映射本机路径                                             |
| `agent`               | string         | 是   | 非空；当前预期 `codex`，但运行时未限制枚举                                       |
| `capabilities`        | string[]       | 是   | 元素只能是 `read`、`write`、`test`；当前只作审计信息透传，Desktop 不据此限制执行 |
| `prompt`              | string         | 是   | 可以为空字符串；当前直接传给 Codex                                               |
| `acceptance_criteria` | string[]       | 是   | 验收条件列表                                                                     |
| `policy_version`      | string         | 是   | 非空策略版本；当前只作审计信息透传，不参与执行授权判断                           |
| `continuation`        | object \| null | 是   | 新任务为 `null`；续接任务为 `{ "parent_task_id": "..." }`，用于定位父任务会话    |

完整示例：

```json
{
  "protocol_version": "1.0",
  "message_id": "msg_gw_0001",
  "sequence": 1043,
  "type": "task.dispatch",
  "sent_at": "2026-10-24T01:00:00Z",
  "expires_at": "2026-10-24T01:30:00Z",
  "payload": {
    "task_id": "tsk_0001",
    "project_id": "prj_0001",
    "agent": "codex",
    "capabilities": ["read", "write", "test"],
    "prompt": "修复登录页空值提交错误并运行测试",
    "acceptance_criteria": ["空值提交不再抛错", "相关测试通过"],
    "policy_version": "2026-10-24",
    "continuation": null
  }
}
```

当前处理规则：

1. 同一 `message_id` 再次出现，不启动第二个 Codex turn，返回 `duplicate`。
2. `project_id` 在本机不存在，返回 `rejected`，原因“项目未授权”。
3. 本地项目已停用，返回 `rejected`，原因“项目授权已撤销”。
4. Codex 启动成功，产生 `task.started`，并返回 `accepted` ack。
5. Codex 启动失败，产生 `task.failed`，并返回 `rejected` ack。
6. 去重记录当前只在内存中，Desktop 重启后不会保留。
7. `capabilities` 与 `policy_version` 当前不参与执行判断，只随包记录。Gateway 不能因为下发了
   `["read"]` 就认为 Desktop 只读执行。
8. `continuation` 不为 `null` 时，Desktop 用 `parent_task_id` 查本机已执行的父任务，并逐项校验：
   父任务必须属于本次请求的 `project_id`；该项目当前仍然授权；目标会话记录的工作目录必须落在该授权
   项目目录内。任一项不满足即返回 `rejected`（“续接目标不属于本次授权项目”“原任务项目授权已撤销”
   “目标会话不属于本次授权项目”“目标会话没有可校验的工作目录”），**不会回落到新建会话**。
9. `task.started` 及其后的事件都会带上实际执行的 `thread_id` 与 `turn_id`，Gateway 用它定位本机会话。
10. 父任务映射当前只在内存中。**Desktop 进程重启后不支持恢复父任务续接**，续接请求会因父任务不存在而
    返回 `rejected`；Gateway 不得通过重放原始执行任务来重建映射——重放产生的是一次新的执行，不是恢复原
    会话身份。持久映射与状态核对／恢复机制见 #11 / #14。
11. 本进程内的父任务续接只覆盖“由本机执行过的任务”，不等于支持直接选择 Desktop 上的任意已有会话。

### 4.3 `task.cancel`

payload：

| 字段      | 类型   | 必填 | 语义                   |
| --------- | ------ | ---- | ---------------------- |
| `task_id` | string | 是   | 非空任务 ID            |
| `reason`  | string | 是   | 取消原因，可为空字符串 |

```json
{
  "protocol_version": "1.0",
  "message_id": "msg_gw_0002",
  "sequence": 1044,
  "type": "task.cancel",
  "sent_at": "2026-10-24T01:05:00Z",
  "payload": {
    "task_id": "tsk_0001",
    "reason": "用户取消"
  }
}
```

当前限制：该消息现在只得到 `accepted` ack，尚未调用 Codex interrupt，也不会产生可靠的
`task.cancelled` 终态。Gateway 不能把 ack 的 `accepted` 当作任务已经取消。

### 4.4 已声明但当前不可下发的项目消息

以下 payload 已存在于类型契约，但尚未接入当前 `GatewayClient` 运行路径。

`project.upsert`：

```json
{
  "project_id": "prj_0001",
  "display_name": "LinxBox",
  "capabilities": ["read", "write", "test"],
  "availability": "online",
  "grant_version": 1
}
```

`project.revoke`：

```json
{
  "project_id": "prj_0001",
  "grant_version": 2,
  "revoked_at": "2026-10-24T01:10:00Z"
}
```

## 5. Desktop → Gateway 输出

### 5.1 当前实现情况

| `type`                   | 当前构造器 | 当前真实发送路径  | 说明                                                        |
| ------------------------ | ---------- | ----------------- | ----------------------------------------------------------- |
| `client.hello`           | 是         | **GatewayClient** | WebSocket open 后立即发送                                   |
| `client.heartbeat`       | 是         | **GatewayClient** | 默认每 15 秒发送                                            |
| `task.ack`               | 是         | **GatewayClient** | dispatcher 判断 delivery 后发送                             |
| `task.started`           | 是         | **E2E wiring**    | dispatcher 已产生，当前 Electron main 尚未接 Gateway client |
| `task.progress`          | 是         | **E2E wiring**    | Codex commentary/final 摘要映射为进度                       |
| `task.completed`         | 是         | **E2E wiring**    | 仅 provider 明确 completed 时产生                           |
| `task.failed`            | 是         | **E2E wiring**    | 启动失败、provider failed/error                             |
| `task.interrupted`       | 是         | **E2E wiring**    | provider 无法证明成功时产生                                 |
| `task.cancelled`         | 是         | 尚未产生          | 类型与构造器存在                                            |
| `task.approval_required` | 是         | 尚未产生          | 类型与构造器存在                                            |
| `agent_sessions.sync`    | 是         | **Electron main** | 全量发布已授权项目内的 Codex 会话目录，不含本机绝对路径     |
| `presentation.upsert`    | 仅类型声明 | 否                | 未接入                                                      |
| `presentation.revoke`    | 仅类型声明 | 否                | 未接入                                                      |
| `command.receipt`        | 仅类型声明 | 否                | 未接入                                                      |

Electron main 当前已实例化 `GatewayClient` 和 `TaskDispatcher`。真实云端 Gateway、固件侧目录读取与
MCP 路由仍未实现；`agent_sessions.sync` 目前只与本机 mock Gateway 完成联调。

### 5.2 `client.hello`

```json
{
  "protocol_version": "1.0",
  "message_id": "msg_desktop_0001",
  "sequence": 1,
  "type": "client.hello",
  "sent_at": "2026-10-24T01:00:00Z",
  "payload": {
    "desktop_version": "0.1.0",
    "protocol_versions": ["1.0"],
    "last_acked_sequence": 0,
    "capabilities": ["project_registry", "codex_app_server"]
  }
}
```

`capabilities` 当前是构造 `GatewayClient` 时传入的自由字符串列表，不是固定枚举。示例中不含
`task_cancel`：当前版本不提供执行中断，Desktop 不应对外宣称该能力。

### 5.3 `client.heartbeat`

```json
{
  "protocol_version": "1.0",
  "message_id": "msg_desktop_0002",
  "sequence": 2,
  "type": "client.heartbeat",
  "sent_at": "2026-10-24T01:00:15Z",
  "payload": {
    "client_time": "2026-10-24T01:00:15Z"
  }
}
```

payload 可选 `summary`。heartbeat 只表达在线状态，不承载任务状态事实。

### 5.4 `task.ack`

payload：

| 字段          | 类型        | 必填 | 语义                                           |
| ------------- | ----------- | ---- | ---------------------------------------------- |
| `message_id`  | string      | 是   | 被确认的入站 `message_id`                      |
| `sequence`    | integer     | 是   | 被确认的入站 `sequence`                        |
| `task_id`     | string      | 是   | 对应任务 ID                                    |
| `disposition` | string      | 是   | `accepted`、`duplicate`、`expired`、`rejected` |
| `error`       | PublicError | 否   | 拒绝时的公开错误                               |

```json
{
  "protocol_version": "1.0",
  "message_id": "msg_gw_0001",
  "sequence": 1043,
  "type": "task.ack",
  "sent_at": "2026-10-24T01:00:01Z",
  "payload": {
    "message_id": "msg_gw_0001",
    "sequence": 1043,
    "task_id": "tsk_0001",
    "disposition": "accepted"
  }
}
```

注意：当前实现的 ack envelope 也复用入站 `message_id` 和 `sequence`。`accepted` 只表示 delivery
被接受并启动处理，不代表任务已经成功完成。

### 5.5 Task event

以下消息共用同一 payload：

- `task.started`
- `task.progress`
- `task.approval_required`
- `task.completed`
- `task.failed`
- `task.cancelled`
- `task.interrupted`

| 字段             | 类型                | 必填 | 语义                                                                         |
| ---------------- | ------------------- | ---- | ---------------------------------------------------------------------------- |
| `task_id`        | string              | 是   | 任务 ID                                                                      |
| `event_id`       | string              | 是   | 事件 ID                                                                      |
| `event_sequence` | integer             | 是   | 同一 task 内从 1 递增                                                        |
| `occurred_at`    | string              | 是   | ISO 8601 时间                                                                |
| `state`          | string              | 是   | 当前任务状态                                                                 |
| `stage`          | string              | 是   | 展示阶段，不驱动状态机                                                       |
| `summary`        | string              | 是   | 可展示摘要；自由文本先移除路径、凭据、代码块、diff 和堆栈，再截断到 120 字符 |
| `result`         | TaskResult \| null  | 是   | 明确成功的终态提供本轮追问结果；其他事件可为空                               |
| `error`          | PublicError \| null | 是   | 失败/中断错误；正常事件为 `null`                                             |
| `thread_id`      | string \| null      | 是   | 本机实际执行的 Codex 会话 ID；未开始时为 `null`                              |
| `turn_id`        | string \| null      | 是   | 本机实际执行的 Codex 轮次 ID；未开始时为 `null`                              |

```json
{
  "protocol_version": "1.0",
  "message_id": "msg_desktop_0003",
  "sequence": 1001,
  "type": "task.progress",
  "sent_at": "2026-10-24T01:00:05Z",
  "payload": {
    "task_id": "tsk_0001",
    "event_id": "evt_mh5abc_2",
    "event_sequence": 2,
    "occurred_at": "2026-10-24T01:00:05Z",
    "state": "running",
    "stage": "analyzing",
    "summary": "正在检查登录表单的空值处理……",
    "result": null,
    "error": null
  }
}
```

当前 Codex 事件映射：

| Codex 事件           | Desktop 输出       | `state`       | `stage`       |
| -------------------- | ------------------ | ------------- | ------------- |
| turn 成功启动        | `task.started`     | `running`     | `preparing`   |
| commentary           | `task.progress`    | `running`     | `analyzing`   |
| final answer         | `task.progress`    | `running`     | `summarizing` |
| terminal `completed` | `task.completed`   | `succeeded`   | `finished`    |
| terminal `failed`    | `task.failed`      | `failed`      | `finished`    |
| terminal 其他/未知   | `task.interrupted` | `interrupted` | `finished`    |
| provider error       | `task.failed`      | `failed`      | `finished`    |

同一任务最多发送一个 terminal event。

### 5.6 `TaskResult`

`result` 非空时使用 `task-result-v1`：

```json
{
  "contract_version": "task-result-v1",
  "status": "succeeded",
  "summary": "修复登录页空值提交错误",
  "changes": [{ "path": "src/login.ts", "description": "增加空值校验" }],
  "verification": [{ "command": "pnpm test", "outcome": "passed", "summary": "相关测试通过" }],
  "risks": [],
  "needs_user_action": null,
  "voice_summary": "登录页空值提交问题已修复，相关测试通过。"
}
```

当前 dispatcher 在 provider 明确 completed 且没有错误时提供结构化结果，只确认本轮追问完成。changes 与 verification 暂为空数组，risk 明确提示改动与验证细节未汇总；不据此声明无改动、测试通过或整体工程已验收。

## 6. PublicError

```json
{
  "code": "POLICY_DENIED",
  "message": "项目未授权",
  "retryable": false,
  "request_id": "msg_gw_0001",
  "details": {
    "reason": "project_not_authorized"
  }
}
```

字段：

| 字段         | 类型    | 必填 | 语义                                          |
| ------------ | ------- | ---- | --------------------------------------------- |
| `code`       | string  | 是   | 公开错误码                                    |
| `message`    | string  | 是   | 可公开、脱敏的错误说明                        |
| `retryable`  | boolean | 是   | 是否允许安全重试；当前 builder 固定为 `false` |
| `request_id` | string  | 是   | 对应请求或消息 ID                             |
| `details`    | object  | 否   | 只能放脱敏标量信息                            |

公开错误码：

```text
UNAUTHENTICATED
PERMISSION_DENIED
INVALID_ARGUMENT
NOT_FOUND
CONFLICT
RESOURCE_EXHAUSTED
DEPENDENCY_UNAVAILABLE
APPROVAL_REQUIRED
POLICY_DENIED
INTERNAL
```

`details` 不得包含 token、cookie、绝对路径、原始 prompt、代码、命令输出、环境变量或 provider 内部错误。

## 7. 一次任务的当前时序

```text
Desktop -> Gateway  client.hello
Desktop -> Gateway  client.heartbeat (周期)

Gateway -> Desktop  task.dispatch
Desktop -> Codex    start thread / start turn
Desktop -> Gateway  task.started
Desktop -> Gateway  task.ack (accepted)
Desktop -> Gateway  task.progress (0..N)
Desktop -> Gateway  task.completed | task.failed | task.interrupted
```

当前 E2E wiring 中 `task.started` 可能先于 `task.ack` 到达，Gateway 不应假设 ack 一定先于 task event。

## 8. 当前联调必须注意的缺口

1. Electron 主进程尚未实例化 `GatewayClient` / `TaskDispatcher`；完整链路目前只在 E2E 脚本中接通。
2. `task.cancel` 尚未真正 interrupt Codex。
3. `expires_at` 尚未在当前 dispatch 路径执行过期判定，因此还不会产生 `expired` disposition。
4. 去重与 `last_acked_sequence` 只在内存中，进程重启会丢失。
5. `project.upsert` / `project.revoke` 尚未接入当前 WSS client。
6. `task.approval_required` / `task.cancelled` 尚无实际生产路径。
7. 成功终态已有本轮完成 TaskResult，但真实文件改动、验证与风险的完整事实投影仍未接入；其他事件的 result 仍可为 null。
8. `presentation.upsert` / `presentation.revoke` / `command.receipt` 只有类型占位。
9. 当前运行时只保留 `gateway-client/frames.ts` 作为入站 parser；类型契约仍声明
   `project.upsert` / `project.revoke`，但 parser 尚未实现，后续共享 schema 需要收敛这处差异。
10. `continuation` 已接入执行路径并校验父任务的项目归属与当前授权，但 `parent_task_id → 会话` 的映射
    只在内存中：Desktop 进程重启后不支持恢复父任务续接，Gateway 也不得靠重放原始执行任务重建映射。
    持久映射与 `conversation_id` 统一标识见 #11 / #14。
11. `capabilities` 与 `policy_version` 当前只随包透传，不参与执行授权判断。
12. `task_id → thread_id / turn_id` 的持久映射尚未实现，Gateway 侧会话标识统一为 `conversation_id` 的
    方向见 #14。

## 9. Codex 数据对 Issue #4 展示的覆盖

2026-10-24 对本机 `codex-cli 0.164.0-alpha.11.1` 做了不调用模型的只读探测。app-server
报告 179 个方法，其中以下与硬件展示直接相关的方法均可用：

```text
project/list
thread/list
thread/read
thread/turns/list
thread/items/list
thread/search
turn/start
turn/interrupt
```

### 9.1 可以直接从 Codex 获得的数据

| Issue #4 展示内容 | Codex 数据来源                                       | 当前数据是否足够 | 说明                                                                |
| ----------------- | ---------------------------------------------------- | ---------------- | ------------------------------------------------------------------- |
| 项目列表          | `project/list`                                       | 是               | 有 project ID、名称、roots 和排序位置                               |
| 任务/会话列表     | `thread/list`                                        | 是               | 有 thread ID、名称/preview、项目、状态、模型和更新时间              |
| 运行中/等待回复   | thread/turn status、`turn/started`、`turn/completed` | 是               | 可以投影为硬件状态文字                                              |
| 有新回复          | `thread/list.readStates.firstUnread`、revision       | 是               | 原始响应已有未读位置和版本信息                                      |
| 摘要              | thread title/preview、最新 agent message             | 基本够           | 没有独立语义摘要；可安全截取标题或最终回复，不应编造摘要            |
| 完整回复          | `thread/turns/list`，`itemsView: "full"`             | 是               | `agentMessage.text` 提供完整回复正文                                |
| 最近八轮历史      | turn 列表                                            | 是               | 每轮有稳定 `turnId`、状态、开始时间和 items；Desktop 取最后八轮即可 |
| 区分用户与 Agent  | `userMessage` / `agentMessage`                       | 是               | Agent message 还带 `commentary` / `final_answer` phase              |
| 回复指定对话      | thread ID + turn ID + message/item ID                | 是               | Desktop 可 `thread/resume` 后在原 thread 中 `turn/start`            |
| 命令与执行状态    | `commandExecution`                                   | 是               | 有命令、cwd、status、exit code；不应原样发给硬件                    |
| 文件改动          | `fileChange`                                         | 是               | 有 path、kind、diff；硬件只应收到脱敏摘要                           |
| MCP/搜索过程      | `mcpToolCall` / `webSearch`                          | 是               | 可投影为“正在调用工具/搜索”，不发送原始参数和结果                   |
| 生成图片/查看图片 | `imageGeneration` / `imageView`                      | 部分             | 有 item ID 和本地路径类信息，但没有可直接给设备使用的安全图片引用   |

### 9.2 当前 adapter 已经保留的数据

当前 `codex-adapter/history.ts` 已能投影：

- thread：`threadId`、title、cwd、model、created/updated time、status；
- turn：`turnId`、status、started time；
- message：用户/Agent、正文、phase；
- detail：reasoning、command、file change、MCP、web search、image view、image generation、compaction；
- 未识别 item：保留为 `other`，避免静默丢失；
- 单个 detail body 最多保留 20,000 字符，超出会明确标记截断。

### 9.3 Codex 有数据，但当前 adapter 尚未暴露

实机 `thread/list` 原始响应包含以下字段，但当前 `CodexSessionSummary` 会丢弃：

- `projectId`
- `readStates[threadId].firstUnread`
- read-state `revision`
- `canAcceptDirectInput`
- `reasoningEffort`
- `source` / `originator`
- `parentThreadId` / `forkedFromId`
- `recencyAt`

要实现硬件端“有新回复”、可靠增量更新和是否可继续输入，Desktop 需要先把其中的
`projectId`、`firstUnread`、`revision`、`canAcceptDirectInput` 加入本地只读视图，再投影成发送给
Gateway 的展示数据。

### 9.4 不能仅靠 Codex 数据完成的部分

| Issue #4 要求                        | 缺口归属                                                                          |
| ------------------------------------ | --------------------------------------------------------------------------------- |
| Gateway 已收到、已排队、电脑已收到   | Gateway / Desktop transport receipt，不是 Codex 数据                              |
| 离线队列、失败重试、发送去重         | Gateway、设备和 Desktop 的持久化状态                                              |
| 图片下载、缩略图、上下局部           | Desktop/Gateway 需要把本地图片转成脱敏、受限的设备资源；不得上传绝对路径          |
| 图片与回复目标绑定                   | 需要 LinxBox 保存 task/thread/turn/item/attachment 映射                           |
| 阅读位置、页码和当前图片不被更新抢走 | 设备本地 UI 状态机                                                                |
| 整个任务是否完成                     | Codex 只能证明当前 turn 的终态；LinxBox 必须区分 `turn_finished` 与整体 task 完成 |

### 9.5 结论

Codex 当前数据足以实现 Issue #4 中以下真实展示：

- 任务/会话列表；
- 运行状态和新回复提示；
- 摘要与完整 Agent 回复；
- 最近八轮用户/Agent 对话；
- 当前轮次的执行进度和终态；
- 图片存在及所属 Codex item 的基础信息。

因此，“把本机 Codex 数据投影给 Gateway，再供硬件显示”的主路径可行。无需读取 Codex 私有数据库，
也无需为文字历史新增 Codex 能力。真正仍需 LinxBox 自己实现的是：展示 projection、未读字段透传、
图片资源封装、transport receipt、持久去重和设备端阅读状态。

## 10. 代码来源

- 类型契约：[`contracts/gateway-desktop.ts`](contracts/gateway-desktop.ts)
- 当前 WSS client 与实际入站 parser：[`daemon/gateway-client/client.ts`](daemon/gateway-client/client.ts)、
  [`daemon/gateway-client/frames.ts`](daemon/gateway-client/frames.ts)
- task dispatch 与 Codex 事件映射：[`daemon/gateway-client/dispatcher.ts`](daemon/gateway-client/dispatcher.ts)
- task result：[`contracts/task-result.ts`](contracts/task-result.ts)
- mock Gateway：[`scripts/mock-gateway.mjs`](scripts/mock-gateway.mjs)
- 当前端到端 wiring：[`scripts/e2e-wss-codex.mjs`](scripts/e2e-wss-codex.mjs)
