# Gateway/Desktop WSS 契约

> Protocol version: `1.0`  
> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24

## 1. 连接

Desktop 主动连接：

```http
GET /v1/clients/connect
Authorization: Bearer <client_token>
Upgrade: websocket
```

Gateway 根据 token hash 确定 client；不接受 query、Header 或 hello payload 覆盖 `client_id`。TLS 是公网部署的必需条件。

## 2. 可靠消息 Envelope

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

| 字段 | 规则 |
| --- | --- |
| `protocol_version` | `major.minor`；不兼容 major 必须 fail closed |
| `message_id` | 全局唯一；接收方先持久化 disposition 再 ACK |
| `sequence` | 对同一 client、同一发送方向单调递增；Gateway 与 Desktop 各自持久化自己的序列 |
| `type` | 已注册消息类型；未知安全消息拒绝 |
| `sent_at` | UTC ISO 8601 |
| `expires_at` | 仅限可过期命令；过期 `task.dispatch` 不执行 |
| `payload` | 按 `type` 严格校验 |

`client.hello`、`server.hello`、`client.heartbeat`、`task.ack` 和 `client.ack` 是连接控制/确认帧，不进入自身待确认 outbox，避免 ACK-of-ACK 循环。其余有业务事实的消息均使用本 Envelope：

- Gateway -> Desktop 的 `sequence` 称为 server sequence；
- Desktop -> Gateway 的 `sequence` 称为 client sequence；
- 两个方向独立递增、独立 ACK、独立重放，不能用一个方向的游标推断另一个方向已送达。

## 3. 客户端会话消息

### `client.hello`

Desktop -> Gateway：

```json
{
  "desktop_version": "1.0.0",
  "protocol_versions": ["1.0"],
  "last_acked_server_sequence": 1042,
  "capabilities": ["project_registry", "codex_app_server", "task_cancel"]
}
```

Gateway 选择一个兼容版本。无兼容版本时发送 `protocol.upgrade_required` 并关闭连接。兼容时返回 `server.hello`：

```json
{
  "protocol_version": "1.0",
  "last_acked_client_sequence": 76
}
```

Desktop 从 `last_acked_server_sequence + 1` 接收 Gateway 重放；Desktop 从 `last_acked_client_sequence + 1` 重放本地 outbox。

### `client.heartbeat`

包含当前时间和可选负载摘要，不携带任务真实状态。缺少 heartbeat 只改变 presence，不直接改变 Task 终态。

## 4. 项目消息

### `project.upsert`

```json
{
  "project_id": "prj_01H...",
  "display_name": "商城前端",
  "capabilities": ["read", "write", "test"],
  "availability": "online",
  "grant_version": 3
}
```

### `project.revoke`

```json
{"project_id":"prj_01H...","grant_version":4,"revoked_at":"2026-10-24T01:00:00Z"}
```

绝对路径永不出现在该协议中。

## 5. 任务命令

### `task.dispatch`

```json
{
  "task_id": "tsk_01H...",
  "turn_id": "trn_01H...",
  "project_id": "prj_01H...",
  "agent": "codex",
  "capabilities": ["read", "write", "test"],
  "prompt": "修复登录页错误并运行测试",
  "acceptance_criteria": ["空值提交不再抛错"],
  "policy_version": "2026-10-24",
  "continuation": null,
  "command_context": null
}
```

`continuation` 为 null 或：

```json
{"parent_task_id":"tsk_parent"}
```

Gateway 为每个 Task 创建公开 `turn_id`；续接只传父任务 ID，并为子 Task 创建新的公开 `turn_id`。Desktop 根据本地持久化的任务所有权记录解析 provider thread/turn；provider 标识不得上传到 Gateway，也不得由调用方指定。

来自 Device 确认时，`command_context` 为：

```json
{
  "command_id": "cmd_01H...",
  "draft_id": "dft_01H...",
  "target": {
    "task_id": "tsk_parent",
    "turn_id": "trn_public_01H...",
    "image_id": "img_01H..."
  },
  "target_revision": 8
}
```

该结构只能由 Gateway 从已验证的 Presentation Bridge 上下文产生，不能由模型或 MCP `arguments` 提供。Gateway 在投递前校验 command、父 Task、公开 Turn、图片和 revision 的所有权；Desktop 在创建 provider turn 前再次校验本地 ownership、grant 和 `command_id` 去重。`turn_id` 是 LinxBox 公开标识，不是 provider turn ID。

### `task.cancel`

