# LinxBox 架构设计

## 目标与范围

LinxBox 把 AI Passport 上的设备请求交给云端 Gateway，再由 macOS Desktop 将请求路由到本机的 Codex app-server。设备只负责交互和展示，工作目录、会话续接和 Agent 执行留在用户的 Mac 上。

本文记录当前仓库的实际边界和主要数据流。消息字段、方向、状态和错误码以 [`contracts/`](../contracts/README.md) 为唯一契约；本文不替代契约，也不把单元测试或本地联调描述成生产验收。

## 总体拓扑

```text
AI Passport / 灵矽入口
          │ WSS / MCP
          ▼
      Cloud Gateway ───── SQLite（绑定、目录、查询索引、请求状态）
          │ WSS
          ▼
   macOS Desktop（Electron 主进程 + daemon）
          │ stdio JSON-RPC
          ▼
      Codex app-server ─── 本机 thread / turn / events
```

Desktop 主动连接 Gateway，公网不暴露本地监听端口。Gateway 是设备、灵矽服务和 Desktop 之间的路由与状态边界；它不读取用户本机文件，也不直接调用 Codex。

## 模块职责

| 模块 | 目录 | 负责 | 明确不负责 |
| --- | --- | --- | --- |
| 固件 | `firmware/` | 配网、设备交互、录音、会话目录和回复展示 | Codex 执行、Mac 文件授权 |
| 共享契约 | `contracts/` | 消息 Envelope、字段、方向、响应配对和错误结构 | 网络传输、业务存储和副作用 |
| Gateway | `gateway/` | WSS/MCP 接入、认证、设备绑定、目录、查询关联、请求生命周期和 SQLite | 本机路径解析、Codex 调用 |
| Desktop 主进程 | `desktop/main/` | Electron 生命周期、连接配置、项目/会话注册、Gateway 编排和窗口状态 | Renderer 直接访问 token、文件系统或子进程 |
| Desktop daemon | `desktop/daemon/` | 帧校验、去重、任务状态、Codex adapter、结果投影 | UI 渲染和系统目录选择器 |
| Preload / Renderer | `desktop/preload/`, `desktop/renderer/` | 受限 IPC、主页面和悬浮窗展示 | token、Node API、路径和子进程 |
| Codex adapter | `desktop/daemon/codex-adapter/` | 通过 app-server JSON-RPC 读取会话、续接 thread、启动 turn、映射事件 | 读取 Codex 私有数据库 |

## 关键数据流

### 设备请求与本机执行

1. 固件或灵矽 MCP 入口向 Gateway 提交请求，Gateway 校验认证、绑定和目标会话。
2. Gateway 向已认证的 Desktop 发送 `task.dispatch`。
3. Desktop 根据已同步的 `project_id` / `agent_session_id` 解析本机目标，不信任远端绝对路径。
4. Dispatcher 校验幂等键、过期时间、项目归属和当前任务占用，再向 Codex adapter 请求执行。
5. Adapter 对已存在会话使用 `thread/resume`，对新会话创建 thread，然后调用 `turn/start`。
6. Codex 事件投影为 `task.started`、`task.progress`、`task.completed` 或失败/中断终态，并按契约回传 Gateway。
7. Gateway 保存可查询的会话/消息元数据；设备通过 `messages.list` 和 `message.content.get` 读取历史与正文。

### 会话目录同步

Desktop 通过 app-server 的 `thread/list` 获取本机会话，只发布不含绝对路径的元数据，并用 `agent_sessions.sync` 做全量替换。Gateway 保存目录版本并返回 `agent_sessions.sync.result`。空 `thread_id` 或非法记录必须在 Desktop 侧过滤，不能上传给 Gateway。

`status: idle` 只表示 Desktop 当前没有任务，不保证 thread 可立即写入。Codex Desktop 或其他 app-server 仍可能持有该 thread 的 writer；这种情况必须返回续接失败，不能静默创建新会话。

### 查询与新消息通知

设备发起 `messages.list` 或 `message.content.get` 后，Gateway 将原请求关联到 Desktop 的 query 消息。Desktop 返回 `query.result`，Gateway 再按 `reply_to` 转回设备。任务完成后，Desktop 只有拿到 provider 的真实最终消息 ID 才发送 `message.available`；任务事件 ID 不能冒充消息 ID。

### 绑定

Desktop 发送 `device.bind` 提交规范化 MAC。Gateway 只有在设备在线、双方认证且配对规则允许时保存绑定，并分别返回 Desktop 的 `binding.result` 和设备侧绑定通知。绑定状态、WSS 连接状态和 Codex 会话状态是三个独立状态，UI 不应混为“设备在线”。

## 安全与失败边界

- token 仅由 Desktop 主进程保存和使用；Renderer 只看到脱敏状态。
- Renderer 通过 sandbox、context isolation 和具名 IPC 工作；路径只能来自主进程系统目录选择器。
- Codex 子进程使用参数数组启动，不经 shell；stderr 只保留本地诊断，不上行。
- Gateway 只接收 `project_id`、`agent_session_id` 等标识，不接收或转发本机绝对路径、token、cookie 和原始环境变量。
- 消息必须按共享 Schema 校验；非法消息拒绝并保留可关联的错误码，不能以“已收到”代替“已执行”。
- `accepted` 只表示消息通过初检并进入处理，不代表 Codex 已启动或任务已成功。
- 目标 thread 已被其他 writer 占用时，Desktop 返回明确的续接失败；不 fallback 到新 thread，避免把指令写入错误会话。
- Gateway 的真实部署负责 TLS、凭据和 SQLite 持久化；本地回环联调工具只用于开发验证，不能当作生产 Gateway。

## 当前实现边界

- Gateway 已提供 WSS 设备/Desktop 入口、绑定、会话目录、历史/正文查询、新消息通知以及 MCP 请求准备/查询/取消等实现；完整的灵矽逐轮语音闭环仍需真实环境验收。
- Desktop 已有 Gateway 连接、会话目录同步、任务调度和 Codex app-server 适配；真实线上链路的凭据、网络和 Codex Desktop writer 竞争仍需按部署环境验证。
- 固件已接入 Gateway 客户端和契约校验，但真机烧录、设备网络、灵矽服务和线上 Gateway 的端到端结果不由本地构建自动证明。

## 评审入口

- Desktop：[desktop/README.md](../desktop/README.md)
- Gateway：[gateway/README.md](../gateway/README.md)
- 共享消息：[contracts/README.md](../contracts/README.md)
- 三端集成：[gateway/docs/integration.md](../gateway/docs/integration.md)
