# MVP 全部接口流程与确认执行

Refs #14、#1。本任务沿用 #14 的共享契约及状态，不新增线上字段、状态查询接口、审批或执行中断入口。Gateway 责任边界沿用项目分工；受影响模块为 Gateway 与 Desktop，跨端评审需覆盖固件和 Desktop。

## MVP 目标流程

以下图为最终目标。固件 Gateway 接线、灵矽最终 ASR 固定路由和逐轮 request_id 透传仍需分别实现与真实验收，不能用 Gateway 模拟测试代替。

### 建连、绑定、目录

```mermaid
sequenceDiagram
    participant F as 固件
    participant G as Gateway
    participant D as Desktop
    participant C as Code Agent
    F->>G: WSS /v1/devices/connect
    F->>G: device.hello（mac_address）
    G-->>F: response（Binding）
    D->>G: WSS /v1/clients/connect
    D->>G: client.hello
    D->>G: client.heartbeat（周期）
    D->>G: device.bind（mac_address）
    G->>G: 验证同账号、设备在线，保存绑定
    G-->>D: binding.result
    G-->>F: device.bound
    D->>C: 枚举可共享的已有会话
    C-->>D: thread、项目、标题、状态
    D->>G: agent_sessions.sync（完整目录）
    G->>G: 原子保存目录
    G-->>D: agent_sessions.sync.result
    G-->>F: agent_sessions.updated
    F->>G: agent_sessions.list（cursor 可选）
    G-->>F: response（最多 5 项、next_cursor）
    F->>G: agent_session.get（agent_session_id）
    G-->>F: response（会话详情）
```

同一 Desktop 等目录保存应答后再发下一份；两端通知的到达顺序不是协议保证。设备 10 秒内 hello；WebSocket ping/pong 与应用 heartbeat 分开。固件的灵矽设备接入服务及其六位激活码，不等于 LinxBox Gateway 或 Desktop MAC 绑定。

### 准备、语音原文、确认、执行

```mermaid
sequenceDiagram
    participant F as 固件
    participant V as 灵矽
    participant G as Gateway
    participant D as Desktop
    participant C as Code Agent
    F->>G: request.prepare（request_id、agent_session_id）
    G->>G: 固定绑定 Desktop、thread、project、标题
    G-->>F: response（awaiting_text）
    F->>V: listen.start（本轮 request_id）
    F->>V: 二进制 PCM
    F->>V: listen.stop
    V->>G: MCP propose_task_reply（最终 ASR 原文 text）
    Note over V,G: 来源头携带设备、智能体、语音会话及 request_id
    G->>G: 原样保存文字，校验来源、串轮和过期
    G-->>V: MCP 业务结果
    G-->>F: request.proposed（awaiting_confirmation）
    F->>F: 显示原文与固定目标
    alt 用户放弃
        F->>G: request.cancel
        G-->>F: response（cancelled）
        G-->>F: request.updated（cancelled）
    else 用户确认
        F->>G: request.confirm
        G->>G: 事务保存确认、task_id、投递身份及 sending
        G->>D: task.dispatch（固定 thread 的 continuation）
        G-->>F: response（当前 Request）
        G-->>F: request.updated（sending 或 unknown）
        D->>D: 去重并校验固定目标
        D-->>G: task.ack（accepted、duplicate、rejected 或 expired）
        alt 明确拒绝或过期且未启动
            G-->>F: request.updated（failed）
        else 接受投递
            D->>C: thread/resume、turn/start
            D-->>G: task.started（thread_id、turn_id）
            G-->>F: request.updated（running）
            loop 执行过程
                C-->>D: commentary、最终文字
                D-->>G: task.progress
            end
            alt 明确成功，TaskResult.status=succeeded
                D-->>G: task.completed（TaskResult）
                G-->>F: request.updated（completed）
                D-->>G: message.available
                G-->>F: message.available
            else 明确失败
                D-->>G: task.failed
                G-->>F: request.updated（failed）
            else 结果不确定
                D-->>G: task.interrupted
                G-->>F: request.updated（unknown）
            end
        end
    end
```

图中 ACK 与 started 的展示顺序不是实现约束：Desktop 可能先发送 started，启动失败时也可能先发送 failed 再返回 rejected ACK。Gateway 都须处理，ACK 不推进 running。MCP 与 TTS 也不保证相对到达顺序，语音播完不能证明任务成功。

### 历史、正文和恢复

