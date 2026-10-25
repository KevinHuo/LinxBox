# LinxBox 架构设计

## 目标与范围

LinxBox 把 AI Passport 上的设备请求交给云端 Gateway，再由 macOS Desktop 将请求路由到本机的 Codex app-server。设备只负责交互和展示，工作目录、会话续接和 Agent 执行留在用户的 Mac 上。

本文记录当前仓库的实际边界和主要数据流。消息字段、方向、状态和错误码以 [`contracts/`](../contracts/README.md) 为唯一契约；本文不替代契约，也不把单元测试或本地联调描述成生产验收。

## 总体拓扑

下图表示三端链路。固件默认保留独立灵矽录放音模式，可选 Gateway 模式已接入设备业务 WSS、真实目录、正文和请求流程；默认开关与配置见[固件接入说明](../firmware/docs/gateway-client.md)。图中的连线不代表整条链路已通过真机验收。

```text
AI Passport ── 语音 WSS ──> 灵矽服务
    │                           │ MCP 文字提案
    │ 设备业务 WSS（可选模式）     ▼
    └──────────────────> Cloud MCP Gateway
                                │       └─ SQLite：设备登记、绑定、目录、请求与执行事实
                                │ Desktop 主动建立 WSS
                                ▼
                        macOS Desktop（Main + daemon）
                                │ stdio JSON-RPC
                                ▼
                          Codex app-server ── 本机 thread / turn / events
```

Desktop 主动连接 Gateway，公网不暴露本地监听端口。Gateway 是设备、灵矽服务和 Desktop 之间的路由与状态边界；它不读取用户本机文件，也不直接调用 Codex。

## 模块职责

| 模块 | 目录 | 负责 | 明确不负责 |
| --- | --- | --- | --- |
| 固件 | `firmware/` | 配网、设备交互、灵矽录放音；可选 Gateway 模式读取目录、正文及处理请求 | Codex 执行、Mac 文件授权 |
| 共享契约 | `contracts/` | 消息 Envelope、字段、方向、响应配对和错误结构 | 网络传输、业务存储和副作用 |
| Gateway | `gateway/` | WSS/MCP 接入、认证、设备绑定、目录、查询关联、请求生命周期和 SQLite | 本机路径解析、Codex 调用 |
| Desktop 主进程 | `desktop/main/` | Electron 生命周期、连接配置、项目/会话注册、Gateway 编排和窗口状态 | Renderer 直接访问 token、文件系统或子进程 |
| Desktop daemon | `desktop/daemon/` | 帧校验、去重、任务状态、Codex adapter、结果投影 | UI 渲染和系统目录选择器 |
| Preload / Renderer | `desktop/preload/`, `desktop/renderer/` | 受限 IPC、主页面和悬浮窗展示 | token、Node API、路径和子进程 |
| Codex adapter | `desktop/daemon/codex-adapter/` | 通过 app-server JSON-RPC 读取会话、续接 thread、启动 turn、映射事件 | 读取 Codex 私有数据库 |

## 模块规格与分工入口

