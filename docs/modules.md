# 模块规范

> Status: accepted  
> Version: 2.0  
> Last updated: 2026-10-24

本文件定义七个模块的职责、依赖方向、状态所有权与交付门禁。跨模块完整链路见[端到端串联](./end-to-end.md)，字段级语义见[契约索引](./contracts/README.md)，目录与分层见[代码与 Desktop UI 结构](./code-structure.md)。

| 模块 | 一句话职责 |
| --- | --- |
| `device` | 三键任务阅读、图片浏览、输入确认、回执和离线恢复 |
| `linx` | 语音理解、MCP 控制和设备展示通道编排 |
| `gateway` | 身份绑定、任务事实、可靠投递和脱敏设备投影 |
| `desktop` | 本地授权、执行事实、结果整理和用户审阅 |
| `codex` | app-server 生命周期到领域事件的唯一适配层 |
| `contracts` | schema、状态机、版本和跨实现 fixtures |
| `quality` | Spec 流程、CI、证据矩阵和真实链路门禁 |

各模块的主负责人、必要 Reviewer 与跨模块 Integration Owner 在对应开发 Issue 的"模块、Spec 与负责人"一节指定，不另设负责人矩阵。

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

## device 模块

目标硬件为 ESP32-C3、8 MB Flash、无 PSRAM、240×320 RGB565 竖屏和 UP/DOWN/OK 三个按键。Device 提供任务阅读和明确确认入口，不判断云端或 Agent 的真实状态。

必须实现：

- 展示有界任务列表、选中态、未读和连接状态；
- 展示任务摘要、分页后的用户可见回复和最近八轮有界历史；
- 展示经过设备适配的图片总览与预设局部，保留 task/turn/image 归属；
- UP/DOWN 选择或翻页，短按 OK 执行，长按 OK 返回；禁止长按释放触发短按、首尾循环和页面穿透；
- 输入开始时冻结 `task_id`、`turn_id` 和可选 `image_id`，显示转写与目标，只有明确确认才发送；
- 区分 sending、desktop_received、queued、agent_accepted、running、turn_completed 和 failed 回执；
- 对 `message_id` 去重；断线、失败和重试保留同一 draft/command 身份，不自动重发；
- 每任务最多一条待执行指令；恢复初始场景清理缓存、草稿、队列、未读和旧计时事件。

状态所有权：Device 只拥有当前页面、选中项、页码、图片位置、未确认草稿和有界展示缓存。Task、Turn 和 Delivery 状态只显示服务端投影，不能由页面跳转或动画反向修改。

输入与输出：按 [`device-presentation/v1`](./contracts/device-linx.md)；不输出本地路径、Codex/provider ID、权限决定、完整 diff、原始日志或凭据。

门禁：主机测试覆盖导航、分页、上下文冻结、按键抑制、去重、乱序、旧事件和恢复；固件资源、字体、Flash/RAM 水位和镜像 SHA-256 有证据；真机检查圆角裁切、中文可读性、100 次导航、25 次回复及 30 分钟稳定性；离线演示证据与真实联网证据分开报告，模拟数据必须显著标识。

## linx 模块

Linx 同时承载两条相互隔离的逻辑通道：Agent 使用 MCP 控制任务；Device Presentation Bridge 将有界任务内容和回执同步到硬件。展示通道不能伪造 MCP 结果，Agent 记忆也不能充当任务事实源。

必须实现：

- 识别创建、查询、续接、停止任务以及阅读任务/回复的意图；
- 项目、任务或目标轮次不唯一时先澄清；
- 只使用 Gateway 返回的 `project_id`、`task_id`、`turn_id` 和 artifact ID；
- 维护 `progressToken -> installation/device/session/connection` 映射；
- 将 MCP progress 合并为语音短摘要，将 presentation snapshot/delta 路由到正确设备；
- 对设备提交的 draft/confirm 保留目标快照和 `command_id`，不能用后续列表变化重定向；
- presentation 消息按 `message_id` 去重、按 revision/sequence 处理乱序并支持重同步。

边界：ASR 文本是不可信输入，模型不能决定身份、绑定、项目权限和风险等级；TTS 只播报短摘要，不朗读全文、代码、路径、diff 或堆栈；图片只转发 Gateway 已授权的设备衍生物，不代理任意 URL；不把 sending、MCP call 返回或 turn_completed 推断为整个 Task succeeded。

门禁：项目/任务/轮次选择准确率 100%，高风险误放行率 0；覆盖 ASR 同音、否定、歧义、重复确认、断线重连、乱序和目标冻结测试，并以真实灵矽连接验证 Presentation Bridge。

## gateway 模块

| 子模块 | 责任与权威数据 |
| --- | --- |
| `mcp-transport` | MCP 认证、DTO、progress 和工具结果；不拥有业务状态 |
| `identity` | token hash、device-client binding、禁用和轮换 |
| `projects` | 绑定范围内的项目别名和 availability 投影 |
| `tasks` | Task、Turn lineage、TaskEvent 和幂等写入 |
| `presentation` | 任务卡片、回复页、图片描述和回执的脱敏设备投影 |
| `broker` | Desktop WSS presence、sequence、ACK 和重放 |
| `outbox` | Task/Presentation Delivery 的事务投递事实 |
| `policy` | 身份、绑定、能力、内容分级和风险预检 |
| `audit` | 不含代码、路径、正文和凭据的脱敏审计 |