```mermaid
sequenceDiagram
    participant F as 固件
    participant G as Gateway
    participant D as Desktop
    F->>G: messages.list（包 id=f1）
    G->>D: messages.list（包 message_id=g1）
    D-->>G: query.result（reply_to_message_id=g1）
    G-->>F: response（reply_to=f1，最多 8 轮、next_cursor）
    loop 正文尚未读完
        F->>G: message.content.get（会话、消息、offset）
        G->>D: message.content.get
        D-->>G: query.result（text、next_offset）
        G-->>F: response（最多 1024 UTF-8 字节）
    end
    Note over G,D: Desktop 断线或 Gateway 重启，执行结果可能不确定
    G->>G: 保存 unknown，保留任务身份，不自动重投
    F->>G: 重连后 device.hello
    G-->>F: response（Binding）
    F->>G: request.get（原 request_id）
    G-->>F: response（持久化快照）
    D->>G: 重连后 client.hello、目录同步
    D-->>G: 可核验的原任务事件
    G->>G: 校验来源、task、thread、turn、事件身份及序号
    G-->>F: request.updated（可核验的新状态）
```

不存在 `task.status.get`，也不通过重新执行查询结果。未收到可核验事实时保持 unknown。历史最新到最旧并保留轮次；offset 为 UTF-8 字节偏移，正文块不是屏幕页码。

### Request 状态

```mermaid
stateDiagram-v2
    [*] --> awaiting_text: request.prepare
    awaiting_text --> awaiting_confirmation: MCP 保存原文
    awaiting_text --> failed: 120 秒超时
    awaiting_text --> cancelled: request.cancel
    awaiting_confirmation --> sending: request.confirm 落盘
    awaiting_confirmation --> failed: 300 秒超时
    awaiting_confirmation --> cancelled: request.cancel
    sending --> sending: ACK accepted / duplicate
    sending --> running: task.started
    sending --> failed: 明确拒绝、过期或启动失败
    sending --> unknown: 发送不确定、断线或重启
    running --> completed: task.completed 且结果 succeeded
    running --> failed: task.failed
    running --> unknown: 中断、断线或重启
    running --> cancelled: 可核验 task.cancelled
    unknown --> running: 可核验原任务 started
    unknown --> completed: 可核验原任务成功结果
    unknown --> failed: 可核验原任务失败结果
    unknown --> cancelled: 可核验原任务取消结果
```

completed、failed、cancelled 不回退。unknown 若来自真实 interrupted 终态，保留该任务的终态屏障；若来自连接中断，则可由原任务后续事实收敛。缺少一致 TaskResult 的 completed 保存为 unknown，不伪造成功。

### 全部共享消息索引

共 37 项，按链路和方向计数；同名跨两条链路的消息分别计数。定义见根目录 contracts/，本表不建立第二份字段契约。

| 方向 | 消息 |
| --- | --- |
| 固件 → Gateway（9） | `device.hello`、`agent_sessions.list`、`agent_session.get`、`messages.list`、`message.content.get`、`request.prepare`、`request.confirm`、`request.cancel`、`request.get` |
| Gateway → 固件（6） | `response`、`device.bound`、`agent_sessions.updated`、`request.proposed`、`request.updated`、`message.available` |
| Desktop → Gateway（14） | `client.hello`、`client.heartbeat`、`device.bind`、`agent_sessions.sync`、`query.result`、`message.available`、`task.ack`、`task.started`、`task.progress`、`task.completed`、`task.failed`、`task.interrupted`、`task.cancelled`、`task.approval_required` |
| Gateway → Desktop（8） | `binding.result`、`agent_sessions.sync.result`、`messages.list`、`message.content.get`、`task.dispatch`、`project.upsert`、`project.revoke`、`task.cancel` |

`project.upsert/revoke` 保留项目元数据兼容，不授予本机路径权限。首版不开放 `task.cancel` 执行中断或 Agent 审批入口；识别 `task.approval_required` 时提示电脑端处理，不自动批准，识别可核验 `task.cancelled` 时记录真实终态。

语音通道另有双向 hello、listen.start/stop、上行 PCM、stt、tts.start/stop、下行 Opus/PCM、abort。语音 abort、确认前 request.cancel、执行中 task.cancel 分属不同层次。MCP 工具只有 propose_task_reply，工具参数只有 text，其他上下文来自认证来源头。

## 本次实现与限制

