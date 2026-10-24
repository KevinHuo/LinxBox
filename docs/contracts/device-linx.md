# Device / Linx 展示与输入契约

> Contract: `device-presentation/v1`  
> Status: accepted semantic baseline  
> Version: 1.0  
> Last updated: 2026-10-24  
> Wire encoding: implementation Spec 必须按真实灵矽能力冻结

该契约定义硬件需要理解的语义对象。它不替代 MCP：MCP 控制任务，Presentation Bridge 同步有界内容和回执。Issue #4 的离线数据是 UI/状态机验收 fixture，不是联网成功证据。

## 1. Envelope

```json
{
  "contract_version": "device-presentation/v1",
  "message_id": "msg_01H...",
  "sequence": 42,
  "type": "task.snapshot",
  "sent_at": "2026-10-24T01:00:00Z",
  "payload": {}
}
```

同一设备连接内 `sequence` 单调递增；`message_id` 全局唯一并用于去重。发现缺口、未知主版本或缓存 revision 不连续时，Device 请求 snapshot，不猜测中间状态。

## 2. 任务卡与快照

```json
{
  "revision": 8,
  "tasks": [{
    "task_id": "tsk_01H...",
    "title": "登录错误修复",
    "agent_label": "Codex",
    "state": "running",
    "stage": "testing",
    "summary": "正在运行相关测试。",
    "unread": true,
    "updated_at": "2026-10-24T01:00:00Z"
  }]
}
```

类型为 `task.snapshot` 或 `task.delta`。Device 不从排序、未读或连接状态推断 Task 终态。任务数量、标题和摘要上限必须由实现 schema 根据设备内存预算固定。

## 3. 回复内容

请求使用 `content.request`：

```json
{"task_id":"tsk_01H...","turn_id":"trn_01H...","page":0}
```

响应使用 `content.page`：

```json
{
  "task_id": "tsk_01H...",
  "turn_id": "trn_01H...",
  "speaker": "agent",
  "page": 0,
  "page_count": 3,
  "text": "已完成登录空值检查……",
  "image_ids": ["img_01H..."]
}
```

`text` 是 Desktop 生成的完整 device-safe 用户可见回复的一个分页，不包括源码、diff、绝对路径、命令原文、内部推理或日志。Device 最多保留最近八轮；服务端保留与分页上限由 implementation Spec 固定。

## 4. 图片

`image.manifest` 至少包含 `image_id/task_id/turn_id/name/width/height/byte_size/sha256/variants`。Device 只请求 `overview/top/bottom` 等预生成 RGB565 兼容衍生物；不接收任意 URL、原始本地图片路径或超预算内容。

具体分块大小、压缩、重传和缓存淘汰依赖真实灵矽通道能力，未由本语义基线假定。实施前必须通过 Spec 冻结并在 8 MB Flash、无 PSRAM 设备验证。

## 5. 草稿与确认

```json
{
  "command_id": "cmd_01H...",
  "draft_id": "dft_01H...",
  "target": {
    "task_id": "tsk_01H...",
    "turn_id": "trn_01H...",
    "image_id": "img_01H..."
  },
  "transcript": "把图片上半部分的问题修一下"
}
```

- `draft.submit` 只保存草稿，不执行；
- `draft.confirm` 必须引用同一 draft/command/target，才可进入控制链路；
- `draft.cancel` 不产生 Task/Turn；
- 超时等同停留在确认态，不等同 confirm；
- 每 Task 最多一条未终结 command；重试复用原 `command_id`。

## 6. 回执

`command.receipt.stage`：

```text
sending -> desktop_received -> queued -> agent_accepted -> running
        -> turn_completed | failed | cancelled | interrupted
```

`turn_completed` 只表示本轮结束；Task 是否 `succeeded` 由 Task 状态字段单独表达。Receipt 必须携带 `command_id/task_id/turn_id/receipt_sequence/summary`，按序去重，不能把网络发送成功映射为 Agent 接受。

## 7. 错误与恢复

错误使用稳定 code：`OFFLINE`、`STALE_REVISION`、`TARGET_NOT_FOUND`、`DUPLICATE`、`QUEUE_FULL`、`CONTENT_UNAVAILABLE`、`PERMISSION_DENIED`、`POLICY_DENIED`、`INTERNAL`；公共 code 见 [MCP 工具契约](./mcp.md) §10。

恢复在线只请求 snapshot 并重放未 ACK 的读取请求；不得自动重发未确认 draft 或失败 command。恢复初始场景是本地显式操作，必须清理旧 timer/event，不能影响 Gateway 的真实 Task。
