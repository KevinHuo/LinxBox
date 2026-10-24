# ADR-0002: Desktop 使用 Codex app-server 作为远程任务执行接口

> Status: accepted  
> Date: 2026-10-24

## Context

产品需要远程启动本地编码任务并获得实时回答、命令、文件变化、取消、审批请求和终态。官方 Codex Desktop 没有公开稳定的外部控制接口；本地 MCP/插件也不能凭远端事件主动创建当前 Desktop thread/turn。

## Decision

Desktop 内置 Companion Daemon，通过本地 stdio JSON-RPC 控制 Codex app-server：

```text
task.dispatch
  → 本地持久化、去重、授权
  → thread/start 或 resume
  → turn/start
  → 标准化 agent/command/file/approval/turn 事件
  → WSS 上报 Gateway
```

`codex exec --json` 只作为开发、兼容或有界批处理 fallback。可选插件只提供 Skill、本地 MCP、Hooks 和 UI 入口；远端主动执行仍通过 Daemon + app-server。

禁止 UI 自动化、屏幕抓取、私有数据库、内部 token、假设 thread 出现在官方 Desktop，以及上传模型内部推理。

## Consequences

获得结构化生命周期和稳定的任务所有权，但需要维护 Desktop Daemon、app-server 版本/认证/崩溃/兼容和自己的任务 UI。任务不保证显示在官方 Codex Desktop。

## Acceptance

- 能 start/resume thread，并以 `turn/interrupt` 中断活动 turn；
- 用户可见文本、命令、文件变化和终态映射为稳定领域事件；
- 崩溃不虚假成功；
- 不同项目 thread 不交叉续接；
- Gateway 不接收凭据、绝对路径、完整 diff 或内部推理；
- 无可信审批 UI 时敏感请求本地拒绝。
