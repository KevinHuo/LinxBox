# MCP 最终识别文字接入

Refs #14、#1。DRI：BoleLiu（Gateway）。主模块：Gateway；受影响模块：固件、Desktop 与灵矽平台接入；必要跨端评审：KevinHuo（固件）、yydounai1234（Desktop）。协议沿用已合并的 `contracts/`，不新增文字投递 API。

目标是接收每轮最终 ASR 原文并保存为待确认请求。范围包括 MCP 接入、录音目标准备、持久化、取消、超时和来源校验；不包括任务确认投递、Agent 执行或灵矽平台代码修改。

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
6. 固件可通过 `request.get` 恢复数据库快照，`request.cancel` 取消确认前请求。Gateway 当前不支持任务调度，`request.confirm` 返回 DEPENDENCY_UNAVAILABLE，请求保持待确认；不返回假成功。

重复 prepare 返回原请求，不更换目标或重置时间。等待文字为 120 秒，待确认为 300 秒；截止时间落盘，启动、每次访问及运行时每秒检查，到期保存 `failed` 并推送 `request.updated`。没有平台明确错误时只记录“未收到文字”，不推断识别失败原因。数据库保留终态与 request_id，不复用旧 ID。目录更新不删除在途请求；接收新文字前再次核实固定 thread/project 仍在目录。

成功结果为标准 MCP `content:[{type:"text",text:"业务 JSON"}],isError:false`；失败为 `isError:true` 与共享业务错误。JSON-RPC id 由官方 SDK 管理，不能用作 request_id。工具保存草稿不会发送任何 Desktop 任务。

## 不修改灵矽代码的边界

[灵矽云端 MCP 文档](https://linx.qiniu.com/docs/xrobot/mcp/software-mcp)规定 SSE 入口及三个来源头，但没有给出最终 ASR 固定路由或逐轮 `X-Linx-Request-Id` 的配置承诺。[设备 WebSocket 文档](https://linx.qiniu.com/docs/xrobot/platform/websocket)也不能作为该透传已经支持的证据。

如果平台提供可配置的最终 ASR 工作流或强制工具节点，并能按轮透传 request_id，可在不修改平台代码的情况下接入。只有注册 MCP、配置提示词让模型调用工具，无法保证普通闲聊、拒答等每一条有效语音都触发，也无法保证 ASR 原文不被模型改写。当前实现不会用模糊匹配、设备最新请求或闲聊回退来绕过此要求。

在平台能力和固件接线完成前，只能验收 Gateway 模拟链路，不能声明“所有灵矽语音必调 MCP”或完整语音闭环已完成。

## 验证入口

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

2026-10-25：完整执行 `INCREMENTAL_BASE=origin/main make check`，格式、静态检查、全部单元/集成测试、整体/关键包/增量包覆盖率与全部包 race 检测均通过。整体语句覆盖率为 **92.41%（1801/1949）**；请求服务所在包 92.86%、传输包 91.41%、存储包 91.84%，所有关键包和增量包均通过原有 90% 门禁，没有新增统计排除或下调门槛。

`make build release` 通过：本机可执行文件及 Linux amd64/arm64 无 CGO 发布构建均已生成；`git diff --check` 通过。Linux 结果为交叉编译，尚未在目标主机运行；容器镜像尚未构建验证。

以上为主机模拟协议链路结果；没有连接灵矽、烧录固件、向真实 Agent 投递任务或部署公网服务。灵矽固定路由与逐轮透传、固件接线、确认执行及真实语音验收仍未完成。