本节索引当前仓库已有的规格与实现说明，不新增协议或替代评审。人员分工以[根目录分工表](../README.md#当前分工)为准，跨模块任务的 DRI 与 reviewer 由对应 Issue 明确。

| 模块 | 当前负责人 | 规格、运行与验收入口 |
| --- | --- | --- |
| 设备固件 | KevinHuo | [固件说明](../firmware/README.md)、[应用与验收边界](../firmware/main/README.md)、[Gateway 接线](../firmware/docs/gateway-client.md)、[硬件接入](../firmware/hardware/folotoy/ai-passport/README.md) |
| Gateway | BoleLiu | [Gateway 说明](../gateway/README.md)、[执行流程与状态](../gateway/docs/execution.md)、[MCP 接入](../gateway/docs/mcp.md) |
| macOS Desktop | yydounai1234 | [Desktop 说明](../desktop/README.md)、[Gateway / Desktop API](../desktop/GATEWAY_API.md) |
| 三端字段契约 | 按 Issue #14 组织三端评审 | [共享 Schema 与 fixtures](../contracts/README.md)、[联调说明](../gateway/docs/integration.md) |

字段、方向和响应结构以根目录 `contracts/` 为唯一正文。各模块 README 记录实现与验证边界；历史草稿编号不能替代可访问的规格正文，也不能作为功能已验收的证据。

## 关键数据流

### 设备请求与本机执行

以下处理已在 Gateway / Desktop 及可选固件 Gateway 模式接线，灵矽逐轮透传与完整真实链路仍需环境验收；完整时序、状态与限制见[确认执行说明](../gateway/docs/execution.md)。

1. 设备先通过 `request.prepare` 固定绑定 Desktop 和目标会话，成功后再开始录音。
2. 灵矽通过 MCP `propose_task_reply` 提交文字，Gateway 校验来源并保存为待确认提案，此时不投递执行任务。
3. 固件 Gateway 模式以停录 OK 记录本轮授权，等原文就绪后发送一次 `request.confirm`，不增加二次确认页面；Gateway 先保存固定投递身份，再向原 Desktop 发送 `task.dispatch`。重复确认不重复投递。
4. Desktop 根据本地保存的项目与会话映射解析目标，校验去重、过期和固定目标；不会把远端绝对路径作为工作目录。已发布会话沿用本机目录，WSS 任务不重复触发项目授权弹窗。
5. 已有会话由 adapter 执行 `thread/resume`、`turn/start`；固定会话续接失败时明确报错，不静默改成新会话。
6. ACK 只表示接收，`task.started` 才表示运行；一致成功 TaskResult 才推进 completed，失败或中断按真实事实回传。
7. Gateway 持久保存请求与执行事实；断线或重启后的不确定结果保持 unknown，不自动重投。历史与正文经 Desktop 查询转发，不把查询入队当成内容已送达。

### 会话目录同步

Desktop 根据本地项目配置、Codex 项目或默认目录组织可发布会话，通过 app-server 读取会话元数据，并用 `agent_sessions.sync` 全量替换目录，不上传本机绝对路径。Gateway 保存目录版本并返回 `agent_sessions.sync.result`。空 `thread_id` 或非法记录必须在 Desktop 侧过滤，不能上传给 Gateway。

`status: idle` 只表示 Desktop 当前没有任务，不保证 thread 可立即写入。Codex Desktop 或其他 app-server 仍可能持有该 thread 的 writer；这种情况必须返回续接失败，不能静默创建新会话。

### 查询与新消息通知

设备发起 `messages.list` 或 `message.content.get` 后，Gateway 将原请求关联到 Desktop 的 query 消息。Desktop 返回 `query.result`，Gateway 再按 `reply_to` 转回设备。任务完成后，Desktop 只有拿到 provider 的真实最终消息 ID 才发送 `message.available`；任务事件 ID 不能冒充消息 ID。

### 绑定

设备先使用账号凭据连接，在 10 秒内通过 `device.hello` 上报真实 MAC，Gateway 动态登记设备、账号及凭据指纹。Desktop 再通过 `device.bind` 提交该 MAC；首次绑定要求设备在线，登记账号与 Desktop 账号一致，且设备未被其他 Desktop 绑定。重复同一绑定幂等，不同账号不能覆盖既有归属。

无需预填设备 MAC 或 `pairing.rules` 白名单；旧配置仍可解析，但不再作为配对白名单。绑定状态、WSS 连接状态和 Codex 会话状态是三个独立状态，UI 不应混为“设备在线”。

MCP 使用独立凭据，并按主线配置限制设备和智能体；设备 WS 的自动登记不会自动扩大 MCP 的授权范围。配置见[MCP 接入说明](../gateway/docs/mcp.md)。

## 安全与失败边界

- token 仅由 Desktop 主进程保存和使用；Renderer 只看到脱敏状态。
- Renderer 通过 sandbox、context isolation 和具名 IPC 工作；路径只能来自主进程系统目录选择器。
- Codex 子进程使用参数数组启动，不经 shell；stderr 只保留本地诊断，不上行。
- Gateway 使用项目、会话等标识路由，本机路径由 Desktop 解析；各端凭据仅用于入口认证，不透传给其他端，不把 cookie 或原始环境变量作为业务消息同步。
- 消息必须按共享 Schema 校验；非法消息拒绝并保留可关联的错误码，不能以“已收到”代替“已执行”。
- `accepted` 只表示消息通过初检并进入处理，不代表 Codex 已启动或任务已成功。
- 目标 thread 已被其他 writer 占用时，Desktop 返回明确的续接失败；不 fallback 到新 thread，避免把指令写入错误会话。
- Gateway 的真实部署负责 TLS、凭据和 SQLite 持久化；本地回环联调工具只用于开发验证，不能当作生产 Gateway。

## 当前实现边界

- Gateway 已提供 WSS 设备/Desktop 入口、动态登记与绑定、目录、历史/正文查询、新消息通知、MCP 提案、确认投递和执行恢复；完整的灵矽逐轮语音闭环仍需真实环境验收。
- Desktop 已有 Gateway 连接、会话目录同步、任务调度和 Codex app-server 适配；真实线上链路的凭据、网络和 Codex Desktop writer 竞争仍需按部署环境验证。
- 固件默认独立模式保留直连灵矽与演示目录；可选 Gateway 模式已有真实绑定、目录、历史正文和准备／确认／恢复接线。灵矽设备接入服务与本项目 Cloud MCP Gateway 分工不同；可选模式已实现也不代表真实平台逐轮透传、实际 Desktop 执行及双 TLS／音频内存峰值已验收。

## 评审入口

- Desktop：[desktop/README.md](../desktop/README.md)
- Gateway：[gateway/README.md](../gateway/README.md)
- 共享消息：[contracts/README.md](../contracts/README.md)
- 三端集成：[gateway/docs/integration.md](../gateway/docs/integration.md)