```json
{"task_id":"tsk_01H...","reason":"user_requested"}
```

取消是请求，不是终态证明。

## 6. 双向 ACK、Outbox 与重放

### 6.1 Gateway -> Desktop

Desktop -> Gateway。以下仅展示 envelope 的 `type` 和 `payload`；其余共通字段仍必须存在：

```json
{
  "type": "task.ack",
  "payload": {
    "message_id": "msg_01H...",
    "sequence": 1043,
    "task_id": "tsk_01H...",
    "disposition": "accepted"
  }
}
```

`disposition`: `accepted | duplicate | expired | rejected`。`rejected` 必须包含脱敏 `PublicError`。

ACK 只证明 Desktop 已持久化 disposition 并处理该 Delivery；`accepted` 不等于 started 或 succeeded。重连后 Gateway 从 `last_acked_server_sequence + 1` 重放未 ACK 消息。Desktop 对相同 `message_id` 返回原 disposition；已接受的重复投递使用 `duplicate`，不得创建第二个 Codex turn。

`task.ack(rejected|expired)` 是执行副作用前的终态事实：Gateway 分别把 Task 从 `dispatched` 收敛到 `rejected|expired`。Desktop 已发送 `accepted` 后的明确准备失败使用 `task.failed`；无法证明 side effect 或终态时使用 `task.interrupted`。

### 6.2 Desktop -> Gateway

`project.*`、`task.*` 事件、`presentation.*` 和 `command.receipt` 必须先写入 Desktop 本地 outbox，再发送。Gateway 在同一事务内持久化业务事实与 client sequence disposition，然后返回：

```json
{
  "type": "client.ack",
  "payload": {
    "message_id": "msg_client_01H...",
    "sequence": 77,
    "disposition": "accepted"
  }
}
```

`disposition`: `accepted | duplicate | rejected`。Desktop 只有收到 `accepted|duplicate` 才能清理对应 outbox 记录；`rejected` 保留本地事实并停止越过该 sequence 发送后续有因果关系的记录，直到人工处理或兼容升级。

重连时 Gateway 通过 `server.hello.last_acked_client_sequence` 返回已持久化的连续 client sequence；Desktop 从下一条开始按原 `message_id/sequence/event_id` 重放。Gateway 对重复记录返回 `duplicate`，对 sequence 缺口拒绝越过缺口处理。最终 Task 事件、Presentation revision 和 command receipt 与普通进度使用相同可靠规则，不能只依赖 heartbeat 或去重 ID 假定已经送达。

Gateway 持久化成功但 ACK 丢失时，重放收敛为 `duplicate`；Gateway 落库前崩溃时，Desktop outbox 保留并重放；Desktop 发送前崩溃时，重启扫描 outbox 后继续。双方不得因连接 presence 改写 Task 终态。

## 7. 任务事件

Desktop -> Gateway 事件类型：

```text
task.started
task.progress
task.approval_required
task.completed
task.failed
task.cancelled
task.interrupted
```

共通 payload：

```json
{
  "task_id": "tsk_01H...",
  "turn_id": "trn_01H...",
  "event_id": "evt_01H...",
  "event_sequence": 7,
  "occurred_at": "2026-10-24T01:05:00Z",
  "state": "running",
  "stage": "testing",
  "summary": "正在运行相关测试。",
  "result": null,
  "error": null
}
```

`event_sequence` 对单 Task 单调递增。最终事件只能出现一次；重放使用相同 `event_id`。`task.completed.result` 必须符合 `task-result-v1`。

## 8. Device-safe 展示投影

Desktop -> Gateway：

```text
presentation.upsert
presentation.revoke
command.receipt
```

`presentation.upsert` 携带 `task_id/turn_id/revision/pages/image_manifests/retention_until`，其页面和图片必须已通过 Desktop `result-projector`，符合 [`device-presentation/v1`](device-linx.md) 的内容限制。Gateway 按 device binding 投影，不接受 Device 或模型指定 client。

`command.receipt` 只追加单调 receipt；`desktop_received` 证明 Desktop 已持久化 command，`turn_completed` 证明本轮终结，均不替代 Task terminal event。

## 9. 本地与云端隐私分界

禁止上传：绝对路径、完整 diff、原始 stdout/stderr、模型内部推理、Codex token、cookie、环境变量和未脱敏代码。

允许上传：相对路径的受限变更摘要、命令类别与通过/失败结果、阶段、脱敏错误、短摘要，以及有明确保留期的 device-safe 回复分页和设备图片衍生物。
