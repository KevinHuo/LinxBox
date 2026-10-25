# 固件接入正式 Gateway

Refs #14、#1。已核对 [Issue #14](https://github.com/KevinHuo/LinxBox/issues/14) 的现有消息与恢复语义；其原始设计中的二次确认页面，按本轮明确需求改为“停录 OK 即授权、原文到达后确认”，协议字段不变。固件以 `contracts/` 为唯一字段来源，本文说明设备侧设计及验收边界。当前基线为 `d906469` 的 Go Gateway；旧 Node 演示服务、设备直传识别文字和独立指定播报均不属于本方案。

## 固定交互与链路

1. 任务目录、概览、最近对话和正文从 Gateway 读取；保留粗体字、波形、大号读秒和可配置长按／双击返回。阅读页只读，从概览的“继续沟通”开启语音。
2. 进入概览时后台准备灵矽连接，软件不采集麦克风。选择继续沟通后，先向 Gateway `request.prepare` 固定真实 `agent_session_id`，成功后才能开始本轮录音。
3. 设备在原灵矽 WSS 发送带本轮 `request_id` 的 `listen.start`，流式上传音频。识别原文由灵矽 Agent 经 MCP 转发，设备不上传 `stt.text`，不调用 `request.propose`。
4. 短按一次 OK 结束录音并授权发送本轮文字，波形和读秒立即冻结。停录命令被接受后登记确认意图；同一 `request_id` 的原文进入 `awaiting_confirmation` 后发送一次 `request.confirm`，原文先到也必须等 OK。不再出现第二次确认页面，等待、播放期间 OK 不重录、不重复发送。
5. 灵矽 Agent 在原会话中自行回复，设备不选句子、不请求另一轮 TTS、不播放固定回执。MCP 保存原文不等于任务已投递；语音说“已发送”不能代替 Gateway 确认与 Desktop 执行事实。
6. 原语音流完成尾音收尾、音频休眠后，设备返回概览。业务推送不能制造发送成功或语音播放完成。返回取消时清理尚未执行的准备请求；按 OK 授权的本轮正常播完后继续等待原文和确认结果，不再自动取消。确认已进入发送队列后，超时／断线只查询，不自动重发。

语音和 Gateway 请求标识分别管理。`gateway_agent_session_id` 是连接复用的目标身份；`gateway_request_id` 每次录音更新，只作为本轮关联元数据，不因它变化而重建同目标的灵矽连接。目标切换、连接失效才重新建立。

灵矽 WSS 在完整回复或取消后空闲保留 30 秒；第 29 秒再录音会暂停旧期限，本条结束后重新计满 30 秒。普通返回、阅读和目录刷新不主动关闭连接，心跳不续期。取消发送 abort 并静音排空旧轮，收到真实 `tts.stop` 才允许下一轮，不能假定旧录音已清空。

## 正式协议和刷新

设备连接 `wss://<域名>/v1/devices/connect`，使用独立设备 Bearer 令牌，保持证书、主机名和时间校验。协议帧为 `{id,type,payload}`，响应通过 `reply_to` 关联一个在途操作。

| 操作 | 用途 |
| --- | --- |
| `device.hello` | MAC 身份和当前绑定 |
| `agent_sessions.list`／`agent_session.get` | 任务目录和固定目标详情 |
| `messages.list`／`message.content.get` | 历史及 UTF-8 正文分块 |
| `request.prepare` | 开始录音前固定目标，首次成功只授予一次录音许可 |
| `request.confirm` | OK 授权后等待本轮原文就绪，发送一次确认；成功应答不等于执行成功 |
| `request.get` | 断线、超时及请求状态恢复，不重新授予许可 |
| `request.cancel` | 清理未执行请求，不撤回已投递任务 |

设备不向列表增加 `limit`，不依赖 `last_turn_status`、`delivery_confirmed` 等非正式字段。会话状态只展示服务端契约允许的 idle、running、completed、failed、unknown；unknown 如实显示，不能猜测执行事实。

WSS 鉴权后接收 `device.bound`、`agent_sessions.updated`、`message.available` 和 `request.proposed/updated`，没有额外订阅接口。目录通知合并刷新，正在阅读时新回复只标记并更新目录／详情，不替换原正文和页码。断线后重新 hello、恢复请求、重新读取目录首批，旧代号和旧包 ID 的响应不能覆盖当前页面。

概览优先显示本机保存的当前会话本轮请求状态；结果未知或断线后尚待核对时显示“结果待确认”，明确的完成、失败和取消结果继续保留。没有对应请求时才显示会话目录状态，切换会话不沿用另一会话的请求结果。“有新回复”独立提示，不代表执行成功。当前会话收到 `message.available` 后，概览第一项显示“查看新回复”，OK 直接读取通知中的 `message_id`；正文成功展示才清除此条提示，加载失败或提前退出仍保留。长按退出后第一项变为“阅读本轮回复”，可重新打开同一消息。阅读期间收到新的通知（含同 ID 更新）不替换当前正文和页码，也不会被当前阅读清除；退出后可查看新回复。此记录仅保留在当前会话的内存中，切换会话时清除，不发送已读协议；设备重启后的跨启动请求恢复尚未实现。

