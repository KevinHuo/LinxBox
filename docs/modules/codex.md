# `codex`：Codex app-server adapter

> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24  
> Owner: `OWNERS.md` 中的 `codex`

## 责任

本模块是 OpenAI app-server 协议和 LinxBox 领域事件之间的唯一转换层。

必须实现：

- 通过本地 stdio 启动固定兼容范围的 app-server；
- 每个连接完成 `initialize`/`initialized`；
- 新任务 `thread/start`，同项目续接 `thread/resume`，请求执行 `turn/start`；
- 使用 `turn/interrupt` 请求中断，并等待 `turn/completed` 确认终态；
- 只有 `turn/completed` 且 status 为 `completed` 才映射成功；
- 将稳定 provider 事件映射为领域 stage、result 和 PublicError；
- token 更新或进程重启后重新初始化并恢复持久化 thread ownership。

不得把实验事件、内部推理、原始 stderr、provider ID 或未经清洗的 command/file event 暴露给 Gateway 或 Device。

## 门禁

固定 app-server 版本/兼容范围；fake-process contract tests 覆盖握手、start/resume/interrupt、超时、异常退出、未知事件和终态；升级先通过旧 fixture、新 fixture 和目标 macOS 真实验收。
