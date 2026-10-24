# 灵矽语音控制 Codex 最终技术架构

> Status: accepted  
> Version: 1.1  
> Last updated: 2026-10-24  
> Scope: ESP32 → 灵矽 AI → Cloud MCP Gateway → Desktop Client → Codex → 本地项目

## 1. 架构结论

LinxBox 使用一个由产品方运营的公共 Gateway。灵矽 Agent 通过 MCP 控制任务，独立的 Presentation Bridge 向硬件同步任务列表、回复、图片和回执。每个需要访问本地代码的用户安装 Desktop Client；Desktop 只建立出站 WSS，并通过本地 Codex app-server 执行任务。

```text
用户语音
   │
   ▼
 ESP32 ═════设备 WSS════► 灵矽 AI
   ▲                         │
   │ Presentation 内容/回执  ├─ MCP tools/call + notifications/progress
   └─────────────────────────┤
      草稿/确认/读取请求 ─────►│ Presentation Bridge
                             ▼
                  ┌────────────────────┐
                  │ Cloud MCP Gateway  │
                  │ 绑定/任务/路由/审计│
                  │ Broker / Outbox    │
                  └─────────┬──────────┘
                            │ WSS（Desktop 主动出站）
                            ▼
                  ┌────────────────────┐
                  │ LinxBox Desktop    │
                  │ UI + Daemon        │
                  │ Project/Policy     │
                  └─────────┬──────────┘
                            │ stdio JSON-RPC
                            ▼
                  ┌────────────────────┐
                  │ Codex app-server   │
                  │ thread/turn/events │
                  └─────────┬──────────┘
                            ▼
                       本地代码仓库
```

不可退化条件：

- 用户不部署、不配置、不暴露本地 MCP Server；
- 设备路由来自 Gateway 可信绑定，不来自模型参数；
- Desktop 是本地资源的最终授权边界；
- 远端主动编码使用 Desktop Daemon + Codex app-server；
- MCP 控制通道和 Device Presentation 通道相互隔离，不能用展示消息绕过任务授权；
- 本地 MCP/插件只做可选工具、Skill 和 Hooks，不能反向启动当前 Codex Desktop 对话；
- 不使用 UI 自动化、截图或私有数据库控制官方 Codex Desktop；
- 代码、绝对路径、完整 diff、原始日志和 Codex 凭据默认不离开用户电脑。

## 2. 目标与非目标

### 2.1 目标

1. ESP32 用户可以语音创建、查询、继续和停止编码任务。
2. 硬件可以用三键浏览任务、摘要、分页回复、最近历史和受限图片，确认下一轮输入并查看分阶段回执。
3. 灵矽将长任务 progress 和设备安全展示投影下发到正确的设备 WSS。
4. Gateway 把指定设备的任务确定性路由到预绑定 Desktop。
5. Desktop 稳定启动 Codex，获得结构化事件、diff、验证和终态。
6. 重试、断线、重放和崩溃不造成重复执行或虚假成功。
7. MVP 不引入产品账号、配对码或扫码，仍保持最小身份隔离。

### 2.2 非目标

- 不保证 app-server thread 出现在官方 Codex Desktop 侧边栏；
- 不从外部注入或读取用户当前打开的 Codex Desktop 对话；
- 不允许仅凭语音批准 push、发布、部署、密钥访问或系统操作；
- 不提供完整远程 IDE、远程终端或任意文件浏览；
- Gateway 不挂载或浏览用户本地文件系统。

## 3. 设计原则

- 实时语音、MCP 任务控制、Device Presentation、本地代码执行/审阅四条链路分离。
- 身份、绑定、项目路径、风险和审批由确定性代码决定。
- Gateway 与 Desktop 双重授权；云端允许投递不代表本地必须执行。
- 任务和 Delivery 先持久化再确认。
- 云端至少一次投递，Desktop 按 `message_id` 至多执行一次。
- 公共 MCP 与 Codex provider 事件隔离。
- 结果只能声明真实修改和真实执行过的验证。

