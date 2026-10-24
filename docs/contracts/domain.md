# 领域状态与结果契约

> Contract: `task-domain/v1` and `task-result-v1`  
> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24

## 1. 标识符

`DeviceId`、`ClientId`、`ProjectId`、`TaskId`、`MessageId`、`RequestId`、`ProviderThreadId` 和 `ProviderTurnId` 是不同值对象，禁止互换或使用裸 string 绕过类型。provider ID 只在 Desktop 本地持久化。

## 2. Task 状态

```text
requested -> rejected
    |
    v
queued -> waiting_for_client -> expired
  |             |
  +-------------+-> dispatched -> accepted -> running
                                                |-> succeeded
                                                |-> failed
                                                |-> interrupted
                                                +-> approval_required -> running/failed/cancelled

any non-terminal -> cancel_requested -> cancelled | interrupted
```

终态：`rejected | expired | succeeded | failed | cancelled | interrupted`。

不变量：

- 终态不转回活动态；
- `acknowledged != accepted != running != succeeded`；
- 无法证明成功或失败时使用 `interrupted`；
- `approval_required` 不是语音审批通道；MVP 无可信 UI 时转为拒绝或取消；
- 续接是新 turn/新执行记录，必须重新授权；
- progress 丢失不改变持久化事实。

## 3. 阶段

`stage` 是用户可见投影，不驱动领域状态转换：

```text
validating | waiting_for_client | preparing | analyzing | editing |
testing | summarizing | waiting_for_approval | finished
```

## 4. 设备状态投影

| 领域状态 | 设备状态 |
| --- | --- |
| `requested/queued/dispatched` | `queued` |
| `waiting_for_client` | `needs_attention` |
| `accepted/running/cancel_requested` | `running` |
| `approval_required` | `needs_attention` |
| `succeeded` | `completed` |
| `rejected/expired/failed/cancelled/interrupted` | `failed` |

设备投影用于展示，不反向修改 Task 状态。

## 5. Command Receipt

硬件发起的一条已确认指令使用独立 receipt 状态：

```text
sending -> desktop_received -> queued -> agent_accepted -> running
        -> turn_completed | failed | cancelled | interrupted
```

Receipt 不属于 Task 状态机。`sending` 不证明 Gateway 收到，`desktop_received` 不证明 Agent 接受，`turn_completed` 不证明整个 Task succeeded。每个 `command_id` 的 `receipt_sequence` 单调递增，重试必须复用 command identity。

## 6. 风险能力

```text
R0 read/explain/search existing project data
R1 edit authorized project and run existing tests
R2 network/install/commit/external write/delete/migration
R3 push/publish/deploy/secrets/system settings/history rewrite
```

MVP 只允许本地 grant 包含的 R0/R1。R2 在无可信本地审批 UI 时拒绝；R3 禁止从语音入口批准。

## 7. `task-result-v1`

```json
{
  "contract_version": "task-result-v1",
  "status": "succeeded",
  "summary": "修复登录表单空值错误",
  "changes": [
    {"path": "src/login.ts", "description": "提交前收窄可空字段"}
  ],
  "verification": [
    {"command": "npm test -- login", "outcome": "passed", "summary": "12 tests passed"}
  ],
  "risks": [],
  "needs_user_action": null,
  "voice_summary": "登录错误已修复，相关测试通过。"
}
```

| 字段 | 规则 |
| --- | --- |
| `status` | `succeeded | failed | cancelled | interrupted | rejected` |
| `summary` | 脱敏的用户可见摘要，不声称未完成行为 |
| `changes[].path` | 相对授权项目根；不允许 `..` 或绝对路径 |
| `verification[].command` | 用户可见的脱敏命令；必须真实执行 |
| `verification[].outcome` | `passed | failed | not_run` |
| `risks` | 未解决风险的简短列表 |
| `needs_user_action` | null 或明确、不执行高风险操作的用户动作 |
| `voice_summary` | 最长 120 个汉字；不包含路径、代码、diff、凭据或堆栈 |

`succeeded` 必须同时满足：请求范围完成；所声称的修改可从本地事实证明；所声称的验证真实运行；provider 终态明确为成功。

## 8. 公共错误

`PublicError` 结构与标准 code 集合见 [MCP 工具契约](./mcp.md) §10；本模块的领域层只补充 `details` 的额外限制。

`details` 不允许出现 token、cookie、绝对路径、原始 prompt、代码、命令输出、环境变量或 provider 内部错误对象。
