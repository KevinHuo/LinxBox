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

## 2. Envelope

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
| `message_id` | 全局唯一；Desktop 先持久化再 ACK |
| `sequence` | Gateway 对单个 client 单调递增 |
| `type` | 已注册消息类型；未知安全消息拒绝 |
| `sent_at` | UTC ISO 8601 |
| `expires_at` | 仅限可过期命令；过期 `task.dispatch` 不执行 |
| `payload` | 按 `type` 严格校验 |

## 3. 客户端会话消息

### `client.hello`

Desktop -> Gateway：

```json
{
  "desktop_version": "1.0.0",
  "protocol_versions": ["1.0"],
  "last_acked_sequence": 1042,
  "capabilities": ["project_registry", "codex_app_server", "task_cancel"]
}
```

Gateway 选择一个兼容版本。无兼容版本时发送 `protocol.upgrade_required` 并关闭连接。

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
  "project_id": "prj_01H...",
  "agent": "codex",
  "capabilities": ["read", "write", "test"],
  "prompt": "修复登录页错误并运行测试",
  "acceptance_criteria": ["空值提交不再抛错"],
  "policy_version": "2026-10-24",
  "continuation": null
}
```

`continuation` 为 null 或：

```json
{"parent_task_id":"tsk_parent"}
```

Gateway 只传父任务 ID。Desktop 根据本地持久化的任务所有权记录解析 thread/turn；provider 标识不得上传到 Gateway，也不得由调用方指定。

### `task.cancel`

```json
{"task_id":"tsk_01H...","reason":"user_requested"}
```

取消是请求，不是终态证明。

## 6. ACK 与重放

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

ACK 只证明 Desktop 已持久化并处理该 Delivery；`accepted` 不等于 started 或 succeeded。重连后 Gateway 从 `last_acked_sequence + 1` 重放未 ACK 消息。Desktop 对相同 `message_id` 返回 `duplicate`，不得创建第二个 Codex turn。

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
