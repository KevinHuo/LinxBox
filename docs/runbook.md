# LinxBox 运行说明

本文面向独立部署和使用者。示例中的域名和版本号均为占位符；真实地址、凭据和设备身份只保存在自己的受限配置中，不写入共享文档。设备身份自动登记，普通使用无需手动配置设备 ID。

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

## 编译 Gateway

从仓库根目录开始，准备 Go `1.25+` 和 Make，执行：

```sh
cd gateway
go mod download
make release
```

`make release` 自动生成共享契约资源，并以 `CGO_ENABLED=0` 交叉编译 Linux 服务，产物为 `gateway/dist/linux-amd64/linxbox-gateway` 和 `gateway/dist/linux-arm64/linxbox-gateway`（路径相对仓库根目录）。服务器 `uname -m` 为 `x86_64` 时选 amd64，为 `aarch64` 或 `arm64` 时选 arm64。服务器运行发布包不需要安装 Go。

此命令只生成服务二进制，不会生成带 `install.sh` 的压缩包。采用下方安装方式时，继续按[编译与打包说明](../gateway/docs/ws-deployment.md#编译与打包)生成完整发布包；已有对应架构发布包的使用者可直接部署。

## 部署 Gateway

生产环境使用与服务器架构匹配的 Linux 发布包，并通过 HTTPS/WSS 提供服务。以下以带安装脚本的发布包和 systemd 为例，部署流程不限定本机系统或文件传输工具。

1. **准备环境与配置**：选择对应架构的发布包，确定监听地址、可写数据库目录、各接入方的独立凭据及 TLS 或反向代理配置。真实地址和凭据只保存在自己的受限配置中。
2. **上传并解压**：将发布包传到目标服务器，核对随包校验和后解压。上传方式和存放目录按自己的环境选择。
3. **安装并启动**：在解压目录执行下方命令。安装脚本创建运行用户、安装程序并注册、启动 systemd 服务；具体配置按随包说明设置。
4. **验证服务**：检查服务状态和 `/readyz` 就绪入口，再验证 HTTPS/WSS 入口及客户端连接。就绪检查成功不代表真实设备、MCP 或 Codex 执行链路已经验收。

```sh
sudo bash install.sh
sudo systemctl status linxbox-gateway --no-pager
```

已有服务升级时，先停服并备份原程序、配置和完整数据库，再安装新版本并验证。普通更新保留已有配置、token 和数据库，程序启动时执行必要迁移。更多部署参数与排查方式见 [Gateway 部署说明](../gateway/docs/ws-deployment.md)。

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
