# MCP 最终识别文字接入

Refs #33、#31、#14、#1。DRI：BoleLiu（Gateway）。主模块：Gateway；受影响模块：固件、Desktop 与灵矽平台接入；必要跨端评审：KevinHuo（固件）、yydounai1234（Desktop）。Streamable HTTP 传输设计见 #31，受控联调的请求自动关联设计见 #33；不能视为已经通过跨端评审或真实联调。

目标是接收每轮最终 ASR 原文并保存为待确认请求。范围包括 MCP 接入、录音目标准备、持久化、取消、超时和来源校验；不包括任务确认投递、Agent 执行或灵矽平台代码修改。

## Gateway 配置

在部署配置中增加 `credentials.mcp`，每项包含独立 `token`、目标设备的规范 MAC `device_id`（设备通过 hello 登记） 和灵矽 `agent_id`。同一 token 不能用于设备或 Desktop；凭据不写入请求数据库。省略或空数组保留只读服务，全部 MCP 调用返回 401。

灵矽侧服务地址配置为公网 HTTPS `https://<网关域名>/mcp`，通过 `Authorization: Bearer <独立 MCP token>` 认证。自定义 MCP 页面选择 `Streamable HTTP`、`Normal`，Query 留空，Header 设置 Authorization。联调实例可沿用 `http://124.220.162.10:8787/mcp` 和专用测试凭据；此地址不使用 ws/wss。

| 传输方式 | 入口与会话 |
| --- | --- |
| Streamable HTTP | `POST /mcp` 完成 initialize、通知、tools/list 和 tools/call；初始化响应返回 Mcp-Session-Id，后续每次请求携带该头。GET /mcp 携带此头读取独立流，DELETE /mcp 携带此头释放会话 |
| 旧版 HTTP/SSE | 无 Mcp-Session-Id 的 GET /mcp 建立 SSE；使用 endpoint 事件返回的 POST /mcp/message?sessionid=... 地址发送消息 |

Streamable HTTP 的 POST 设置 `Content-Type: application/json`、`Accept: application/json, text/event-stream`，后续请求携带协商出的 `MCP-Protocol-Version`。SDK 返回 JSON 响应；HEAD /mcp 返回 200，OPTIONS /mcp 返回 204 和 Allow，仅表示通过认证的入口探测，不代表已初始化或收到识别文字。两类探测都不会创建会话，也不开放浏览器跨源访问。

两种传输共用以下约束：

- 每个 HTTP 请求都需要独立 MCP 凭据；传输会话绑定创建它的凭据。Mcp-Session-Id、旧 SSE sessionid 都不是业务 request_id 或灵矽语音会话，不能混用。
- 每条消息不超过配置帧限制和契约的 64 KiB；旧 SSE 接收队列有界，两种传输共用 MCP `max_online_connections` 上限。旧 SSE 最长保留 30 分钟；Streamable 会话空闲 30 分钟释放，客户端到期后重新 initialize。关闭 Gateway 时释放所有会话。单次工具业务操作最长 15 秒，Streamable POST 处理最长 20 秒。
- 反向代理保持 SSE 流式转发，禁用响应缓冲。不得把 token 放在地址中；代理访问日志不得记录凭据或语音原文。

唯一工具 `propose_task_reply` 的参数为 `{"text":"最终 ASR 原文"}`。inputSchema 从共享契约导出，不依赖客户端联网解析 `$ref`；不接受模型指定设备、目标或请求标识。保留原有空白、标点和文字，拒绝纯空白、超过 16000 码点或超过 16 KiB UTF-8 的文字，不截断。

initialize 与 tools/list 只需认证，不要求录音业务上下文。默认严格模式下，真正调用工具时必须逐轮传递以下四头；受控演示可按下节开启缺失请求 ID 的自动关联。不要在自定义 MCP 页面把 X-Linx-Request-Id 写成固定值；配置服务描述或提示词也不能保证所有最终 ASR 自动调用工具。

## 无请求 ID 的受控联调模式

用户已选择优先演示，可在 gateway.json **顶层**增加以下配置（与 credentials 同级，区别于 credentials.mcp 凭据数组）：

