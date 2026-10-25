# MCP 最终识别文字接入

Refs #14、#1。DRI：BoleLiu（Gateway）。主模块：Gateway；受影响模块：固件、Desktop 与灵矽平台接入；必要跨端评审：KevinHuo（固件）、yydounai1234（Desktop）。协议沿用已合并的 `contracts/`，不新增文字投递 API。

MCP 工具负责接收每轮最终 ASR 原文并保存为待确认请求，包含来源校验、录音目标准备、持久化、取消和超时；工具调用本身不执行任务。确认投递及执行事实由独立的[确认执行流程](execution.md)处理，灵矽平台代码不在本仓库修改。

## Gateway 配置

在部署配置中增加 `credentials.mcp`，每项包含独立 `token`、目标设备的规范 MAC `device_id`（设备通过 hello 登记） 和灵矽 `agent_id`。同一 token 不能用于设备或 Desktop；凭据不写入请求数据库。省略或空数组保留只读服务，全部 MCP 调用返回 401。

灵矽侧服务地址配置为公网 HTTPS `https://<网关域名>/mcp`，通过 `Authorization: Bearer <独立 MCP token>` 认证。该入口使用灵矽文档的 HTTP/SSE 传输：

- `GET /mcp` 建立 SSE，SDK 返回 `endpoint` 事件，指向 `POST /mcp/message?sessionid=...`。
- 每个 POST 都需要相同凭据。SSE sessionid 只定位 MCP 连接，不是业务 `request_id` 或灵矽语音会话。
- 每条消息不超过配置帧限制和契约的 64 KiB；接收队列有界，MCP 连接数受 `max_online_connections` 单独限制。SSE 连接最长保留 30 分钟，客户端到期后重连；单次工具业务操作最长 15 秒。
- 反向代理保持 SSE 流式转发，禁用响应缓冲。不得把 token 放在地址中；代理访问日志不得记录凭据或语音原文。

唯一工具 `propose_task_reply` 的参数为 `{"text":"最终 ASR 原文"}`。inputSchema 从共享契约导出，不依赖客户端联网解析 `$ref`；不接受模型指定设备、目标或请求标识。保留原有空白、标点和文字，拒绝纯空白、超过 16000 码点或超过 16 KiB UTF-8 的文字，不截断。

## 每轮顺序

1. 固件通过已认证 Gateway WSS 调用 `request.prepare`，携带新的 `request_id` 和目标 `agent_session_id`。Gateway 验证绑定与目录，保存 Desktop、Agent 会话、thread/project/title；同设备录音串行，同会话最多一个未结束请求。
2. prepare 成功后才录音。固件向灵矽发送带本轮 `request_id` 的 `listen.start`；这项透传需要平台支持，当前固件尚未接入。
3. 平台只对最终非空 ASR 调用 `propose_task_reply`。每个工具 POST 携带 `X-Linx-Device-Id`、`X-Linx-Agent-Id`、`X-Linx-Session-Id`、`X-Linx-Request-Id`。前三项按平台文档传递，第四项仍待平台支持。Header 名大小写不敏感；缺失、重复或不匹配均拒绝，不读取设备“最新 request”。
4. Gateway 校验凭据限定的设备和智能体、原请求归属与存活状态，再保存原文，进入 `awaiting_confirmation`，推送 `request.proposed`。灵矽语音会话与智能体随首次成功提案固定；同请求后续不得更换来源。
5. 重复 MCP 的相同原文与来源返回已有快照，不重复推送；文字或来源变化返回 CONFLICT。取消/超时后的调用也拒绝，不能污染下一轮。
6. 固件可通过 `request.get` 恢复数据库快照，`request.cancel` 取消确认前请求。`request.confirm` 在确认及固定投递身份落盘后发送 `task.dispatch`；ACK 不代表运行，真实执行事件驱动状态。确认前 Desktop 离线时保留草稿并返回依赖不可用；落盘后结果不确定时为 unknown，不自动重投。目标流程与当前实现边界见[确认执行说明](execution.md)。

重复 prepare 返回原请求，不更换目标或重置时间。等待文字为 120 秒，待确认为 300 秒；截止时间落盘，启动、每次访问及运行时每秒检查，到期保存 `failed` 并推送 `request.updated`。没有平台明确错误时只记录“未收到文字”，不推断识别失败原因。数据库保留终态与 request_id，不复用旧 ID。目录更新不删除在途请求；接收新文字前再次核实固定 thread/project 仍在目录。

