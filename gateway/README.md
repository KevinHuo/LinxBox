# LinxBox Gateway

LinxBox Gateway 是用 Go 编写的常驻后端服务：固件（AI Passport）、Mac Desktop 与灵矽
语音服务通过它路由会话目录、绑定、追问请求与执行事实。首版为一个 Go 服务进程、
一个监听端口、一个本地 SQLite 数据库；MCP 与 WebSocket 是同一程序的不同入口。

当前实现覆盖工程与契约、绑定与目录，以及历史 / 正文读取路由和新消息通知。
本轮优先保证三端只读联调；MCP、请求确认、调度与执行事实暂缓。本仓库 `contracts/`
仍是字段契约的唯一正文，本程序生成的 `internal/contracts/assets/` 为构建资源。

## 目录

- `cmd/linxbox-gateway`：服务入口
- `cmd/linxbox-smoke`：真实网络入口的模拟链路（固件 + Desktop）
- `internal/transport`：连接、认证、Schema 校验、发送与背压
- `internal/service`：绑定与目录业务事务
- `internal/service/query.go`：有界查询关联、超时、来源与代次校验
- `internal/domain`：MAC 规范化、身份与业务错误
- `internal/storage`：SQLite 连接、迁移与仓储
- `internal/lifecycle`：在线连接表、日志
- `internal/contracts`：嵌入共享 Schema 与 Go 类型
- `integration`：跨模块与网络集成测试
- `deploy`：systemd / Docker / compose 部署文件

## 环境要求

- Go 1.25 或更新版本（本机开发）
- 无 CGO 的 Linux 二进制用于部署（`make release`）

## 安装与启动

```sh
cd gateway
go mod download
make generate          # 从根目录 contracts/ 复制共享 Schema
cp config.example.json config.local.json
# 编辑 config.local.json：监听地址、数据库路径、凭据与配对规则
go run ./cmd/linxbox-gateway --config ./config.local.json
```

编译为独立二进制（仓库外任意工作目录可运行）：

```sh
make build
./bin/linxbox-gateway --config ./config.local.json
```

## 配置

`config.example.json` 含全部配置项与无效示例值；真实配置和凭据不入仓库。
必需项：`listen.addr`、`database.path`、`credentials.devices`、
`credentials.desktops`（至少各一个）。配对许可由 `pairing.rules` 预置；
知道设备 MAC 不等于拥有配对权。

- 开发与回环地址允许 HTTP/WS；公网由反向代理终止 TLS。如直接暴露，
  配置 `listen.tls_cert_file` / `listen.tls_key_file`。
- 凭据 token 通过子协议 `linxbox.v1, linxbox-token.<token>` 或
  `Authorization: Bearer <token>` 传递；token 不进入帧、日志或数据库。

## 验证

```sh
make check             # gofmt、go vet、测试、覆盖率（整体与关键包 ≥90%）、race
make test
make smoke-local       # 临时数据库、模拟两端；无需配置，不调用模型
make smoke             # 需要已启动的服务与 config.local.json
make release           # CGO_ENABLED=0 构建 Linux amd64/arm64
```

覆盖率门禁由 `tools/covergate` 执行；PR 增量包门禁在 CI 中以
`INCREMENTAL_BASE=origin/main make check` 运行。
race 检测与发布构建分开：发布构建不要求 CGO。

`make smoke-local` 自动启动回环 TCP 服务，以固定模拟会话验证绑定、5+1 目录分页、
会话详情、历史、中文正文连续分块、新消息通知和错误凭据拒绝。它连续运行两次，
再重启 Gateway，先确认数据库中的绑定和目录仍可读取，再跑一轮完整链路。
配置和 SQLite 数据均放在自动清理的临时目录，不需要真实设备、Desktop、Codex 或 MCP。
运行需要允许本机 TCP 监听；内存管道测试是受限环境下的补充，不能替代此入口。

最近本地结果与未验证项见[模拟联调验证记录](docs/mock-validation.md)。

## 当前接口

| 入口 | 作用 | 状态 |
| --- | --- | --- |
| `WSS /v1/devices/connect` | hello、目录分页、单条会话、历史与正文读取 | 已实现 |
| `WSS /v1/clients/connect` | hello、绑定、目录同步、query.result、新消息通知 | 已实现 |
| `GET /healthz` / `GET /readyz` | 存活与就绪探测 | 已实现 |
| `HTTPS /mcp` | 灵矽 `propose_task_reply` | 后续阶段 |
| 请求与调度 | `request.*`、`task.*` | 契约已识别，未开放 |

## 已知限制（与设计文档一致）

- 尚未实现录音准备、MCP 文字接收、确认投递、执行事实与 unknown 恢复（阶段 3/4）。
- 目录 cursor 绑定设备、Desktop 和目录版本，5 分钟过期；目录变动或 Gateway 重启后从首页重取。
- 默认每设备最多 4 条、全局最多 128 条在途历史 / 正文查询，15 秒超时；Desktop 断线明确失败，不自动重试。
- 现有 Desktop 运行代码尚未支持绑定、全量目录与 sync.result，需独立实现 PR。
- 灵矽逐轮 `X-Linx-Request-Id` 透传待平台支持；当前只能 mock 验证。
- 真固件 + 灵矽 + Gateway + Desktop 的端到端语音链路验收未完成；
  本程序的 `make smoke` 与集成测试是主机模拟证据，不能据此声称端到端可用。

三端接线、只读验收和部署步骤见 [联调说明](docs/integration.md)。本轮不改共享 Schema，不新增绕过 MCP 的文字投递接口。

新增 `TestInterop*` 使用内存管道中的真实 HTTP upgrade / WebSocket 帧及 SQLite，环境禁止监听 TCP 时可运行：

```sh
make generate
go test -race ./internal/transport -run '^TestInterop' -count=1
go test -race ./internal/service
```

这些定向用例不能替代完整 `make check`、网络部署和真机验收。容器健康检查复用程序的 `--healthcheck`，不依赖运行镜像包含 wget 或 shell。
