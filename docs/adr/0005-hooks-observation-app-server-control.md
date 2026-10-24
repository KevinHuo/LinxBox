# ADR-0005: Hooks 只做被动观察，app-server 负责双向控制

> Status: accepted  
> Date: 2026-10-24

## 背景

Codex Hooks 可以在 Session、Prompt、Tool、Permission、Stop、Interrupt 等生命周期点运行命令或 MCP tool。社区项目 [`codex-passport`](https://github.com/JuneLeGency/codex-passport) 已验证将这些事件经本机 collector、私有 HTTP/加密 BLE 和 Android fallback 投递到 AI Passport，并提供 ACK、离线队列和真机证据。

但 Hooks 由既有 Codex 生命周期触发，不是外部创建或续接 turn 的 API。`codex-passport` 当前也明确只提供通知，不发送回复、不续接会话、不批准动作；其 transcript JSONL 轮询只是兼容 fallback，不是稳定接口。

## 决策

- LinxBox 可提供可选 `hook-observer`，采集用户在其他 Codex 客户端中启动的会话状态，并生成只读、低信任度的观察事件。
- LinxBox 自己管理的任务仍由 Desktop Daemon 通过 app-server 执行 `thread/start|resume`、`turn/start|interrupt`，并以 `turn/completed` 为执行终态依据。
- Hook 事件不能创建、续接或停止 LinxBox Task，不能批准 `PermissionRequest`，也不能把 `Stop` 直接映射为 Task succeeded。
- app-server 与 Hook 同时观察到同一 thread/turn 时，以本地 ownership 映射去重；app-server 事件为权威，Hook 只补充通知。
- 不把 `transcript_path`、JSONL 格式或 ChatGPT/Codex Desktop 私有数据库定义成公共契约。
- 非 managed Hook 安装、变更和信任由用户显式审阅；Hook 失败不得绕过策略或虚假成功。

## 后果

硬件可以先复用 Hooks 获得现有 Codex 会话的有界通知，但真正的硬件到 Codex 双向任务仍依赖 Desktop、Gateway 和 app-server。Hook observer 和双向 Presentation wire format 分别测试，不能用通知 POC 证明控制链路完成。

## 依据

- [OpenAI Codex Hooks](https://learn.chatgpt.com/docs/hooks)
- [`codex-passport` README](https://github.com/JuneLeGency/codex-passport/blob/main/README.md)
- [`codex-passport` validation](https://github.com/JuneLeGency/codex-passport/blob/main/docs/VALIDATION.md)
