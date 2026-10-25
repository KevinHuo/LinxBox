# LinxBox 运行说明

本文面向独立部署和使用者。示例中的域名、SSH 用户和版本号均为占位符；真实地址、凭据和设备身份只保存在自己的受限配置中，不写入共享文档。设备身份自动登记，普通使用无需手动配置设备 ID。

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

启动后，在 LinxBox 首页的“灵矽连接”中填写部署方提供的 Gateway 地址，例如：

```text
wss://gateway.example.com/v1/clients/connect
```

示例域名不对应实际服务，使用时替换为自己的部署地址。填写部署方提供的 Desktop 专属 token，不能混用设备或 MCP token，也不要依赖开发环境的预填信息。保存并连接后，让设备完成自动登记，再在配对界面输入设备显示的 MAC；首次绑定要求设备在线，无需在服务器配置中预填设备身份。

Desktop 会主动连接 Gateway，Mac 不需要开放监听端口。会话目录按本地项目配置、Codex 项目或默认目录组织；“本地项目”用于配置目录，不是 WSS 任务执行前再次弹出授权的入口。已有会话沿用本机 Codex 记录的工作目录，固定会话无法续接时明确报错，不创建新会话兜底。连接配置保存在 Electron user-data 中，凭据不要写入共享说明或日志。

## 本地运行 Gateway（可选）

只有需要修改 Gateway 或离线运行时才需要启动本地实例。

环境要求：Go `1.25+`。

```sh
cd gateway
go mod download
make generate
cp config.example.json config.local.json
```

编辑 `config.local.json`，填写监听地址、可写数据库路径和独立的设备/Desktop 凭据。两端使用同一账号，token 不同；设备连接后自动登记身份，Desktop 再完成绑定，无需配置设备 ID 或 MAC 白名单。

接入 MCP 时另外配置独立凭据，并按所用版本限定设备及灵矽智能体范围。设备 WS 自动登记不等于 MCP 授权范围自动扩大；具体配置由部署方按 [MCP 接入](../gateway/docs/mcp.md)设置，不向普通使用者分发服务器配置正文。配置完成后启动：

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

## 更新已部署 Gateway（简版）

在本机终端进入发布包所在目录，将 `LINX_PACKAGE` 换成不含 `.tar.gz` 的实际包名，`LINX_SSH_TARGET` 换成自己的 SSH 用户和服务器域名。服务器 x86_64 选择 amd64，aarch64/arm64 选择 arm64。

```sh
LINX_PACKAGE=linxbox-gateway-linux-amd64-实际版本
LINX_SSH_TARGET=user@server.example.com
scp "$LINX_PACKAGE.tar.gz" "$LINX_SSH_TARGET:~/"
ssh -t "$LINX_SSH_TARGET" "tar -xzf $LINX_PACKAGE.tar.gz && cd $LINX_PACKAGE && sudo bash install.sh"
```

安装脚本替换程序并重启服务，普通更新保留配置、token 和数据库。设备 WS 的身份自动登记，无需服务器预配 MAC 白名单；MCP 的授权范围按所用版本配置。

示例用户名、域名和包名均需替换，文档不提供实际服务器、账号或凭据。生产接入使用 HTTPS/WSS。升级前保留原配置、程序和完整数据库备份；更多部署参数见 [Gateway 部署说明](../gateway/docs/ws-deployment.md)。

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

这些命令不自动烧录设备。首次安装、仅更新应用及串口查看的命令见[固件烧录说明](../firmware/README.md#烧录与串口查看)，配网和录放音操作见[固件配置说明](../firmware/README.md#灵矽配置与操作)。固件默认独立模式不启用 Gateway；使用真实会话目录时，先在 menuconfig 的“LinX 网络与语音”中启用 Gateway，并填写自己的 WSS 地址和独立设备凭据。接线及验收限制见[固件 Gateway 接入说明](../firmware/docs/gateway-client.md)，不需要预填设备 MAC。可选模式已有接线，不等于真实平台、设备和 Desktop 完整链路已验收。

## 常见问题

- 连接失败：检查 Gateway 地址是否包含 `/v1/clients/connect`，以及 Desktop token 是否正确。
- `device.bind` 失败：检查设备是否完成 hello 登记、MAC 是否正确、两端账号是否一致以及首次绑定时设备是否在线；不通过添加 MAC 白名单解决。
- `active writer`：目标 Codex thread 正被其他 Codex writer 使用，释放原会话后再试。
- `agent_sessions.sync` 被拒绝：检查是否存在空 `thread_id` 或不符合共享契约的会话记录。
- MCP 提示来源与凭据不匹配：核对智能体、账号及凭据范围，确认设备已登记并绑定到同账号；已有旧版部署的转换方式见 [MCP 兼容说明](../gateway/docs/mcp.md#旧版单设备配置兼容)。
