# `desktop`：LinxBox Desktop UI / Daemon

> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24  
> Owner: `OWNERS.md` 中的 `desktop`

## 子模块

| 子模块 | 目录 | 责任 |
| --- | --- | --- |
| `ui` | `apps/desktop/src/main`、`apps/desktop/src/renderer` | token、连接、项目授权、任务、完整 diff、验证、风险和取消 |
| `gateway-client` | `apps/desktop/src/daemon/gateway-client` | 出站 WSS、hello、heartbeat、sequence、ACK 和恢复 |
| `project-registry` | `apps/desktop/src/daemon/project-registry` | 本地路径、不透明 project ID、grant 和撤销 |
| `policy` | `apps/desktop/src/daemon/policy` | realpath、能力交集、风险和本地最终拒绝 |
| `tasks` | `apps/desktop/src/daemon/tasks` | 去重、执行所有权、状态、取消和崩溃恢复 |
| `result-projector` | `apps/desktop/src/daemon/result-projector` | 将本地结果投影为 `local-detail`、`cloud-summary`、`device-content` 三级 |
| `codex-adapter` | `apps/desktop/src/daemon/codex-adapter` | Codex app-server 生命周期与事件到领域事件的映射，见 [`codex`](./codex.md) |
| `local-store` | `apps/desktop/src/daemon/local-store` | project、delivery、task/provider ownership、完整本地结果 |

目录以 [`docs/code-structure.md`](../code-structure.md) §2 的仓库目录树为准；新增子模块必须同时更新该目录树。

## 结果分级

1. `local-detail`：完整 diff、命令输出和本地路径，只供 Desktop 用户审阅；
2. `cloud-summary`：相对路径摘要、验证结果和脱敏错误；
3. `device-content`：无源码/路径/凭据的标题、摘要、分页用户可见回复和设备图片衍生物。

每级都由确定性 projector 产生；不能把 app-server 原始事件直接上传。用户要求“完整回复”表示完整的 device-safe 用户可见回复，不表示完整内部日志、推理或代码。

## 不变量与门禁

- 保存 `task_id <-> thread_id <-> turn_id <-> project_id`，provider ID 只在本地；
- 接到 device command 后重新检查 binding、grant、目标 task/turn/image 和风险；
- 同一 `message_id/command_id` 至多产生一次 turn；
- 路径逃逸、跨项目续接、无可信审批的 R2/R3、app-server 崩溃和恢复不确定性全部 fail closed；
- UI、Daemon、SQLite 和真实 app-server 分别提供单元、集成、崩溃恢复和目标 macOS 验收证据。