规则：

- 写操作以已验证身份和 idempotency key 去重；Task 与首个 Delivery 同事务提交；
- Device 不能提交 client ID、本地路径或 provider ID；续接只携带父 Task ID；
- Presentation 只包含 Desktop 明确生成的 device-safe 内容：标题、短摘要、分页正文、受限图片衍生物和回执；
- 禁传字段与内容分级见[契约索引](./contracts/README.md) 的单一事实源；
- 内容保存期限、最多八轮设备历史、页数/字节上限和删除必须由 schema/配置固定，不能无限增长；
- ACK、receipt、turn terminal 和 task terminal 是不同事实。

门禁：依赖 contracts、PostgreSQL 和 WSS transport，不依赖本地文件系统或 Codex 凭据。身份隔离、幂等、Outbox、ACK/重放、乱序、限流、内容上限、删除和脱敏必须有单元、契约及真实存储集成测试。

## desktop 模块

| 子模块 | 目录 | 责任 |
| --- | --- | --- |
| `ui` | `apps/desktop/src/main`、`apps/desktop/src/renderer` | token、连接、项目授权、任务、完整 diff、验证、风险和取消 |
| `gateway-client` | `apps/desktop/src/daemon/gateway-client` | 出站 WSS、hello、heartbeat、sequence、ACK 和恢复 |
| `project-registry` | `apps/desktop/src/daemon/project-registry` | 本地路径、不透明 project ID、grant 和撤销 |
| `policy` | `apps/desktop/src/daemon/policy` | realpath、能力交集、风险和本地最终拒绝 |
| `tasks` | `apps/desktop/src/daemon/tasks` | 去重、执行所有权、状态、取消和崩溃恢复 |
| `result-projector` | `apps/desktop/src/daemon/result-projector` | 将本地结果投影为 `local-detail`、`cloud-summary`、`device-content` 三级 |
| `codex-adapter` | `apps/desktop/src/daemon/codex-adapter` | Codex app-server 生命周期与事件到领域事件的映射，见 [`codex` 模块](#codex-模块) |
| `local-store` | `apps/desktop/src/daemon/local-store` | project、delivery、task/provider ownership、完整本地结果 |

结果分级：

1. `local-detail`：完整 diff、命令输出和本地路径，只供 Desktop 用户审阅；
2. `cloud-summary`：相对路径摘要、验证结果和脱敏错误；
3. `device-content`：无源码/路径/凭据的标题、摘要、分页用户可见回复和设备图片衍生物。

每级都由确定性 projector 产生；不能把 app-server 原始事件直接上传。用户要求"完整回复"表示完整的 device-safe 用户可见回复，不表示完整内部日志、推理或代码。

不变量与门禁：

- 保存 `task_id <-> thread_id <-> turn_id <-> project_id`，provider ID 只在本地；
- 接到 device command 后重新检查 binding、grant、目标 task/turn/image 和风险；
- 同一 `message_id/command_id` 至多产生一次 turn；
- 路径逃逸、跨项目续接、无可信审批的 R2/R3、app-server 崩溃和恢复不确定性全部 fail closed；
- UI、Daemon、SQLite 和真实 app-server 分别提供单元、集成、崩溃恢复和目标 macOS 验收证据。

## codex 模块

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

门禁：固定 app-server 版本/兼容范围；fake-process contract tests 覆盖握手、start/resume/interrupt、超时、异常退出、未知事件和终态；升级先通过旧 fixture、新 fixture 和目标 macOS 真实验收。

## contracts 模块

维护 MCP、Device/Linx、Gateway/Desktop、Task/Turn/Receipt 状态、结果和错误的版本化 schema、语义文档及正反例 fixtures。

规则：

- schema 是实现阶段的单一事实源，类型从 schema 生成或双向一致性检查；
- ID 使用不同值对象，禁止裸字符串互换；
- 删除、改名、类型或语义改变升主版本，并写迁移/回滚；
- 安全请求默认拒绝未知字段和超限 payload；
- producer、consumer 使用同一组 sanitized fixtures；
- Task state、Turn receipt、Delivery ACK 和 Device UI state 分开定义；
- 跨边界变更需要 Spec、双方 Reviewer 和 Integration Owner。

模块不拥有 transport、数据库、UI 或 provider 实现；契约索引见 [`docs/contracts/README.md`](./contracts/README.md)。

## quality 模块

维护 Spec 流程、契约 fixtures、CI、端到端环境、安全回归、故障/负载/稳定性验证和证据索引。Quality 不拥有业务规则，也不替代模块自己的单元测试。

必须区分：

- 文档/Schema 检查；
- 单元和增量覆盖率；
- producer/consumer 契约测试；
- 真实数据库、WSS、Electron、app-server 集成；
- 模拟 Device/Linx E2E；
- 真实 AI Passport、灵矽、macOS 和 app-server E2E；
- 真机资源、按键、屏幕和 soak；
- 合并门禁与功能启用/发布门禁。

不得把 mock、截图、构建成功、生成固件、作者自审或 Spec 状态字段修改表述为真实验收通过。详细门禁见 [`docs/quality-gates.md`](./quality-gates.md)。