成功结果为标准 MCP `content:[{type:"text",text:"业务 JSON"}],isError:false`；失败为 `isError:true` 与共享业务错误。JSON-RPC id 由官方 SDK 管理，不能用作 request_id。工具保存草稿不会发送任何 Desktop 任务。

## 不修改灵矽代码的边界

[灵矽云端 MCP 文档](https://linx.qiniu.com/docs/xrobot/mcp/software-mcp)规定 SSE 入口及三个来源头，但没有给出最终 ASR 固定路由或逐轮 `X-Linx-Request-Id` 的配置承诺。[设备 WebSocket 文档](https://linx.qiniu.com/docs/xrobot/platform/websocket)也不能作为该透传已经支持的证据。

如果平台提供可配置的最终 ASR 工作流或强制工具节点，并能按轮透传 request_id，可在不修改平台代码的情况下接入。只有注册 MCP、配置提示词让模型调用工具，无法保证普通闲聊、拒答等每一条有效语音都触发，也无法保证 ASR 原文不被模型改写。当前实现不会用模糊匹配、设备最新请求或闲聊回退来绕过此要求。

在平台能力和固件接线完成前，只能验收 Gateway 模拟链路，不能声明“所有灵矽语音必调 MCP”或完整语音闭环已完成。下方历史验证记录描述当时范围，新增确认投递与执行事实的验证单独记录在确认执行说明中。

## 验证入口

### 联调日志

DRI：BoleLiu（Gateway）。本次日志改动仅涉及 Gateway 传输层，不改变 MCP 参数、认证、请求状态或固件通知协议。验收包括正常提案、同轮重试、上下文缺失、来源不匹配、参数错误、请求不存在和设备离线的主机模拟链路；真实灵矽接入仍需单独验收。

`log.level=info` 即可查看以下事件，默认 text 格式输出中文摘要；`log.format=json` 可用于按字段筛选。

| 事件 | 阅读方式 |
| --- | --- |
| `mcp.connection.opened` / `mcp.connection.closed` | SSE 建连与结束，包含连接编号、持续时间和结束原因；建连成功不表示已收到识别结果 |
| `mcp.http.rejected` | HTTP 入口拒绝，包含状态码、错误原因和排查提示；检查凭据、Origin、入口、SSE 是否过期或消息大小 |
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

测试使用临时 SQLite、模拟语音文字和真实本机 HTTP/SSE/WSS 协议栈，不连接灵矽或真实 Agent，不产生编码任务。涵盖工具发现、唯一参数、原文保留、普通闲聊接收、多轮 SSE 逐次上下文、认证/来源隔离、重复调用、取消后迟到、超时及恢复。

真实验收必须分别提供：固件 prepare 后录音、灵矽最终 ASR 固定调用与逐轮 ID 透传、错误时不转入通用 LLM、设备显示原文及确认页的证据。现有测试不能替代这些结果。

## 本次本地验证结果

2026-10-25（补齐 MCP 日志）：在独立源码快照执行 `GOCACHE=/tmp/linxbox-gateway-integration-go-cache GOFLAGS=-buildvcs=false make check build release`，并基于同一 coverage.out 运行 `tools/covergate -base origin/main`，格式、静态检查、全部单元/集成测试、整体/关键包/增量包覆盖率与全部包 race 检测均通过。整体语句覆盖率为 **93.14%（2390/2566）**；请求服务所在包 92.11%、传输包 93.50%、存储包 92.26%，所有关键包和增量包均通过原有 90% 门禁，没有新增统计排除或下调门槛。MCP 日志回归覆盖正常/重复调用、上下文与参数错误、来源不匹配、请求不存在、设备离线、通知失败和敏感信息脱敏；测试配置在建连前固定，避免活跃 SSE 期间写入配置。

`make build release` 通过：本机可执行文件及 Linux amd64/arm64 无 CGO 发布构建均已生成；`git diff --check` 通过。Linux 结果为交叉编译，尚未在目标主机运行；容器镜像尚未构建验证。

以上为主机模拟协议链路结果；没有连接灵矽、烧录固件、向真实 Agent 投递任务或部署公网服务。灵矽固定路由与逐轮透传、固件接线、确认执行及真实语音验收仍未完成。

本工作区随后将上述日志与确认执行实现合并，合并后的检查及服务器包记录见 [确认执行说明](execution.md#mcp-日志合入与服务器包2026-10-25)。合并包覆盖率为 92.89%，与上方仅日志改动的历史结果分别记录。
