# Ubuntu 24.04 WS 联调部署与升级

本包实现连接后动态登记 MAC，不需要提前填写设备 MAC 或配对白名单。范围为 Gateway；
固件与 Desktop 按[三端联调说明](integration.md)接入。本页使用通用占位符，不提供实际服务器、账号、设备身份或凭据，也不代表已完成目标服务器安装或真实板卡验收。

## 上传安装包

在服务器执行 `uname -m`：`x86_64` 选 amd64，`aarch64` 选 arm64。
下列命令按 amd64 编写，arm64 替换架构。将包名换成实际发布版本，SSH 目标换成自己的用户和服务器域名，在本机发布目录执行：

```sh
LINX_PACKAGE=linxbox-gateway-linux-amd64-实际版本
LINX_SSH_TARGET=user@server.example.com
scp "$LINX_PACKAGE.tar.gz" "$LINX_SSH_TARGET:~/"
ssh "$LINX_SSH_TARGET"
```

## 安装或更新

已有实例先停止服务并备份原数据目录，再执行后面的安装命令：

```sh
sudo systemctl stop linxbox-gateway
sudo cp -a /var/lib/linxbox "/var/lib/linxbox-backup-$(date +%Y%m%d%H%M%S)"
```

在 Ubuntu 终端执行：

```sh
sudo apt update
sudo apt install -y python3 curl ca-certificates
LINX_PACKAGE=linxbox-gateway-linux-amd64-实际版本
tar -xzf "$LINX_PACKAGE.tar.gz"
cd "$LINX_PACKAGE"
sha256sum -c SHA256SUMS
sudo bash install.sh
sudo systemctl status linxbox-gateway --no-pager
curl --fail --show-error http://127.0.0.1:8787/readyz
```

安装脚本创建 linxbox 用户和可写数据目录，首次安装生成独立随机 token，监听 `0.0.0.0:8787`，
数据库为 `/var/lib/linxbox/gateway.db`。更新时停止旧服务并替换二进制，保留已有配置和数据库；
程序启动自动执行数据库迁移。不要删除旧数据库，不需要传入 MAC。

