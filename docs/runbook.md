# LinxBox 运行说明

## 运行 Desktop

环境要求：macOS、Node.js `>= 22.12`、pnpm `11`，以及本机可运行的 Codex CLI/app-server。

```sh
cd desktop
pnpm install
pnpm dev
```

首次安装如果 Electron 未完成下载：

```sh
node node_modules/.pnpm/electron@*/node_modules/electron/install.js
pnpm dev
```

启动后，在 LinxBox 首页的“灵矽连接”中选择线上 Gateway，确认地址为：

```text
ws://124.220.162.10:8787/v1/clients/connect
```

线上测试 token 已随 Desktop 默认配置写入，通常无需手动填写；保存并连接即可。然后输入设备 MAC 完成配对，在“本地项目”中授权需要执行任务的工作目录即可使用。

Desktop 会主动连接 Gateway，Mac 不需要开放监听端口。连接配置保存在 Electron user-data 中，token 不提交到仓库。

## 本地运行 Gateway（可选）

只有需要修改 Gateway 或离线运行时才需要启动本地实例。

环境要求：Go `1.25+`。

```sh
cd gateway
go mod download
make generate
cp config.example.json config.local.json
```

编辑 `config.local.json`，填写监听地址、数据库路径、设备/Desktop/MCP 凭据和配对规则，然后启动：

```sh
go run ./cmd/linxbox-gateway --config ./config.local.json
```

再将 Desktop 的 Gateway 地址切换为本地实例的 `/v1/clients/connect`，并填入对应 Desktop token。

本地 Gateway 检查：

```sh
cd gateway
make check
make test
make smoke-local
```

## 构建和检查 Desktop

```sh
cd desktop
pnpm format:check
pnpm lint
pnpm typecheck
pnpm test
pnpm coverage
pnpm build
pnpm package
```

`pnpm package` 生成未签名、未 notarize 的 macOS 开发包。

## 构建和检查固件

固件需要 ESP-IDF `5.5.3`：

```sh
cd firmware
./tools/validate.sh --static
./tools/validate.sh
```

这些命令不自动烧录设备。固件烧录和配置见 [firmware/README.md](../firmware/README.md)。

## 常见问题

- 连接失败：检查 Gateway 地址是否包含 `/v1/clients/connect`，以及 Desktop token 是否正确。
- `device.bind` 失败：检查设备 MAC、设备在线状态和 Gateway 配对配置。
- `active writer`：目标 Codex thread 正被其他 Codex writer 使用，释放原会话后再试。
- `agent_sessions.sync` 被拒绝：检查是否存在空 `thread_id` 或不符合共享契约的会话记录。