- Gateway 在发送前事务保存确认及 request/task/dispatch 映射；重复确认只返回原快照。Desktop 离线且尚未确认时返回依赖不可用，保留待确认；落盘后发送失败保存 unknown，不自动重发。
- ACK 配对原 dispatch 的 message_id、sequence、task_id；started 才进入 running。执行事件核对当前认证 Desktop、固定 thread/turn、event_id 指纹和任务内序号；重复无副作用，串线、乱序、冲突及终态回退被拒绝。
- 追加迁移 004，保留旧迁移；Gateway 启动恢复 sending/running 为 unknown。目录移除不删除在途映射；新确认仍校验原目标存在且未改路由，旧账号不能查询或接收原请求通知。
- Desktop 仅在 provider 明确完成且无错误时生成本轮追问的 TaskResult。changes/verification 暂为空数组，risk 明确说明未汇总执行详情；不声称没有改动、测试已通过或整个工程已验收。
- Desktop 完整执行事实投影、持久去重与跨进程结果补报仍需后续工作；Gateway 本次不会因重复确认、断线或重启重新投递任务。没有自动重投或事件重放机制。
- 固件业务接线、灵矽固定路由／逐轮 ID、Desktop 目录同步应答等待和按轮次历史分页不在本次确认执行改动中。完整 MVP 必须分别补齐并提供真实链路证据。

## 检查入口

从 gateway/ 执行 `make check`、`make build`、`make smoke-local`；定向用例为 `go test -race ./internal/service ./internal/transport -run 'TestConfirm|TestExecution' -count=1`。这些测试使用模拟 Desktop／执行事件，不连接真实模型、固件或灵矽。

从 desktop/ 执行 `pnpm typecheck`、`pnpm test`、`pnpm coverage`，本次结果回归位于 daemon/gateway-client/confirmation-result.test.ts。实际结果另行记录，命令清单不表示已经通过。

## 本次验证记录（2026-10-25）

基线为 df045c82c1f8a1e7ebc1e0513ebe7c97ee41a270，在隔离工作区验证，避免与其他任务的 MCP 日志改动混合。

- Gateway：`make check build` 通过，包含格式、go vet、全部单元／集成测试、覆盖率门禁和 race。整体语句覆盖率 92.64%（2455/2650）；service 91.36%、storage 92.22%、transport 92.82%，所有关键包保持原有 90% 门槛。
- 回归覆盖并发重复确认、事务失败不发送、固定目标、ACK 晚到与冲突、事件去重／乱序／串线、成功／失败／中断、连接代次、真实关闭并重开 SQLite 后恢复，以及 WebSocket 确认→dispatch→执行事件→固件快照。
- Desktop：改动文件格式与 lint、类型检查、构建通过；新增 2 项结果对接测试通过。
- Desktop 全量测试未通过：当前 588 通过、12 失败、3 跳过；独立导出的原提交在相同依赖与回环网络条件下为 586 通过、相同的 12 项失败、3 跳过。失败名称集合完全一致，未删除断言或修改既有测试来放行。既有问题涉及授权／续接／取消、目录标识、provider 错误及 mock 控制台；因此不能声明 Desktop 全量检查或覆盖率门禁通过。
- 未连接真实灵矽、设备或模型执行任务，未烧录、部署或进行真实三端语音验收。

## MCP 日志合入与服务器包（2026-10-25）

当前工作区已合入日志提交 41e89f8aa372f0e2b7af7489ad5d8ceadf0e7b0b 的实现与回归测试，保留本次确认投递、执行事件和 unknown 恢复。合并后重新执行 `make check build release`，格式、go vet、全部单元／集成测试、race 与原覆盖率门禁通过；整体覆盖率 92.89%（2613/2813），service 91.36%、storage 92.22%、transport 93.48%。

Linux amd64／arm64 的 Gateway 和 smoke 工具均使用 CGO_ENABLED=0 静态交叉编译；部署包包含 systemd、安装脚本、配置示例、日志查看说明、源码指纹及 SHA-256 校验和。默认 info 文本日志可查看 MCP 调用阶段、通知入队／离线／失败及任务事件。包内不包含私人配置、真实凭据或数据库。

新装默认配置 WS 凭据，接入 MCP 仍须配置独立 MCP 凭据与真实设备／智能体标识；已有部署保留原配置，若其日志级别为 warn/error，应调整为 info 才能看到正常流程日志。尚未在目标 Linux 主机执行安装、部署或真实语音联调。包身份包含未提交源码指纹，不能只用基线提交号代替二进制 SHA-256。
