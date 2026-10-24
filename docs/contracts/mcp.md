# MCP 工具契约

> Contract: `linxbox.mcp/v1`  
> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24

## 1. 信任边界

`installation_id`、`device_id`、`session_id` 和入口身份由 Gateway transport 从已验证的连接与 Header 注入。所有工具 `arguments` 中都禁止出现这些字段，也禁止出现 `client_id`、本地路径、cwd 或任意运行时配置。

所有 ID 是不透明字符串，调用方不得解析其前缀或猜测值。

## 2. 共通字段

| 字段 | 类型 | 规则 |
| --- | --- | --- |
| `request_id` | string | 必填；用于跟踪单次请求，不提供幂等性 |
| `idempotency_key` | string | 所有写工具必填；同一身份和工具下稳定重放原结果 |
| `task_id` | string | 只能使用 Gateway 返回的值 |
| `project_id` | string | 只能使用 `list_projects` 返回的值 |

字符串限制、枚举、未知字段拒绝和 payload 大小上限必须在实现 schema 中明确，不使用无上限字符串。

## 3. `list_projects`

请求：

```json
{"request_id":"req_01H..."}
```

结果 `structuredContent`：

```json
{
  "contract_version": "linxbox.mcp/v1",
  "projects": [
    {
      "project_id": "prj_01H...",
      "display_name": "商城前端",
      "capabilities": ["read", "write", "test"],
      "availability": "online"
    }
  ]
}
```

`availability`: `online | offline | revoked`。不返回绝对路径、Git remote 或本地用户名。

## 4. `list_agents`

请求：

```json
{"request_id":"req_01H...","project_id":"prj_01H..."}
```

结果：

```json
{
  "contract_version": "linxbox.mcp/v1",
  "agents": [
    {
      "agent": "codex",
      "display_name": "Codex",
      "availability": "online",
      "capabilities": ["read", "write", "test"]
    }
  ]
}
```

## 5. `start_task`

请求：

```json
{
  "request_id": "req_01H...",
  "idempotency_key": "opaque-key",
  "project_id": "prj_01H...",
  "agent": "codex",
  "prompt": "修复登录页错误并运行相关测试",
  "acceptance_criteria": ["空值提交不再抛错", "相关测试通过"]
}
```

语义：

1. Gateway 先验证身份、绑定、project 所有权和字段。
2. 同一幂等作用域的重复 key 返回原 `task_id`，不产生第二次执行。
3. Task 和首个 Delivery 同事务持久化后，才发送首条 progress。
4. `tools/call` 保持进行中，直到终态或 transport 超时；transport 断开不取消 Task。

首条 progress 必须包含 `task_id`、`state=queued` 和单调 `progress` 序号。最终结果符合 [`task-result-v1`](./domain.md#7-task-result-v1)。

## 6. `get_task_status`

请求：

```json
{"request_id":"req_01H...","task_id":"tsk_01H..."}
```

结果：

```json
{
  "contract_version": "linxbox.mcp/v1",
  "task": {
    "task_id": "tsk_01H...",
    "project_id": "prj_01H...",
    "state": "running",
    "stage": "testing",
    "summary": "正在运行相关测试。",
    "updated_at": "2026-10-24T01:00:00Z",
    "result": null
  }
}
```

只允许查询当前已验证 device/session 有权访问的任务。

## 7. `continue_task`

请求：

```json
{
  "request_id": "req_01H...",
  "idempotency_key": "opaque-key",
  "task_id": "tsk_01H...",
  "prompt": "把错误提示改得更清楚",
  "acceptance_criteria": ["提示区分网络失败和凭据错误"]
}
```

续接前 Gateway 和 Desktop 重新计算绑定、project grant、能力和风险。只能在同一 client、project、provider 和策略允许的范围内 resume。续接产生新 turn，但保留 task lineage。

## 8. `stop_task`

请求：

```json
{
  "request_id": "req_01H...",
  "idempotency_key": "opaque-key",
  "task_id": "tsk_01H...",
  "reason": "user_requested"
}
```

结果只能声明已记录取消请求，不得在 Desktop/provider 真正终止前返回 `cancelled`。

## 9. 进度

```json
{
  "jsonrpc": "2.0",
  "method": "notifications/progress",
  "params": {
    "progressToken": "prog_01H...",
    "progress": 4,
    "message": "正在运行相关测试",
    "_meta": {
      "linx.task": {
        "contract_version": "task-domain/v1",
        "message_id": "evt_01H...",
        "task_id": "tsk_01H...",
        "state": "running",
        "stage": "testing",
        "voice_summary": "正在运行相关测试。"
      }
    }
  }
}
```

`progress` 是同一 token 中的单调序号，不是百分比。`voice_summary` 最长 120 个汉字，不包含代码、绝对路径、命令输出或内部推理。

## 10. 错误

所有失败使用 JSON-RPC error，`error.data` 符合 `PublicError`：

```json
{
  "code": "PERMISSION_DENIED",
  "message": "当前设备无权访问该项目",
  "retryable": false,
  "request_id": "req_01H..."
}
```

公共 code：`UNAUTHENTICATED`、`PERMISSION_DENIED`、`INVALID_ARGUMENT`、`NOT_FOUND`、`CONFLICT`、`RESOURCE_EXHAUSTED`、`DEPENDENCY_UNAVAILABLE`、`APPROVAL_REQUIRED`、`POLICY_DENIED`、`INTERNAL`。
