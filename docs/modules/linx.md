# `linx`：灵矽 AI 与设备通道

> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24  
> Owner: `OWNERS.md` 中的 `linx`

## 责任

Linx 同时承载两条相互隔离的逻辑通道：Agent 使用 MCP 控制任务；Device Presentation Bridge 将有界任务内容和回执同步到硬件。展示通道不能伪造 MCP 结果，Agent 记忆也不能充当任务事实源。

必须实现：

- 识别创建、查询、续接、停止任务以及阅读任务/回复的意图；
- 项目、任务或目标轮次不唯一时先澄清；
- 只使用 Gateway 返回的 `project_id`、`task_id`、`turn_id` 和 artifact ID；
- 维护 `progressToken -> installation/device/session/connection` 映射；
- 将 MCP progress 合并为语音短摘要，将 presentation snapshot/delta 路由到正确设备；
- 对设备提交的 draft/confirm 保留目标快照和 `command_id`，不能用后续列表变化重定向；
- presentation 消息按 `message_id` 去重、按 revision/sequence 处理乱序并支持重同步。

## 边界

- ASR 文本是不可信输入；模型不能决定身份、绑定、项目权限和风险等级；
- TTS 只播报短摘要，不朗读全文、代码、路径、diff 或堆栈；全文由屏幕分页阅读；
- 图片只转发 Gateway 已授权的设备衍生物，不代理任意 URL；
- 不把 sending、MCP call 返回或 turn_completed 推断为整个 Task succeeded。

## 交付门禁

项目/任务/轮次选择准确率 100%，高风险误放行率 0；覆盖 ASR 同音、否定、歧义、重复确认、断线重连、乱序和目标冻结测试，并以真实灵矽连接验证 Presentation Bridge。