```json
"mcp": {
  "allow_implicit_request_id": true
}
```

默认值为 false，设回 false 并重启即可恢复严格模式。开启后，只有 X-Linx-Request-Id **完全缺失**时才自动关联；显式空值、重复值、错误/过期 ID 不会回退。设备、智能体、语音会话三个来源头和独立 Bearer token 仍需正确，所有原有归属、目录、目标和文字限制继续生效。

自动关联在请求服务锁与数据库事务内进行：先处理过期，再查找该设备唯一未过期的 awaiting_text/awaiting_confirmation 请求。没有候选返回 NOT_FOUND，要求固件先 request.prepare；出现多个候选返回 CONFLICT。awaiting_confirmation 仅用于按既有原文和来源校验返回幂等快照，不覆盖文字或更换来源。成功仍使用固件原有 request_id，进入 awaiting_confirmation 并通知设备，不自动创建请求或增加任务调度。

日志以 mcp.demo.enabled 标记开关，以 mcp.request.resolved、request_id_source=unique_pending 和真实 request_id 标记成功关联；显式请求使用 request_id_source=header。缺失头仍保留在 missing_headers 中，这是诊断事实，在开启模式后不再单独构成拒绝原因。

该模式适用于受控串行演示：缺少可靠逐轮标识时，取消或超时后的旧文字可能关联到下一轮新请求，重复投递也没有跨轮隔离保证。不要将演示通过描述为正式语音可靠性验收；正式关联仍应使用可透传的逐轮标识。

## 每轮顺序

1. 固件通过已认证 Gateway WSS 调用 `request.prepare`，携带新的 `request_id` 和目标 `agent_session_id`。Gateway 验证绑定与目录，保存 Desktop、Agent 会话、thread/project/title；同设备录音串行，同会话最多一个未结束请求。
2. prepare 成功后才录音。固件向灵矽发送带本轮 `request_id` 的 `listen.start`；这项透传需要平台支持，当前固件尚未接入。
3. 平台只对最终非空 ASR 调用 `propose_task_reply`。默认每个工具 POST 携带 `X-Linx-Device-Id`、`X-Linx-Agent-Id`、`X-Linx-Session-Id`、`X-Linx-Request-Id`。前三项按平台文档传递，第四项仍待平台支持。Header 名大小写不敏感；严格模式下缺失、重复或不匹配均拒绝。演示开关仅允许请求 ID 完全缺失时关联唯一待处理请求，不查询任意“最新 request”。
4. Gateway 校验凭据限定的设备和智能体、原请求归属与存活状态，再保存原文，进入 `awaiting_confirmation`，推送 `request.proposed`。灵矽语音会话与智能体随首次成功提案固定；同请求后续不得更换来源。
5. 重复 MCP 的相同原文与来源返回已有快照，不重复推送；文字或来源变化返回 CONFLICT。取消/超时后的调用也拒绝，不能污染下一轮。
6. 固件可通过 `request.get` 恢复数据库快照，`request.cancel` 取消确认前请求。Gateway 当前不支持任务调度，`request.confirm` 返回 DEPENDENCY_UNAVAILABLE，请求保持待确认；不返回假成功。

重复 prepare 返回原请求，不更换目标或重置时间。等待文字为 120 秒，待确认为 300 秒；截止时间落盘，启动、每次访问及运行时每秒检查，到期保存 `failed` 并推送 `request.updated`。没有平台明确错误时只记录“未收到文字”，不推断识别失败原因。数据库保留终态与 request_id，不复用旧 ID。目录更新不删除在途请求；接收新文字前再次核实固定 thread/project 仍在目录。

成功结果为标准 MCP `content:[{type:"text",text:"业务 JSON"}],isError:false`；失败为 `isError:true` 与共享业务错误。JSON-RPC id 由官方 SDK 管理，不能用作 request_id。工具保存草稿不会发送任何 Desktop 任务。

## 不修改灵矽代码的边界

