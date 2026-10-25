# 固件接入正式 Gateway

Refs #14、#1。固件以 `contracts/` 为唯一字段来源，本文说明设备侧设计及验收边界。当前基线为 `d906469` 的 Go Gateway；旧 Node 演示服务、设备直传识别文字和独立指定播报均不属于本方案。

## 固定交互与链路

1. 任务目录、概览、最近对话和正文从 Gateway 读取；保留粗体字、波形、大号读秒和可配置长按／双击返回。阅读页只读，从概览的“继续沟通”开启语音。
2. 进入概览时后台准备灵矽连接，软件不采集麦克风。选择继续沟通后，先向 Gateway `request.prepare` 固定真实 `agent_session_id`，成功后才能开始本轮录音。
3. 设备在原灵矽 WSS 发送带本轮 `request_id` 的 `listen.start`，流式上传音频。识别原文由灵矽 Agent 经 MCP 转发，设备不上传 `stt.text`，不调用 `request.propose` 或 `request.confirm`。
4. 短按一次 OK 结束录音，波形和读秒立即冻结；发送 `listen.stop` 后等待原语音会话的回复，不再出现“确认指令”。等待、播放期间 OK 不重录、不重复发送。
5. 灵矽 Agent 等待 MCP 的真实结果，自行回答“已发送”“已发送 codex”或“未能成功发送，原因…”。设备不选句子、不请求另一轮 TTS、不播放固定回执。
6. 原语音流完成尾音收尾、音频休眠后，设备返回概览。业务推送不能制造发送成功或语音播放完成。取消／失败／正常结束时清理尚未执行的准备请求；执行中及结果不确定的状态不盲目重发。

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
| `request.get` | 断线、超时及请求状态恢复，不重新授予许可 |
| `request.cancel` | 清理未执行请求，不撤回已投递任务 |

设备不向列表增加 `limit`，不依赖 `last_turn_status`、`delivery_confirmed` 等非正式字段。会话状态只展示服务端契约允许的 idle、running、completed、failed、unknown；unknown 如实显示，不能猜测执行事实。

WSS 鉴权后接收 `device.bound`、`agent_sessions.updated`、`message.available` 和 `request.proposed/updated`，没有额外订阅接口。目录通知合并刷新，正在阅读时新回复只标记并更新目录／详情，不替换原正文和页码。断线后重新 hello、恢复请求、重新读取目录首批，旧代号和旧包 ID 的响应不能覆盖当前页面。

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

TLS mock 验证真实 C controller、LVGL、分页、推送刷新和单次停录。第二项启动仓库的真实 Go Gateway，模拟 Desktop 查询内容、模拟平台 SSE MCP 和四个逐轮来源头，验证原文不变、目标固定、错轮拒绝以及保存提案不会变成执行成功。抓包逐条按共享 Schema 校验。全部模拟均不访问私人账号或执行真实 Codex 任务。

**当前服务端限制：** `d906469` 的 `propose_task_reply` 只持久化到 `awaiting_confirmation`；`request.confirm` 返回 `DEPENDENCY_UNAVAILABLE`，尚未投递 Desktop。此时 Agent 不能说“已发送”，必须依据真实工具结果说明未能发送。硬件不会通过自动确认或旁路补齐这一缺口。

**当前平台限制：** [正式 MCP 接入说明](../../gateway/docs/mcp.md)明确要求每轮 `X-Linx-Device-Id/Agent-Id/Session-Id/Request-Id`，其中 request_id 透传及最终 ASR 原话固定路由仍待灵矽平台支持。设备发送 `listen.start.request_id` 不等于平台已经透传；不使用“最新请求”推断，也不让模型猜目标。

完成代码／mock 不代表真实闭环通过。服务部署后仍要分别验证：TLS 配置、真实目录与推送、逐轮可信头和原话、MCP→Desktop 实际投递、Agent 自然回执、同连接多轮复用、取消排空及双 TLS／音频内存峰值。
