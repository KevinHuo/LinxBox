# 三端共享消息契约

对应 [Issue #14](https://github.com/KevinHuo/LinxBox/issues/14)。本 PR 供固件、Gateway、Desktop 共同评审；合并后本目录 Schema 是字段契约的唯一正文。Schema 定义消息格式，不代表三端功能或灵矽逐轮透传已经实现。

DRI：BoleLiu（Gateway）；受影响模块：固件、Gateway、Desktop；必要评审：KevinHuo（固件）与 yydounai1234（Desktop）。

## 文件与使用入口

使用 JSON Schema Draft 2020-12，开启 `date-time` 格式校验，预加载本目录全部 Schema，离线解析相对 `$ref`。`$id` 是稳定标识，不是需要请求的 HTTP 地址；禁止校验不可信消息时自动联网取 Schema。

| 文件 | 校验入口 |
| --- | --- |
| [common.schema.json](common.schema.json) | 共享对象库，通过 `#/$defs/对象名` 引用，不单独校验整包 |
| [firmware-gateway.schema.json](firmware-gateway.schema.json) | 根入口为全部消息；按连接方向选择 `FirmwareToGateway` / `GatewayToFirmware` |
| [desktop-gateway.schema.json](desktop-gateway.schema.json) | 根入口为全部消息；按连接方向选择 `DesktopToGateway` / `GatewayToDesktop` |
| [mcp.schema.json](mcp.schema.json) | 根入口 / `Arguments` 是工具参数；`Result` 是业务返回，二者分开校验 |
| [fixtures/](fixtures/) | 每例含 `name/schema/valid/data`；这是测试包装，线上只发送 `data` |

所有线上对象默认拒绝额外字段；字段只有出现在 `required` 中才必填。可省略不等于可传 `null`。`offset` 省略时业务代码按 0 处理；Schema 的 `default` 不会自动填值。ID 和分页 cursor 暂定 1～128 字符；标题、项目名、Agent 名非空，摘要允许空字符串。字符长度按 Unicode 码点计，不按 UTF-8 字节数。

列表一次最多 5 个会话。历史按最新到最旧返回最终消息元数据，一批最多 8 轮（最多 16 条 user/assistant 消息）；服务端必须保留轮次边界，不能仅用条数代替轮次。分页 `next_cursor=null` 表示结束，首批不传 cursor。正文 offset / next_offset 是 UTF-8 字节偏移，不是字符位置或屏幕页码。

## 固件与 Gateway

固件连接使用配置的设备凭据，MAC 只是定位信息；连接已鉴权后才处理 `device.hello`。

| 方向 | type | payload / 成功返回对象 |
| --- | --- | --- |
| 固件 → Gateway | `device.hello` | `mac_address` → Binding |
| 固件 → Gateway | `agent_sessions.list` | 可选 `cursor` → SessionPage |
| 固件 → Gateway | `agent_session.get` | `agent_session_id` → AgentSession |
| 固件 → Gateway | `messages.list` | `agent_session_id`、可选 `cursor` → MessagePage |
| 固件 → Gateway | `message.content.get` | `agent_session_id/message_id`、可选 `offset` → ContentChunk |
| 固件 → Gateway | `request.prepare` | `request_id/agent_session_id` → Request |
| 固件 → Gateway | `request.confirm/cancel/get` | `request_id` → Request |
| Gateway → 固件 | `response` | `reply_to` 配对原包；`ok:true,data` 或 `ok:false,error` |
| Gateway → 固件 | `device.bound` | BoundBinding，Desktop 不可为空 |
| Gateway → 固件 | `request.proposed` | 已保存文字的 Request，状态为 awaiting_confirmation |
| Gateway → 固件 | `request.updated` | 当前完整 Request |
| Gateway → 固件 | `agent_sessions.updated` | 空对象，通知刷新目录 |
| Gateway → 固件 | `message.available` | `agent_session_id/message_id` |

每个操作均有独立的 `$defs["操作.response"]`，例如 `#/$defs/messages.list.response`。Gateway/固件先按 `reply_to` 找到原操作，再用该定义校验 data。根入口只能检查 data 属于某个已知对象；空历史与空会话列表形状相同，根入口不能代替原查询配对。

MAC 输入接受连续 12 个十六进制字符、统一冒号或统一横线格式；Gateway 规范输出大写冒号格式。Desktop 用户输入 MAC 后发送 `device.bind`；Gateway 校验已认证双方、在线设备与配对权限，成功保存绑定并发送双方通知。重复同一绑定幂等，不覆盖其他 Desktop 的绑定。

## Desktop 与 Gateway

保留已有 `protocol_version="1.0"/message_id/sequence/type/sent_at/payload` 和可选 `expires_at`。整数序号非负，时间为 RFC 3339 字符串。保留已有 `client.hello/heartbeat`、`project.upsert/revoke`、`task.dispatch/ack/cancel`、全部任务事件及 TaskResult 必填字段。

| 方向 | 新增 type | 用途 |
| --- | --- | --- |
| Desktop → Gateway | `device.bind` | 提交固件 MAC |
| Gateway → Desktop | `binding.result` | 按 reply_to_message_id 回答绑定成功/失败 |
| Desktop → Gateway | `agent_sessions.sync` | items 全量替换本 Desktop 的会话目录 |
| Gateway → Desktop | `agent_sessions.sync.result` | 回答本次全量保存是否成功 |
| Gateway → Desktop | `messages.list` | 读取历史元数据 |
| Gateway → Desktop | `message.content.get` | 读取正文块 |
| Desktop → Gateway | `query.result` | 回答历史/正文查询 |
| Desktop → Gateway | `message.available` | 通知有最终回复 |

全量记录为 `agent_session_id/thread_id/project_id/project_name/agent/title/status/summary`。只替换发送方目录；同一 Desktop 串行发送并等待保存应答，变化合并到下一份完整目录。`items=[]` 确认目录为空，不得用于枚举失败或掉线兜底。缺失会话移出目录、停止新投递；保留在途 request/task 映射。超限或任一记录非法时整次拒绝，保留旧目录。保存成功后通知固件 `agent_sessions.updated`；固件丢弃列表旧 cursor，从首批重取并刷新原详情，不改变固定输入目标。

查询流程：固件 `messages.list(id=f1)` → Gateway `messages.list(message_id=g1)` → Desktop `query.result(payload.reply_to_message_id=g1)` → Gateway `response(reply_to=f1)`。Gateway 保存认证来源、两个包 ID、目标与原查询参数，并在完成/超时后释放映射，拒绝迟到或串线应答。

`query.result` 首版只回答历史/正文；根据原查询分别使用 `#/$defs/messages.list.result` 或 `#/$defs/message.content.get.result` 校验。没有 `task.status.get`。`request.get` 只读 Gateway 已保存快照，断线导致结果不确定时为 unknown，不推断未执行、不自动重发。

兼容边界：

- 本 PR 不修改 Desktop 运行代码。现有构建尚未实现新增消息，不可直接宣称兼容完成。后续实现 PR 应接入共享 Schema 或从 Schema 生成类型，并替换本地重复定义。
- 旧 `presentation.upsert/revoke`、`command.receipt` 是未定结构占位，不纳入新契约；`agent_session.upsert` 也不提供。会话目录统一使用全量同步。
- 既有 `PublicError` 保留 `retryable/request_id/details?`；新增接口错误用最小 `Error{code,message}`，不能互换后删掉旧必填字段。
- `task.dispatch.continuation` 保留 `null` 与 `{parent_task_id}`，增加 `{agent_session_id,thread_id}`，两种非空形式不能混传。固件追问只能使用已固定会话的可验证续接；null 仅用于既有协议的独立新任务，不能作为续接失败的回退。
- TaskEvent 的 state/stage 沿用现有字符串字段；其值和事件一致性须由执行事实校验。`task.started` 才推进 Request.running；ACK accepted/duplicate 不等于正在运行。`task.completed` 且 TaskResult.status=succeeded 才推进 completed；其余终态必须按实际事实映射。首版不开放审批和执行中断入口，保留消息用于识别真实状态，不据此假报成功或自动授权。

## 灵矽 MCP

只注册 `propose_task_reply`，`inputSchema` 使用 `mcp.schema.json#/$defs/Arguments`，唯一模型参数是 `text`。最终 ASR 原文固定调用该工具，不让通用 LLM 改写、闲聊或自行选择工具。

| 上下文 | 传递方式 |
| --- | --- |
| device_id | 已有 `X-Linx-Device-Id`，与认证来源核对 |
| 灵矽智能体/会话 | 已有 `X-Linx-Agent-Id/X-Linx-Session-Id` |
| request_id | 拟新增 `X-Linx-Request-Id`，从本轮 listen.start 绑定并透传，仍待平台支持 |
| Desktop 与 Agent 目标 | Gateway 从 request.prepare 保存记录取得，不是模型参数 |

请求头大小写按 HTTP 规则处理，必须认证 MCP 凭据；不能把头字段的存在当成可信证明。Header 与鉴权不进入工具参数。灵矽语音会话不是 `agent_session_id`。

返回业务体按 `Result` 校验，交给 MCP SDK 封装为 `content:[{type:"text",text:"业务 JSON 字符串"}]`；成功 `isError=false`，失败 `isError=true`。不改 JSON-RPC envelope；SDK 的调用 ID 不是 request_id。重复调用返回原请求状态，因此成功 data 允许已确认/终态 Request，但必须已有非空文字。

逐轮 ID 透传和 ASR 固定路由尚待灵矽实现/联调，当前只能做 mock。所有有效语音必须走该固定 MCP 路径；识别失败或 MCP 不可用时不回退聊天。

## 必须由业务代码检查

Schema 检查字段形状，以下规则仍需三端各自实现和集成测试：

- WSS/MCP 鉴权、双方绑定、项目授权、会话与消息归属；MAC 不是凭据。
- 每台设备录音串行、每会话最多一个未结束请求；prepare 固定目标，确认前不得执行。
- 同一 request_id 不可换文字/目标；确认与 task_id 去重并持久化；取消/过期的旧 MCP 不得写入新录音。
- awaiting_text 超时 120 秒、awaiting_confirmation 超时 300 秒；未知执行不自动重投。
- 整帧最多 64 KiB UTF-8，草稿最多 16000 码点且最多 16 KiB UTF-8，正文块最多 1024 UTF-8 字节；偏移必须位于字符边界。Schema 字符长度不等于字节限制。
- 全量列表 agent_session_id 不重复、目录原子替换、只接受当前有效连接；有界历史按轮次分页，cursor 绑定查询快照和认证主体。
- 原查询与回应匹配、事件顺序和去重、实际 task 事实与结果一致；保存前做大小限制，错误信息脱敏。

## 本地检查

Python 3.11 或更新版本，仅为契约校验工具，不要求固件/Gateway/Desktop 运行 Python。

```sh
python3 -m venv .venv
.venv/bin/pip install -r contracts/requirements.txt
.venv/bin/python contracts/validate.py
.venv/bin/coverage run --branch --source=validate -m unittest discover -s contracts -p 'test_*.py'
.venv/bin/coverage report --fail-under=90
git diff --check
```

校验器先检查全部 Schema 合法性，再按 fixture 指定的定义验证合法/非法样例，任何预期不符都失败；测试还检查消息根入口与调用方向。覆盖率只统计校验工具 `contracts/validate.py`，不代表 Schema 规则穷尽、三端业务实现或真实语音链路覆盖率。