[灵矽云端 MCP 文档](https://linx.qiniu.com/docs/xrobot/mcp/software-mcp)规定 SSE 入口及三个来源头，但没有给出最终 ASR 固定路由或逐轮 `X-Linx-Request-Id` 的配置承诺。[设备 WebSocket 文档](https://linx.qiniu.com/docs/xrobot/platform/websocket)也不能作为该透传已经支持的证据。

如果平台提供可配置的最终 ASR 工作流或强制工具节点，并能按轮透传 request_id，可在不修改平台代码的情况下可靠关联。当前另提供显式开启的唯一待处理请求联调关联，接受上述跨轮限制。只有注册 MCP、配置提示词让模型调用工具，无法保证普通闲聊、拒答等每一条有效语音都触发，也无法保证 ASR 原文不被模型改写。

在平台能力和固件接线完成前，只能验收 Gateway 模拟链路，不能声明“所有灵矽语音必调 MCP”或完整语音闭环已完成。

## 验证入口

### 联调日志

DRI：BoleLiu（Gateway）。本次日志改动仅涉及 Gateway 传输层，不改变 MCP 参数、认证、请求状态或固件通知协议。验收包括正常提案、同轮重试、上下文缺失、来源不匹配、参数错误、请求不存在和设备离线的主机模拟链路；真实灵矽接入仍需单独验收。

`log.level=info` 即可查看以下事件，默认 text 格式输出中文摘要；`log.format=json` 可用于按字段筛选。

| 事件 | 阅读方式 |
| --- | --- |
| `mcp.connection.opened` / `mcp.connection.closed` | SSE 建连与结束，包含连接编号、持续时间和结束原因；建连成功不表示已收到识别结果 |
| `mcp.http.rejected` | HTTP 入口拒绝，包含状态码、错误原因和排查提示；检查凭据、Origin、入口、SSE 是否过期或消息大小 |
| `mcp.http.completed` | Streamable HTTP 处理结束，包含具体 HTTP 方法、受限 RPC 方法、传输类型和状态码；HTTP 200/202 不能代替工具业务结果 |
| `mcp.call.received` | 已收到 `propose_task_reply`，记录本轮上下文、脱敏参数 body 和文字长度 |
| `mcp.call.validated` | 来源和参数 Schema 校验通过，开始核对原请求并保存文字；不表示持久化已经成功 |
| `mcp.call.completed` | 工具最终业务结果；成功包含请求状态、目标会话和脱敏 response_body；失败包含 failed_stage、error_code、error_message、error_hint 和耗时 |
| `request.notification.enqueued` | 请求状态通知已进入固件发送队列，尚不能证明设备收到或用户已确认 |
| `request.notification.deferred` | 固件离线，快照已保存，重连后使用 request.get 获取 |
| `push.failed` | 通知未入队或编码/校验失败，包含业务请求编号和原因；不能据此认定文字没有保存 |

同一次工具调用用 `trace_id` 串联接收、校验和结果。`request_id` 关联固件 prepare、MCP 提案和通知；`agent_session_id` 是固定的 Desktop 目标会话，`voice_session_id` 是平台语音会话。`mcp_connection_id` 仅用于日志关联 SSE 连接，`rpc_id` 是 JSON-RPC 调用编号；两者都不能替代业务 request_id。原始 SSE sessionid、认证头和 token 不写入这些日志。

失败阶段包括 `authentication`（认证）、`context`（来源头）、`arguments`（工具参数）、`request_state`（原请求/来源/目标状态）、`persist`（保存过程）及 `response`（响应契约）。`missing_headers` / `duplicate_headers` 列出缺失或重复的头名；来源不匹配时可比较 device_id / linx_agent_id 与 credential_device_id / credential_linx_agent_id。HTTP 鉴权失败中的 claimed_* 字段仅为未经验证的客户端声明。

识别内容来自 MCP 工具参数，日志标记 `text_source=mcp_tool_argument`；Gateway 无法据此证明平台传入的是未经模型改写的 ASR 原文。body.text、标题、路径等保持脱敏，额外记录 `text_is_string`、`text_non_blank`、UTF-8 字节数 `text_bytes` 和码点数 `text_codepoints`，可区分缺失、空白、长度超限；正文仍按既有协议保存和返回。脱敏 body 沿用 4096 字节预算和截断标记。

例如缺少本轮 ID 时，会看到“收到文字提案工具调用”，随后出现 WARN“缺少本轮请求或灵矽上下文”，`failed_stage=context` 且 missing_headers 包含 X-Linx-Request-Id；此时先检查逐轮头透传。HTTP POST 返回 202 仅表示 JSON-RPC 消息入队，应继续查看 mcp.call.completed 的 ok 和请求快照；机械事件 mcp.rpc.accepted 仅在 debug 输出。

```sh
# 服务器默认 text 日志：查看最近 MCP 与请求通知过程。
sudo journalctl -u linxbox-gateway --since '10 minutes ago' --no-pager | \
  rg 'mcp\.|request.notification\.|push.failed'

# 知道业务 request_id 后，关联固件 prepare、MCP 和通知。
sudo journalctl -u linxbox-gateway --since '10 minutes ago' --no-pager | \
  rg -F '本轮的request_id'
```

服务器未安装 ripgrep 时，可把上述 `rg` 换成 `grep -E`、`rg -F` 换成 `grep -F`。

### 自动化验证

```sh
cd gateway
go test -count=1 ./internal/service -run '^TestRequest'
go test -count=1 ./internal/transport -run '^TestMCP'
make check
make build
```

测试使用临时 SQLite、模拟语音文字和真实本机 HTTP/Streamable HTTP/SSE/WSS 协议栈，不连接灵矽或真实 Agent，不产生编码任务。涵盖工具发现、唯一参数、原文保留、普通闲聊接收、两种传输的逐次上下文、认证/来源隔离、重复调用、取消后迟到、超时及恢复；另验证 DELETE/HEAD/OPTIONS、传输会话凭据隔离、连接上限、关闭释放、消息大小、Content-Type/Accept/版本头及 SDK 日志脱敏。

真实验收必须分别提供：固件 prepare 后录音、灵矽最终 ASR 固定调用与逐轮 ID 透传、错误时不转入通用 LLM、设备显示原文及确认页的证据。现有测试不能替代这些结果。

## 本次本地验证结果

2026-10-26（受控联调关联）：在独立管理工作树执行 `GOCACHE=/tmp/linxbox-gateway-integration-go-cache GOFLAGS=-buildvcs=false make check build release`，并基于同一 coverage.out 运行 `tools/covergate -base origin/main`，格式、静态检查、全部单元/集成测试、整体/关键包/增量包覆盖率与全部包 race 检测均通过。整体语句覆盖率为 **93.44%（2564/2744）**；请求服务所在包 92.41%、传输包 94.18%、存储包 91.57%，所有关键包和增量包均通过原有 90% 门禁，没有新增统计排除或下调门槛。

联调关联回归覆盖开关默认关闭/显式开启、无 prepare、过期/取消、错误设备/来源、目录与归属变化、损坏数据库多候选拒绝、同轮幂等与重启恢复；两种传输均验证缺头关联，以及显式错误、空值、重复和过期 ID 不回退。该结果不证明缺头模式具有跨轮迟到隔离能力。

官方 Streamable 客户端验证初始化、独立 GET 流、工具发现、原文保存、同轮幂等、多轮逐 POST 上下文与 DELETE。HTTP 回归另覆盖跨 token 会话拒绝、关闭后不可复用、连接总量、停止时释放、HEAD/OPTIONS、Content-Type/Accept/版本头、错误 JSON 与超限。SDK 日志适配器隐藏原始会话 ID 和不可信消息，机械 SDK 事件留在 debug；即使 debug 日志也断言私人正文、路径、凭据、传输 session ID 和伪造诊断头不落日志。旧版 SSE 及既有固件/Desktop 回归通过。

`make build release` 通过：本机可执行文件及 Linux amd64/arm64 无 CGO 发布构建均已生成；`git diff --check` 通过。Linux 结果为交叉编译，尚未在目标主机运行；容器镜像尚未构建验证。

以上为主机模拟协议链路结果；没有连接灵矽、烧录固件、向真实 Agent 投递任务或部署公网服务。灵矽固定路由与逐轮透传、固件接线、确认执行及真实语音验收仍未完成。
