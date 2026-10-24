# 模块索引与依赖

> Status: accepted  
> Version: 1.1  
> Last updated: 2026-10-24

本目录是模块职责的唯一索引。每个模块只在自己的文档中定义职责、输入输出、状态所有权、依赖、禁止项和交付门禁；跨模块完整链路见[端到端串联](../end-to-end.md)。

| 模块 | 规范 | 一句话职责 |
| --- | --- | --- |
| `device` | [AI Passport / ESP32](./device.md) | 三键任务阅读、图片浏览、输入确认、回执和离线恢复 |
| `linx` | [灵矽 AI](./linx.md) | 语音理解、MCP 控制和设备展示通道编排 |
| `gateway` | [Cloud Gateway](./gateway.md) | 身份绑定、任务事实、可靠投递和脱敏设备投影 |
| `desktop` | [Desktop UI / Daemon](./desktop.md) | 本地授权、执行事实、结果整理和用户审阅 |
| `codex` | [Codex adapter](./codex.md) | app-server 生命周期到领域事件的唯一适配层 |
| `contracts` | [契约与领域](./contracts.md) | schema、状态机、版本和跨实现 fixtures |
| `quality` | [质量与集成](./quality.md) | Spec 流程、CI、证据矩阵和真实链路门禁 |

```text
device <== device presentation ==> linx <== MCP ==> gateway
                                                   |
                                                   | Gateway/Desktop WSS
                                                   v
                                                desktop
                                                   |
                                                   | local stdio
                                                   v
                                                 codex

contracts ──约束──> 所有边界
quality   ──验证──> 所有模块与完整链路
```

依赖规则：

- `contracts` 不依赖 transport、数据库、Electron、LVGL 或 app-server SDK；
- Device 展示通道与 MCP 控制通道分离，共享 Task/Turn/Receipt 标识和状态语义；
- Gateway 拥有云端任务和 Delivery 事实，Desktop 拥有本地授权和执行事实，Device 只拥有 UI 游标、草稿及有界缓存；
- 模块不能复制另一模块的状态机或 DTO；跨边界变更先修改契约与 Spec；
- 一个 Task/Spec 只指定一个主模块，跨边界必须列 producer、consumer 和 Integration Owner。