外部依据：

- [灵矽云端 MCP 接入指南](https://linx.qiniu.com/docs/xrobot/mcp/software-mcp)
- [灵矽第三方业务系统接入指南](https://linx.qiniu.com/docs/xrobot/guide/third-app)
- [OpenAI Codex as a platform](https://developers.openai.com/blog/codex-as-a-platform)
- [OpenAI Codex app-server](https://developers.openai.com/codex/app-server/)
- [OpenAI Codex Hooks](https://learn.chatgpt.com/docs/hooks)
- [`codex-passport` 硬件通知参考实现](https://github.com/JuneLeGency/codex-passport)

## 4. 组件职责

模块内部功能、依赖方向和交付标准见[模块规范](modules.md)；四条链路的完整闭环见[端到端串联](end-to-end.md)；实施目录和 Desktop UI DOM/组件树见[代码与 Desktop UI 结构](code-structure.md)。

| 组件 | 核心责任 | 不负责 |
| --- | --- | --- |
| ESP32 | 三键任务/回复/图片 UI、草稿确认、回执、缓存和消息去重 | Codex 凭据、项目路径、审批、Task 事实 |
| 灵矽 AI | ASR/TTS、Agent、MCP Client、Presentation Bridge、设备路由 | 用户权限、本地执行 |
| Cloud Gateway | 入口认证、绑定、工具、Task Store、展示投影、路由、Outbox、审计 | 本地文件、Codex 凭据 |
| Broker | Desktop presence、WSS、sequence、ACK、重放 | 业务状态机 |
| Desktop UI | token 导入、项目选择、任务、diff、验证、升级 | ASR/TTS |
| Desktop Daemon | WSS、去重、本地策略、Codex 生命周期、事件映射 | 绕过本地授权 |
| Codex app-server | thread、turn、流式事件、命令、文件修改、取消、审批请求 | 设备身份和云端路由 |
| 可选 Codex 插件 | Skill、本地 MCP、Hooks、产品入口 | 远端主动创建 Codex turn |

Gateway MVP 采用模块化单体：MCP Transport、Identity/Binding、Task Service、Presentation、Policy、Broker、Outbox 和 Audit 同进程、按模块隔离。

Device Presentation 的语义由 [`device-presentation/v1`](contracts/device-linx.md) 定义。Issue #4 的离线演示是 Device UI 和状态机 fixture；真实联网、图片传输和 Agent 接入必须另有 integration/real-E2E 证据。

## 5. 身份与设备绑定

### 5.1 Desktop 身份

Gateway 管理员创建 Desktop client：

```text
client_id    = cli_<UUID/ULID>       # 路由标识，不是秘密
client_token = lcc_<256-bit-random>  # 认证秘密，只展示一次
```

Gateway 只保存 `client_token_hash`，Desktop 只导入 `gateway_url + client_token` 并存入系统 Keychain/凭据库。Gateway 根据 token hash 反查 `client_id`；Desktop 不得自行选择或覆盖 client ID。连接方式见 [Gateway/Desktop WSS 契约](contracts/gateway-desktop.md) §1。

### 5.2 MVP 绑定

MVP 由运维预配置 `linx_device_id → client_id`；多 installation 时扩展为 `(installation_id, linx_device_id) → client_id`。`X-Linx-Device-Id` 是路由属性，不是独立认证，Gateway 同时验证灵矽 MCP 入口凭据。

### 5.3 首次使用

安装 Desktop → 导入 client token → 选择本地项目和能力 → 完成 Codex 官方认证 → 检查 app-server → 建立 Gateway WSS。

MVP 不实现产品账号登录、配对码或扫码。换机、解绑和 token 轮换由运维完成。

## 6. 项目授权

Desktop 选择目录后创建不透明 `project_id`，只向 Gateway 同步别名与能力，不上传绝对路径。每次执行前 Desktop 必须重新 `realpath`、检查 grant 和有效期、防止符号链接逃逸，并拒绝 MCP 参数或 prompt 中的任意 cwd/path 覆盖。字段见 [Gateway/Desktop WSS 契约](contracts/gateway-desktop.md) §4。

## 7. 公共 MCP 契约

本节是工具语义摘要；字段、结果、进度、幂等和错误的规范定义见 [MCP 工具契约](contracts/mcp.md)。

| 工具 | 语义 |
| --- | --- |
| `list_projects` | 当前设备绑定 Desktop 的项目别名和在线状态 |
| `list_agents` | 可用执行能力，不暴露本地凭据 |
| `start_task` | 创建并执行任务，通过 progress 实时返回阶段 |
| `get_task_status` | 查询持久化状态，用于主动查询和断线恢复 |
| `continue_task` | 重新授权后续接已有任务 |
| `stop_task` | 请求取消，不等于已经停止 |

`client_id/device_id/session_id/installation_id` 从认证上下文、Header 和绑定注入，工具参数不能覆盖；`request_id` 必填，写工具还要 `idempotency_key`。

`start_task` 先验证身份、绑定、project 所有权和字段；同一幂等作用域的重复 key 返回原 `task_id`；Task 与首个 Delivery 同事务持久化后才发首条 progress；`tools/call` 保持进行中直到终态或 transport 超时，transport 断开不取消 Task。

### 7.1 MCP 长任务进度

灵矽已支持 MCP 长任务。MVP 保持 `tools/call` 进行中，Gateway 发送标准 `notifications/progress`：

- 灵矽维护 `progressToken → device_id/session_id/device_wss_connection`；
- `progress` 是同一 token 内单调递增序号，无法估算百分比时省略 `total`；
- 首条通知包含已持久化的 `task_id`；
- Gateway 只发阶段变化和短摘要，不发送原始日志或内部推理；
- 最终返回标准 `CallToolResult + structuredContent`；
- MCP 断线不取消任务，恢复后通过 `get_task_status` 查询；
- 只有双方协商能力后才切换 MCP Tasks 扩展。

## 8. 端到端时序

创建任务的主路径时序见[端到端串联](end-to-end.md) §2；四条链路的完整闭环与事实源划分也在该文档。

## 9. Gateway ↔ Desktop WSS 协议

本节是传输摘要；envelope、hello、project、dispatch、ACK、重放和事件字段以 [Gateway/Desktop WSS 契约](contracts/gateway-desktop.md) 为准。

内部协议不是 MCP。消息族：

```text
client.hello / heartbeat / capabilities
project.upsert / revoke
task.dispatch / cancel / ack
task.started / progress / approval_required
task.completed / failed / cancelled / interrupted
protocol.upgrade_required
```

交付要求：

- Gateway 为每个 client 维护单调递增 sequence；Task 与 Delivery/Outbox 同事务持久化；
- Desktop 先持久化 `message_id` 再 ACK；ACK 只表示收到，不表示 started 或 succeeded；
- 重连携带 `last_acked_sequence`，Gateway 重放未 ACK 消息；Desktop 同一 `message_id` 至多执行一次；
- 超过 `expires_at` 的任务不得执行；不兼容主版本和未知安全字段 fail closed。

## 10. Desktop 与 Codex

### 10.1 正式执行接口

Desktop Daemon 通过本地 stdio JSON-RPC 控制 Codex app-server：

```text
initialize → initialized
thread/start | thread/resume
turn/start
agent message / command / file / approval events
turn/completed | failed | interrupted
```

要求：

- app-server 不开放公网端口；
- Codex 凭据只存在本机安全存储；
- Desktop 保存 `task_id ↔ thread_id ↔ turn_id ↔ project_id` 所有权映射；
- provider 事件先映射为稳定领域事件；
- 只上传用户可见摘要和执行状态，不上传内部推理；
- `codex exec --json` 仅为开发或兼容 fallback。

### 10.2 Hooks 观察边界

可选 `hook-observer` 可以通过用户审阅并信任的 Codex Hooks 接收 Session、Prompt、Tool、Permission、Stop 和 Interrupt 等事件，用于现有 Codex 会话的有界只读通知。Hook 不是外部 turn 控制 API：不能创建或续接 LinxBox Task，不能批准动作，也不能把 `Stop` 直接映射为成功。`transcript_path` 和 JSONL 仅可作为兼容 fallback，不是稳定契约。

LinxBox 管理任务以 app-server 事件为权威；Hook 和 app-server 重合时按本地 thread/turn ownership 去重。详细决策见 [ADR-0005](adr/0005-hooks-observation-app-server-control.md)。

### 10.3 本地 MCP/插件边界

可选插件可包含 Skill、本地 MCP、Hooks 和产品入口。MCP 的正常方向是 Codex 调用工具；远端任务到达本地 MCP 并不会自动创建 Codex turn。远端主动执行仍由 Companion Daemon 调用 app-server。

系统不依赖 Accessibility、屏幕抓取、当前窗口焦点、Codex 私有数据库或内部 token。

### 10.4 Desktop UI

MVP UI 至少包含：

- token 导入和连接状态；
- 项目选择、别名和授权；
- 任务阶段和最终摘要；
- 本地 diff、验证结果和取消；
- 客户端版本和安全升级提示。

可信审批 UI 未完成前，所有需要审批的动作拒绝执行。

## 11. 任务状态机

状态转换、设备投影、风险等级和 `task-result-v1` 以 [领域状态与结果契约](contracts/domain.md) 为准。

## 12. MVP 数据模型

| 实体 | 关键字段 |
| --- | --- |
| `DesktopClient` | client_id、token_hash、enabled、version、last_seen |
| `DeviceClientBinding` | installation_id?、device_id、client_id、enabled |
| `Project` | project_id、client_id、display_name、capabilities、revoked_at |
| `Task` | task_id、device/session、project、state、policy_snapshot、summary |
| `Delivery` | message_id、sequence、client_id、ack、attempts、expires_at |
| `TaskEvent` | task_id、sequence、type、redacted_payload、occurred_at |
| `PresentationDocument` | task/turn、revision、device-safe pages/image manifests、retention |
| `CommandReceipt` | command/task/turn、receipt_sequence、stage、summary |
| `AgentSession`（仅 Desktop 本地） | provider、opaque_thread_id、project/task ownership |

云端 Project 不保存本地绝对路径；Gateway 不保存 `AgentSession` 或 provider thread/turn 标识。产品化后再加入 User/Tenant、Approval、Artifact 和自助配对实体。

## 13. 权限与风险

```text
effective_capabilities =
  gateway_entry_authenticated
  ∩ binding_enabled
  ∩ client_enabled
  ∩ project_local_grant
  ∩ task_capabilities
  ∩ desktop_policy
  ∩ codex_sandbox
```

| 风险 | 示例 | MVP |
| --- | --- | --- |
| R0 | 搜索、解释、读取测试结果 | 可执行 |
| R1 | 编辑授权项目、运行现有测试 | 按 grant 执行 |
| R2 | 网络、安装依赖、commit、外部写入、批量删除 | 拒绝；未来本地可信审批 |
| R3 | push、发布、部署、密钥、系统设置、改写历史 | 语音入口永久禁止 |

语音“确认”不是审批。未来审批绑定 `task_id + normalized_action + target + args + policy_version + expiry` 的 digest；参数或身份变化后重新审批。

## 14. 安全与隐私

- 公网连接使用 TLS；入口凭据和 client token 可撤销、轮换；
- 外部输入运行时严格校验，安全请求拒绝未知字段；
- 子进程使用 `shell: false`、参数数组、环境 allowlist、超时和进程树清理；
- project root 只由本地 registry 解析；
- 日志不记录 token、完整 prompt、代码、绝对路径或原始 stderr；
- 完整 diff、命令输出、内部推理和未清洗 thread 内容只在本地；
- Gateway 可按明确保留期保存 Desktop 生成的 device-safe 回复分页和受限图片衍生物；不保存源码、任意附件或原始 provider 事件；
- 仓库、语音转写和 Codex 输出均是不可信输入；
- 客户端更新必须签名，支持最低安全版本和撤销。

## 15. 故障恢复

| 故障 | 行为 |
| --- | --- |
| 灵矽 MCP 断开 | 任务继续；重连后查 Task Store |
| ESP32 断开 | 不影响任务；重连不自动重放写请求 |
| Presentation sequence 缺口 | Device 请求 snapshot，不自行拼接或猜测状态 |
| 内容/图片不可用 | 保留任务与回执，可返回上一页，不进入黑屏死路 |
| Desktop 离线 | `waiting_for_client`，仅在 TTL 内等待 |
| WSS 短断 | sequence 重放未 ACK Delivery |
| Gateway 重启 | DB + Outbox 恢复 |
| Desktop/app-server 崩溃 | 不确定运行态标记 `interrupted` |
| 重复 MCP 请求 | idempotency key 返回同一 task |
| 项目撤销 | 新任务拒绝，未开始任务执行前复检 |
| progress 丢失 | 不影响事实状态，查询或最终结果补偿 |

## 16. 部署与代码边界

```text
src/
├── gateway/
├── desktop/
└── admin-cli/
```

契约的权威表述在 `docs/contracts/`；JSON Schema 与 fixtures 是实现产物，落位由对应实现 Spec 决定。插件能力属于产品化阶段，MVP 不预建目录。

MVP Profile：Gateway 模块化单体 + PostgreSQL；Electron Desktop 分离 UI/Main/Daemon + SQLite + Codex app-server；灵矽包含公共 MCP 和独立 Presentation Bridge；ESP32 实现三键任务阅读与确认状态机。完整的目录、分层和 UI 组件树见[代码与 Desktop UI 结构](code-structure.md)，本节不另建一套实现规范。

## 17. 实施路线

1. **契约实现**：将 MCP、Device Presentation、Gateway/Desktop、错误、task-result-v1、状态机实现为 schema、类型和跨实现 fixtures。
2. **Desktop Daemon**：Project Registry、SQLite、app-server adapter、事件、取消和崩溃恢复。
3. **Cloud Gateway**：client/token、binding、Task/Outbox、Desktop WSS、progress 和 Presentation relay。
4. **真实 MVP**：ESP32 三键任务/回复/图片/确认、灵矽双通道、两设备/两 Desktop 隔离验收。
5. **产品化**：可信审批、签名升级、自助配对、多 Desktop、可选插件和 MCP Tasks。

## 18. 验收门禁

- 两个 device ID 只能路由到各自绑定 Desktop；
- 错误 token、禁用 client、禁用 binding 和未知项目均被拒绝；
- Desktop 不开放入站公网端口；
- ESP32 可浏览任务、回复、最近八轮和受限图片，并区分发送、电脑收到、排队、Agent 接受、运行、本轮结束和 Task 终态；
- 确认前不发送，失败重试不改变目标或生成重复 turn，sequence 缺口可恢复 snapshot；
- 重放不产生第二个 Codex turn；
- Gateway、Desktop、MCP 任一断线后能恢复事实状态；
- app-server 能 start/resume/interrupt，并正确映射失败和崩溃；
- 路径逃逸、prompt injection、密钥请求、R2/R3 全部拒绝；
- 详细代码、绝对路径、凭据和内部推理不进入云端日志；
- mock 不能替代真实灵矽、ESP32、WSS、app-server 和目标操作系统验收。
- 合并门禁与功能启用门禁分开，证据分级遵守[质量门禁](process/quality-gates.md)。

## 19. 延后事项

- 官方 Codex Desktop 当前可视 thread 的外部控制；
- 产品账号、自助登录、配对码和扫码；
- 多 Desktop 自动选择和迁移；
- 云端仓库执行器；
- 完整远程 IDE 和终端；
- 自动 push、发布、部署和生产运维。

延后事项进入实施前必须新增 ADR、威胁模型和真实验收。