普通部署不填写设备 ID 或 MAC 白名单，设备建立连接后自动上报并登记身份。设备与 Desktop 的 token 不同，账号相同。
MCP 使用独立凭据，并按所用版本限定设备及智能体范围；设备 WS 自动登记不会自动扩大 MCP 授权，见[MCP 接入说明](mcp.md#旧版单设备配置兼容)。修改配置使用：

```sh
sudoedit /etc/linxbox/gateway.json
sudo systemctl restart linxbox-gateway
sudo journalctl -u linxbox-gateway -n 80 --no-pager
```

按自己的监听与反向代理配置设置云主机安全组和防火墙，仅开放所需入口。公网使用 HTTPS/WSS；明文 WS 仅限隔离的测试环境及专用测试凭据。

## 先运行模拟两端

仅在专用模拟实例的服务器执行，工具使用模拟设备身份，无需修改配置或填写真实设备 ID：

```sh
sudo -u linxbox /opt/linxbox/bin/linxbox-smoke \
  --config /etc/linxbox/gateway.json \
  --url ws://127.0.0.1:8787
```

期望 `SMOKE PASS: 15 项检查通过`。重复执行、重启服务后再执行均应通过。
smoke 会替换该 Desktop 的会话目录，并以相同身份连接；只在专用 mock 实例运行，
不要与真实 Desktop 同时使用同一身份。

从另一台机器验证远程就绪入口，将示例域名换成自己的 Gateway 域名：

```sh
curl --fail --show-error https://gateway.example.com/readyz
```

此检查只验证 HTTP 就绪入口，不能代替 WS 业务联调。不要把服务器配置正文、真实 token 或设备身份复制到共享文档、截图和公开日志中。

## 交给两端的信息

| 信息 | 固件端 | Desktop 端 |
| --- | --- | --- |
| WS 地址示例 | `wss://gateway.example.com/v1/devices/connect` | `wss://gateway.example.com/v1/clients/connect` |
| 专属 token | `credentials.devices[].token` | `credentials.desktops[].token` |
| 身份 | 设备自动上报，无需服务端预填 | 自己部署的 Desktop 客户端标识 |
| 认证 | `Authorization: Bearer <token>` 或子协议 | 同左，使用 Desktop 自己的 token |

子协议认证发送 `linxbox.v1, linxbox-token.<token>`，服务端只回选 `linxbox.v1`。
两端均可先连 WS；固件在连接后 10 秒内完成 hello 并上报自身 MAC。用户在 Desktop 配对界面使用设备显示的 MAC 完成绑定，不需要在服务器上配置设备 ID。
绑定后验证目录同步、分页、详情、历史、正文分块、新消息通知和断线重连；协议字段与完整帧见[联调接口](integration.md)，其中示例身份仅用于说明格式。不同账号不能认领已登记的设备。

## 查看请求日志

默认 `log.level=info`、`log.format=text`，直接阅读中文流程摘要。需要日志采集时可显式设置 `format=json`。
已有配置未填写 format 时也使用文本格式；只需更新程序并重启，无需更换 token 或数据库。

```json
"log": {"level":"info","format":"text"}
```

```sh
sudo journalctl -u linxbox-gateway -f -o cat

# 用一条日志中的短号串起收到请求、转发和最终结果，替换为实际 trace_id
sudo journalctl -u linxbox-gateway --since "10 minutes ago" --no-pager -o cat \
  | grep -F 'trace_id=c4-实际短号'
```

默认一轮同步请求保留“收到请求、业务结果”；转发查询保留“收到请求、转发 Desktop、最终结果”。
机械分发、应答已接受及发送入队细节降为 debug，同一次业务失败只在最终结果输出一条 WARN。
发送失败或应答被拒绝仍单独告警。开启 debug 时可看到完整的队列和分发记录。

| 字段 / 事件 | 排查用途 |
| --- | --- |
| `trace_id` | 基于连接代次和消息 ID 生成的检索短号，只用于日志，不参与业务路由 |
| `connection_id` | 进程内连接代次，重启后重新计数；配合 `device_id` / `client_id` 判断双方 |
| `message_id` | 当前端原始网络包 `id` 或 `message_id`，不再混称 request_id |
| `request_id` | 一轮追问的业务 ID；prepare/get/cancel 的不同包共享它 |
| `query_id` / `desktop_message_id` | Gateway 转发查询 ID、Desktop 返回该查询的应答包 ID |
| `body` / `response_body` | 请求 payload、处理结果的脱敏结构；文本展开为 body.agent_session_id 等键，JSON 保留嵌套对象 |
| `body_bytes` / `body_redacted` / `body_truncated` | 原始 payload 字节数、是否脱敏及是否截断；response_body 有同名后缀 |
| `error_code` / `error_message` | 业务错误码及可读原因；例如 NOT_FOUND + 会话不存在 |
| `error_source` / `error_hint` | 错误来自 gateway 或 desktop，以及下一步排查方向 |
| `error_detail_redacted` | 对端未知异常的原文被隐藏；已知“会话不存在”等诊断文案可直接显示，常见权限/超时/thread 缺失异常归类显示 |
| `request.state` | 当前保存的业务请求、会话 ID 和状态；get/cancel 不因入站未带会话字段而显示空目标 |
| `data_source=gateway_catalog` | 当前结果来自 Gateway 保存的会话目录，不能据此判断 Desktop 的底层历史是否可读 |
| `query.completed` | 最终业务结果与毫秒耗时，小数保留亚毫秒处理时间 |
| `end_reason` | desktop_result、timeout、desktop_disconnected 或 forward_failed，区分结束原因 |

body 会显示 MAC、会话/消息/业务请求 ID、分页 cursor 和字节偏移等诊断字段。
正文、标题、摘要、路径和凭据使用脱敏标记；未纳入诊断白名单的字段名和值不直接输出。
每个 body 预算为 4096 字节，数组最多显示前 3 项，字符串和深层结构有界，超出时带截断标记。
因此日志 body 是诊断视图，不能作为完整原始报文重放。

例如历史查询失败时，默认摘要直接说明“Desktop → Gateway：读取会话历史失败：会话不存在”，
并带实际 agent_session_id、请求 body、error_source=desktop 和排查提示；
不会再把“应答已接受”显示为另一条容易误读的 INFO 成功记录。

`query.result_received` 仅在 debug 出现，其 `accepted` 表示路由器接受应答，
业务是否成功以 `query.completed.ok` 为准。`response.enqueued` 仅表示入队，不能证明固件已收到。
未通过 Schema 的消息会记录 JSON/字段错误提示和脱敏 body，然后按既有规则丢弃。

日志字段的兼容变化：旧版网络包 `request_id` 现改称 `message_id`；`request_id` 专指业务请求。
依赖旧版 JSON 的采集程序应设置 format=json，并同步更新字段映射。

本机真实 TCP 验证使用模拟固件和 Desktop；交叉编译及校验和验证不代表 Linux 安装、公网连接或真机联调已通过。
