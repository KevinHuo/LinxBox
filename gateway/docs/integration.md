# 三端只读联调

关联 Issue：[#14](https://github.com/KevinHuo/LinxBox/issues/14)。DRI：BoleLiu（Gateway）；受影响模块：Gateway、Desktop、固件；跨端评审：KevinHuo（固件）、yydounai1234（Desktop）。

本轮验收范围是连接、绑定、会话目录、历史列表、正文块、新消息通知及断线重连。MCP 语音提案和任务执行不在本轮范围。共享字段仍以根目录 `contracts/` 为准，没有修改契约或另加文字投递 API。

## 运行 Gateway

```sh
cd gateway
go mod download
make generate
cp config.example.json config.local.json
# 编辑配置，使用联调专用凭据；database.path 指向已存在且可写的目录。
go run ./cmd/linxbox-gateway --config ./config.local.json
```

设备、Desktop 的 token 必须分别唯一，两者 `subject` 相同。配置只需账号凭据，无需设备 MAC 或 `pairing.rules`。
两端建立 WS 的顺序不限；固件在连接后 10 秒内发送 `device.hello` 上报自己的 MAC，
Gateway 自动将 MAC 登记到该凭据的账号，返回 Binding。Desktop 获得 MAC 后发送 `device.bind`。
尚未 hello 的连接不计作在线设备，业务请求返回 `CONFLICT`；超过 10 秒未成功 hello 则关闭连接。
未登记的 MAC 不能绑定，返回 `DEPENDENCY_UNAVAILABLE`。真实 token 只保存在受限本机配置中。

首次绑定会检查设备当前在线，离线返回 `DEPENDENCY_UNAVAILABLE` 且不写入绑定；
已有同一账号、同一 Desktop 的绑定允许离线重复提交并返回原结果。
读取绑定、目录、历史路由、请求快照和发送通知时，均复核持久化绑定的 subject 与双方当前凭据归属。
设备登记记录持久化 MAC、账号和凭据 SHA-256 指纹，不保存明文 token。
移除或改变凭据账号后，关联的旧绑定不能继续读取或推送数据；同账号的新凭据可在重新 hello 后恢复原绑定。
不同账号上报同一个已登记 MAC 返回 `PERMISSION_DENIED`，不会替换原设备连接或归属。
同一连接成功登记后不可更换 MAC；同账号的不同设备可使用账号接入凭据分别 hello，
相同 MAC 重连替换旧连接。MAC 上报本身不证明硬件真实性，固件凭据应只交给对应账号的受信设备。

升级时新增登记表，已有绑定自动保留原账号。旧配置的 `credentials.devices[].device_id` 仅用于为旧绑定补齐凭据关联，
不限制新 hello 可上报的 MAC；旧 `pairing.rules` 仍能解析，但不再作为授权白名单。
直接使用新配置升级时，已有设备重新 hello 后恢复凭据关联，不需要清空数据库。

同一端口的路径分别是 `/v1/devices/connect` 和 `/v1/clients/connect`。采用子协议认证时客户端请求 `linxbox.v1, linxbox-token.<token>`，Gateway 只返回 `linxbox.v1`，不回显 token。固件也可通过 Authorization Bearer 头认证。

生产部署使用 HTTPS/WSS 和有效证书。本轮临时公网联调可通过 HTTP/WS 访问同一入口，
例如固件 `ws://124.220.162.10:8787/v1/devices/connect`、Desktop `ws://124.220.162.10:8787/v1/clients/connect`；
WS 不加密，使用联调专用凭据。设备上的 127.0.0.1 指向设备自身，不能作为云端地址。

## 当前开放接口

以下为本 PR 已实现的服务入口；实际域名由部署方提供，提交 PR 本身不代表已有公网实例。
完整字段、必填项与枚举以根目录[共享 Schema 说明](../../contracts/README.md)为准。

| 入口 | 调用方 | 用途与应答 |
| --- | --- | --- |
| `GET /healthz` | 探测程序 | 存活检查；正常返回 HTTP 200 |
| `GET /readyz` | 探测程序 | 就绪检查；就绪返回 200，未就绪返回 503 |
| `WSS /v1/devices/connect` | 固件 | 设备凭据认证后的持久 WebSocket；消息见下表 |
| `WSS /v1/clients/connect` | Desktop | Desktop 凭据认证后的持久 WebSocket；消息见下表 |

WebSocket 认证失败以关闭码 `4401` 拒绝，帧超过配置上限以 `1009` 关闭。
服务端每 30 秒发 WebSocket ping，客户端须处理 ping / pong；读期限为 60 秒。
`client.heartbeat` 是应用消息，不能替代 WebSocket pong。上述时间为当前实现固定值。

所有固件 response 都按 JSON 编码后的实际字节数检查 `limits.max_frame_bytes`，
超限返回保持原 `reply_to` 的小型 `RESOURCE_EXHAUSTED` 错误；正常数据和错误消息均适用。
64 KiB 是协议硬上限，不是固件常驻缓冲目标。帧预算需包括包头及 JSON 转义开销，
按板卡剩余内存确定配置；本次同时验证了 1 KiB 配置下的有界错误，但未验证 400 KB 总内存设备的真机预算。

### 固件通道

| 方向 | type | payload / 应答 |
| --- | --- | --- |
| 固件 → Gateway | `device.hello` | `{mac_address}` → Binding，未绑定时 `desktop_client_id=null` |
| 固件 → Gateway | `agent_sessions.list` | 首批 `{}`，续批 `{cursor}` → `{items,next_cursor}`，每批最多 5 项 |
| 固件 → Gateway | `agent_session.get` | `{agent_session_id}` → AgentSession |
| 固件 → Gateway | `messages.list` | `{agent_session_id,cursor?}` → `{items,next_cursor}` |
| 固件 → Gateway | `message.content.get` | `{agent_session_id,message_id,offset?}` → `{text,next_offset}`；offset 默认 0 |
| Gateway → 固件 | `response` | 外层 `reply_to` 回指原包 `id`；payload 为 `{ok:true,data}` 或 `{ok:false,error}` |
| Gateway → 固件 | `device.bound` | `{device_id,desktop_client_id}`，Desktop 非 null |
| Gateway → 固件 | `agent_sessions.updated` | `{}`，通知从目录首页刷新 |
| Gateway → 固件 | `message.available` | `{agent_session_id,message_id}`，通知按需读取新回复 |

固件帧固定为 `{id,type,payload}`，仅响应增加 `reply_to`。可选 cursor 首批省略，不传 null。
目录 cursor 绑定设备、Desktop、目录版本，5 分钟过期；未知、过期或版本失效返回 `CONFLICT`，
从首页重取；跨主体使用返回 `INVALID_ARGUMENT`。Gateway 重启后旧 cursor 失效。

### Desktop 通道

Desktop 的双向帧均保留 `protocol_version="1.0"`、`message_id`、`sequence`、`type`、
`sent_at`、`payload`，可带 `expires_at`。不能使用固件的精简外层替代。

| 方向 | type | payload / 用途 |
| --- | --- | --- |
| Desktop → Gateway | `client.hello` | `{desktop_version,protocol_versions:["1.0"],last_acked_sequence,capabilities}`；当前只记录，不返回 hello ACK |
| Desktop → Gateway | `client.heartbeat` | `{client_time,summary?}`；当前只记录，不返回 ACK |
| Desktop → Gateway | `device.bind` | `{mac_address}`，电脑身份来自认证连接 |
| Gateway → Desktop | `binding.result` | `{reply_to_message_id,ok,data?,error?}`，成功 data 为已绑定 Binding |
| Desktop → Gateway | `agent_sessions.sync` | `{items:SessionRecord[]}`，原子替换该 Desktop 的全部目录 |
| Gateway → Desktop | `agent_sessions.sync.result` | `{reply_to_message_id,ok,error?}`；成功时没有 data |
| Gateway → Desktop | `messages.list` | `{agent_session_id,cursor?}`，读取原会话的历史；外层含查询 message_id / expires_at |
| Gateway → Desktop | `message.content.get` | `{agent_session_id,message_id,offset?}`，读取原消息正文 |
| Desktop → Gateway | `query.result` | `{reply_to_message_id,ok,data?,error?}`，按原查询类型返回数据；成功 data 与失败 error 互斥 |
| Desktop → Gateway | `message.available` | `{agent_session_id,message_id}`，最终回复可读后发送 |

SessionRecord 每项必须包含 `agent_session_id/thread_id/project_id/project_name/agent/title/status/summary`。
Gateway 只向固件投影 AgentSession，不把 `thread_id/project_id` 附在设备目录中。
历史仅含最终 user / assistant 消息，单批最多 8 轮、16 条；每项为 `{message_id,role,summary}`，
summary 最多 120 个 Unicode 码点。Desktop 必须保留轮次边界，不能只按消息条数截取。
正文由 Desktop 按 UTF-8 字符边界切成最多 1024 字节的块，`next_offset=null` 表示结束；
非末块 next_offset 必须等于输入 offset 加本块 UTF-8 字节数。

合法请求的业务错误返回最小 `{code,message}`；未通过 Schema 的入站消息当前记录后丢弃，
不会伪造成功应答，客户端需有自己的等待超时。默认查询超时 15 秒，每设备最多 4 条、全局 128 条；
Desktop 离线或查询中断返回 `DEPENDENCY_UNAVAILABLE`，超出并发限制返回 `RESOURCE_EXHAUSTED`。

只读阶段之外，Gateway 已增加 SSE MCP 与 `request.prepare/get/cancel`，见 [MCP 接入说明](mcp.md)。
`request.confirm` 和调度未实现；`task.*` 既有事件仅识别和记录，尚不推进执行状态。
本轮只读验收仍不能证明真实语音提案、确认投递、执行或任务状态回传已完成。

## Desktop 接线清单

Desktop 的连接与契约处理入口见 `desktop/main/gateway-connection.ts` 和 `desktop/daemon/gateway-client/frames.ts`。
部署后仍需在 Desktop 配置实际 Gateway 地址和凭据，并按以下消息完成真实联调验收：

1. 核对 Electron 主进程 GatewayClient 的连接配置，确认能接收 binding.result、agent_sessions.sync.result、messages.list、message.content.get。连接后发送 client.hello，并处理 WebSocket ping / pong。
2. 用户输入 MAC 后发送 device.bind，按 payload.reply_to_message_id 关联结果。
3. 从本机明确允许共享的项目读取 Agent 会话。会话的项目授权、thread 归属和本机路径校验仍由 Desktop 负责；不将全部私人历史自动上传。
4. 串行发送 agent_sessions.sync 并等待保存应答；变化合并到下一份完整目录。枚举失败或掉线不能发 items=[]，成功枚举为空才表示清空目录。
5. 收到历史 / 正文查询后，按原 agent_session_id 查本机 thread，仅返回该会话的最终 user/assistant 消息。保留轮次边界，历史每批最多 8 轮、16 条，最新到最旧；正文最多 1024 UTF-8 字节。
6. 将查询结果放入 query.result；payload.reply_to_message_id 必须等于 Gateway 原查询的 message_id。失败使用最小 Error，不把 provider 原始异常、私有路径或凭据放进错误。
7. 有最终消息时发送 message.available。Gateway 只转发标识，正文由固件按需查询。查询关联关闭后不重放迟到结果。

三种 ID 不可混用：Desktop 帧 message_id、消息正文的 message_id、Agent 会话的 agent_session_id。query.result 是被查询触发的回应，不是定时状态上报。

最小 Desktop 建连消息示例，capabilities 仅保留现有必填字段，不代表已启用能力协商：

```json
{"protocol_version":"1.0","message_id":"hello-1","sequence":1,"type":"client.hello","sent_at":"2026-10-25T10:00:00Z","payload":{"desktop_version":"integration-0.1","protocol_versions":["1.0"],"last_acked_sequence":0,"capabilities":[]}}
```

## 固件接线清单

固件实现方应增加独立的 Gateway 只读连接，保持既有灵矽语音连接的职责不变：

1. 认证建立 WS 后 10 秒内发 device.hello，使用自己的真实 MAC，无需服务端预登记；读取 Binding，未绑定时不能显示为已连接电脑。
2. 收到 device.bound 后从 agent_sessions.list 首页读取目录；一次最多 5 项。
3. 进入会话时读取 agent_session.get 和 messages.list；选中消息后通过 message.content.get 分块取正文。
4. 固件每个请求使用唯一 id，通过 response.reply_to 配对。正文 offset / next_offset 是 UTF-8 字节位置，不是字符数或屏幕页码。
5. 收到 agent_sessions.updated 后丢弃旧目录 cursor，从首页重读；刷新详情不改变用户已固定的输入目标。收到 message.available 后刷新对应会话历史。
6. 超时或 Desktop 离线时展示读取失败；重连后重新 hello 并主动读取快照。不能假定断线期间的推送会自动补发，也不能把旧查询结果用于新页面。

设备的内存、字形覆盖、按键响应和 LVGL 线程安全须由固件实现及真机验收。Gateway 的主机测试不替代硬件验证。

最小固件建连消息示例，MAC 由固件在连接后上报：

```json
{"id":"hello-f1","type":"device.hello","payload":{"mac_address":"AA:BB:CC:DD:EE:01"}}
```

## 查询流程与保护

固件 messages.list(id=f1) → Gateway messages.list(message_id=g1) → Desktop query.result(reply_to_message_id=g1) → Gateway response(reply_to=f1)。正文查询采用相同流程。

例如固件发出：

```json
{"id":"f1","type":"messages.list","payload":{"agent_session_id":"session-1"}}
```

Desktop 收到 Gateway 转发的查询，读取结果后返回（g1 必须换成实际收到的 message_id）：

```json
{"protocol_version":"1.0","message_id":"answer-1","sequence":2,"type":"query.result","sent_at":"2026-10-25T10:00:01Z","payload":{"reply_to_message_id":"g1","ok":true,"data":{"items":[{"message_id":"msg-1","role":"assistant","summary":"模拟最终回复"}],"next_cursor":null}}}
```

固件随后收到的 response 外层 `reply_to` 为 f1，不是 g1；查询 data 中的 msg-1 则用于后续正文读取。

Gateway 固定设备 / Desktop 主体、双方连接代次、两个包 ID、原操作和参数；按原操作具体 Schema 校验应答。不同来源、不同连接代次、过期、重复和串线结果拒绝。设备断线释放其关联；Desktop 断线向仍在线的原设备返回 DEPENDENCY_UNAVAILABLE。读取不自动重试。

连接替换与同一主体的业务操作串行化，旧连接不能覆盖新目录；达到在线连接数上限时，同一身份仍可替换自己的旧连接。分页 cursor 是有界、临时、绑定主体的随机标识，目录版本变化后失效。

## 主机模拟与验收

无配置的本机模拟入口：

```sh
cd gateway
make smoke-local
```

该命令使用临时配置、专用假凭据和临时 SQLite 数据库，启动真实回环 HTTP / WebSocket
服务，并模拟固件和 Desktop。首次连接、同一数据库重复运行、Gateway 重启后恢复各执行一轮，
每轮检查绑定、目录分页、会话详情、历史、中文正文连续分块、新消息通知和认证拒绝。
重启验证在重新同步前读取绑定和目录快照，避免把重新写入当作恢复成功。
测试退出后清理本次配置与数据库，不访问私人项目，不调用模型或 MCP。

启动 Gateway 后另开终端：

```sh
cd gateway
make smoke
make check
make release
```

make smoke 使用配置中的首个设备和 Desktop 凭据，写入模拟目录。只能针对专用联调实例运行，不能针对正在使用私人目录的实例。它验证真实网络入口的握手、绑定、5+1 分页、历史 / 正文查询、新消息通知和错误凭据拒绝；不会启动 Agent 或调用模型。持久数据库上重复运行也应通过。

按以下顺序记录证据：

| 层次 | 验收项 | 当前不能据此宣称 |
| --- | --- | --- |
| 自动化 / 内存 WebSocket | 协议、归属、超时、旧代次、UTF-8 边界、race | 公网网络、TLS 和硬件已验证 |
| TCP 主机模拟 | make smoke-local；外部专用实例运行 make smoke，至少连续执行两次 | 真实 Desktop 和真实固件已接好 |
| 真 Desktop + 固件 | 显示真实会话、读取最终历史 / 正文、最终消息刷新、断线重连 | MCP 或编码执行已完成 |

交付给两端的验收顺序：设备 hello → Desktop hello / bind → 同步至少 6 条模拟会话 →
固件读取 5+1 目录和会话详情 → 历史 / 中文正文连续分块 → 新消息刷新 →
Desktop 断线显示失败 → 两端重连后主动读取快照。首先使用固定模拟数据，确认传输与界面正确后，
再替换为已授权的真实会话读取。远程专用实例可在本机执行：

```sh
go run ./cmd/linxbox-smoke --config ./config.local.json --url wss://gateway.example.com
```

替换为实际联调域名，并使用服务端已配置的对应凭据；该命令会覆盖所选 Desktop 的目录，
不能对私人目录实例运行。每轮预期输出 `SMOKE PASS: 15 项检查通过`。

## 部署

Ubuntu 24.04 的二进制安装、升级命令、两端接入地址和请求日志排查见
[WS 联调部署步骤](ws-deployment.md)。固件模拟器可通过 `--device-mac` 指定主动上报的 MAC，
无需修改服务端配置。

直接部署使用 `make build` 的本机构建或 `make release` 的对应 Linux 架构产物，配置使用绝对数据库路径。systemd 模板通过 StateDirectory 建立数据目录，程序本身不自动创建任意父目录。

容器从仓库根目录构建上下文运行：

```sh
docker compose -f gateway/deploy/compose.yaml up --build -d
```

事先准备 gateway/deploy/gateway.json：容器内 listen.addr 为 0.0.0.0:8787，database.path 为 /var/lib/linxbox/gateway.db；该文件不提交。挂载配置需允许容器非 root 用户读取，并保持受限权限。Compose 默认只向宿主机回环地址发布端口，公网由反向代理提供 HTTPS/WSS。

运行镜像不携带 Go 工具链、wget 或 shell；healthcheck 调用程序自身。SQLite 数据位于独立卷，禁止同时启动多个实例共享同一数据库。容器构建、目标机器运行、真实 TLS 与真机验证必须另行执行，不能用交叉编译成功代替。

## 动态登记与日志验证

2026-10-25 本机验证：`make check` 通过格式、静态检查、全部测试、覆盖率与 race。
整体覆盖率 92.23%；本次变更包均达到 90%，包括 app 92.86%、service 92.09%、transport 90.77%。
`make build release` 完成本机和 Linux amd64 / arm64 无 CGO 构建。

真实回环 TCP 的模拟两端未配置 MAC 或白名单，分别验证首次联调、重复执行和 Gateway 重启后恢复；
另有回归测试验证跨账号拒绝、旧绑定升级、待 hello 上限与超时、同 MAC 重连、原连接不可切换 MAC。
请求日志测试验证成功、对端错误和超时查询的两端 ID 关联，并检查凭据、标题、正文、对端错误文案和畸形 payload 不进入日志。
这些结果不代表腾讯云部署、真实固件、真实 Desktop 或 MCP 平台已完成验收。

### 可读日志与 body 验证

默认日志格式改为 text，JSON 可通过 log.format 显式启用。接口字段、鉴权、业务状态和数据库未因日志优化而改变。
info 记录中文流程、统一 trace_id、请求 body、结果和失败原因；分发/入队细节降为 debug。
message_id 为网络包 ID，request_id 专指业务请求；历史查询记录实际目标及 Desktop 应答包 ID。
日志 body 是有界脱敏视图，正文、标题、凭据和路径不直接输出，不能用于报文重放。

2026-10-25 执行 make check 通过：整体语句覆盖率 92.97%（2234/2403），transport 93.06%，全部关键包达到 90%，全部包 race 通过。
回归复现缓存会话读取、prepare/get/cancel 和 Desktop 返回 NOT_FOUND 的流程；
验证 get/cancel 使用持久快照中的会话 ID、同一查询默认仅保留三条关键记录及一条失败告警，
并检查 body 的脱敏、数组/深度/字节预算和文本/JSON 格式。该验证仍为主机模拟，未替代目标服务器或真机验收。