进入语音受阻时，概览和请求页优先展示当前可用的真实错误说明；服务端错误同时显示返回的错误码，例如 `CONFLICT`、`NOT_FOUND`。请求页的错误覆盖登记／取消等待提示，较长错误在固定区域滚动显示。真实语音故障优先显示对应的配网、连接、协议或音频原因；正常关闭语音不能用通用网络建议覆盖网关错误。此处仅调整展示，不改变请求状态、录音许可、取消与只读核对行为，也不把查不到请求解释为取消成功。

准备失败的本地恢复有严格释放边界：`request.prepare` 明确返回 `NOT_FOUND`、`INVALID_ARGUMENT`、`UNAUTHENTICATED`、`PERMISSION_DENIED` 或 `CONFLICT`，随后同一请求的 `request.get` 返回 `NOT_FOUND`，且仍未收到任何有效请求快照、没有录音领取、确认意图、确认发送或取消发送记录，才清除本地旧 ID 和取消意图、结束恢复。原始准备错误与目标保留，页面显示“本次未提交”；短按 OK 或返回只退出，再次选择“继续沟通”才生成新 ID，不自动开始录音或重发。

上述释放不生成远端 `failed`／`cancelled` 状态。准备超时、`INTERNAL`、`DEPENDENCY_UNAVAILABLE`、`RESOURCE_EXHAUSTED` 等仍按结果不确定处理；收到过有效快照，或已确认发送、进入 `sending/running/unknown` 的请求，不得仅凭 `NOT_FOUND` 释放。旧包应答和旧请求推送不能恢复已释放的请求或启动新一轮录音。该规则仅释放本次失败准备的本地占用，不会解除服务端其他未结束请求造成的冲突。

## 串口协议日志

INFO 级别日志覆盖 Gateway 业务收发和灵矽文本控制消息；日志输出到串口，不保存日志历史。

- `linx_gateway` 的 `protocol=rx` 可看到 `request.proposed/updated`、响应所对应的操作、状态与错误码；`protocol=tx_queued type=request.confirm` 表示确认已被本地发送队列接受，`tx_rejected` 表示未入队。
- `linx_gateway` 的 `wire=tx` 记录实际 WebSocket 写入长度和返回值；`wire=rx` 记录收到完整消息或控制帧。入队、成功写入和服务端成功响应是不同事实；均不能代替 Desktop 接收和执行结果。
- `linx_voice` 记录发送的 `hello/listen.start/listen.stop/abort`，以及接收的 `hello/stt/tts.start/tts.stop`；未知和无效文本消息只打印固定分类。保留现有音频启动／停止和传输失败记录，音频与心跳不逐包打印。
- 字段仅包含方向／阶段、连接或语音代号、白名单消息类型、字节数、固定状态／错误码及发送／解析结果；不输出令牌、地址、业务 ID、字幕、指令、回复正文或任意外部错误文字。协议类型与状态日志复用已存在的解析结果，不重新解析或复制整帧。

ESP32-C3 工具链量测：Gateway client 常驻结构从 24,400 增至 24,408 字节（回调指针及对齐共 8 字节）；单次回调记录在栈上占 28 字节。灵矽会话未增加字段或缓存。16 KiB 消息的主机替身测试开启与关闭日志时，动态分配次数和峰值相同。上述数值不包含 ESP-IDF 日志格式化与串口驱动内部开销；串口输出耗时及音频并发下的实际栈水位仍需真机验证。可通过 ESP-IDF 日志等级减少输出。

## 展示让出内存

ESP32-C3 没有 PSRAM，网络、音频和显示共用内部 RAM。优先减少展示缓存，不以关闭原语音流程、忽略 TTS 或改用直传文字来省内存。

- 每批缓存前两个会话和前两条历史；收到正式上限五个会话／十六条消息时仍完整校验，再只保留前两条。界面标明本批有省略；服务端 `next_cursor` 仍指向下一批，不虚构本地分页。若希望无遗漏地逐批浏览任务，部署时将现有 `limits.session_page_size` 设为 2。
- 仅保留标题、工程、Agent、状态和必要标识；会话摘要与请求原文在解析验证后丢弃，不长期复制。主机量测 client 从 32,784 降至 24,432 字节，常驻减少 8,352 字节；真实完整文字留在服务端。
- 正文按最多 1024 个 UTF-8 字节分块，不累积整篇；最多记录十六个块起点。非末块的 `next_offset` 必须与 UTF-8 字节位置一致，拒绝跳字、溢出和不前进。
- ID／cursor 最多 128 Unicode 码点、513 字节缓冲；标题 1024 字节，项目名 512，Agent 名 128。用于显示的超限字段明确报错，不截断身份或原文。网络帧最多 64 KiB，解析前限制深度与节点数。
- 离开页面丢弃正文、历史及旧查询。固定数组清零只去掉过期内容，实际常驻节省来自缩容量和删除重复字段；不能把清零称为释放堆。
- 应用网络缓冲按实际大小申请并保留分配余量检查。解析直接读取有界借出帧，去除整帧临时副本；16 KiB 原文回归中，解析额外分配峰从 33,871 降至 17,243 字节（不含已借出的网络帧）。它不是隔离内存分区；两条 TLS、JSON 解析和 Opus 同时运行的峰值仍需真机测试。粗体字库存于 Flash，保留已确认的清晰度。

`linx_device.*` 是唯一 Wi-Fi／校时所有者，Gateway 通过网络意图和 epoch 协调配网与停用。灵矽语音保持既有 30 秒连接生命周期。音频是流式的固定 PCM／Opus 缓冲，不保存完整录音或回复。Gateway 收发和 JSON 处理在 LVGL 锁外，UI 只登记有界意图、读取模型。

## 部署后配置

Gateway 默认关闭。在 menuconfig 的“LinX 网络与语音”打开 `LINX_GATEWAY_ENABLE`，填写 `LINX_GATEWAY_URL` 和 `LINX_GATEWAY_TOKEN`。地址和令牌可以在服务部署后再配；未配置时明确显示错误，不连接旧演示服务，也不回退假数据。Wi-Fi 仍使用手机配网。

Gateway 默认只接受 `wss://`。开发服务明确使用 `ws://` 时，需另开默认关闭的 `LINX_GATEWAY_ALLOW_WS`；例如 `ws://124.220.162.10:8787/v1/devices/connect` 使用 TCP，设备 Bearer 令牌和协议内容明文传输。该开关只影响 Gateway；WSS 仍保留证书、主机名和时间校验，灵矽语音始终使用 WSS。

设备、Desktop、MCP 各用独立凭据。真实 MCP 地址和可信来源头由平台／Gateway 配置，不把令牌放在 URL 或提交到 Git。`sdkconfig` 和固件镜像可能包含凭据，保持本地私有。

## 验证与尚未完成的边界

从固件目录、激活既有 ESP-IDF 环境后执行：

```sh
./tools/validate.sh --static
./tools/validate_linx_host.sh
./tools/validate_coverage.sh
LINX_GATEWAY_BUILD=on ./tools/validate.sh --firmware
LINX_GATEWAY_BUILD=only ./tools/validate.sh --firmware
```

TLS mock 与正式 Go 服务集成需仓库已有 Node、Go、OpenSSL 和 `contracts/requirements.txt` 验证环境：

```sh
LINX_CONTRACT_PYTHON="$PWD/../.local/issue14/contracts-venv/bin/python" ./tools/validate_gateway_mock.sh
LINX_CONTRACT_PYTHON="$PWD/../.local/issue14/contracts-venv/bin/python" ./tools/validate_go_gateway.sh
```

TLS mock 验证真实 C controller、LVGL、分页、推送刷新和单次停录。第二项启动仓库的真实 Go Gateway，模拟 Desktop 查询内容、模拟平台 SSE MCP 和四个逐轮来源头，先验证真实设备 hello 登记后再绑定，再核对原文不变、目标固定、错轮拒绝、OK 后单次确认和唯一任务投递；模拟 Desktop 的 ACK、开始及完整完成结果分别验证 sending、running、completed 展示，不把收到投递或 ACK 当作执行完成。抓包逐条按共享 Schema 校验。全部模拟均不访问私人账号或执行真实 Codex 任务。

2026-10-25 本轮验证：静态检查、真实 LVGL 主机回归、Gateway + 灵矽 + WS 配置的 ESP32-C3 固件构建均通过；自有 C 代码行覆盖率 99.38%、分支 93.31%、函数 100%，每个文件均达到 90% 门槛。TLS mock 和本地正式 Go Gateway 集成均验证了单次确认、明确拒绝及无真实任务投递，所有抓包通过共享 Schema 校验。协议日志包含敏感内容排除与分配峰值回归。本轮未重新烧录，未实测实际部署服务、灵矽平台与已绑定另一台电脑的完整确认执行链路。

**服务端版本边界：** 上述 2026-10-25 记录使用旧服务 `d906469`，当时确认返回 `DEPENDENCY_UNAVAILABLE` 且未投递。当前仓库已实现确认投递，正式 Go 集成用例已按动态 hello 登记、绑定、单次投递和执行回执更新；该本机模拟结果不代表线上 Codex 执行成功。Agent 仍须依据真实工具与执行结果反馈，不能把保存提案或收到 ACK 说成执行完成。其余接口状态见[三端接口清单](interface-status.md)。

**当前平台限制：** [正式 MCP 接入说明](../../gateway/docs/mcp.md)明确要求每轮 `X-Linx-Device-Id/Agent-Id/Session-Id/Request-Id`，其中 request_id 透传及最终 ASR 原话固定路由仍待灵矽平台支持。设备发送 `listen.start.request_id` 不等于平台已经透传；不使用“最新请求”推断，也不让模型猜目标。

完成代码／mock 不代表真实闭环通过。服务部署后仍要分别验证：TLS 配置、真实目录与推送、逐轮可信头和原话、MCP→Desktop 实际投递、Agent 自然回执、同连接多轮复用、取消排空及双 TLS／音频内存峰值。
