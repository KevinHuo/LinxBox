# Ubuntu 24.04 WS 联调部署与升级

本包实现连接后动态登记 MAC，不需要提前填写设备 MAC 或配对白名单。范围为 Gateway；
固件与 Desktop 按[三端联调说明](integration.md)接入。未进行腾讯云安装或真实板卡验收。

## 上传安装包

在服务器执行 `uname -m`：`x86_64` 选 amd64，`aarch64` 选 arm64。
下列命令按 amd64 编写，arm64 替换包名和目录中的架构。
在 Mac 终端进入发布目录执行：

```sh
scp linxbox-gateway-linux-amd64-dynamic.tar.gz ubuntu@124.220.162.10:~/
ssh ubuntu@124.220.162.10
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
tar -xzf linxbox-gateway-linux-amd64-dynamic.tar.gz
cd linxbox-gateway-linux-amd64-dynamic
sha256sum -c SHA256SUMS
sudo bash install.sh
sudo systemctl status linxbox-gateway --no-pager
curl --fail --show-error http://127.0.0.1:8787/readyz
```

安装脚本创建 linxbox 用户和可写数据目录，首次安装生成独立随机 token，监听 `0.0.0.0:8787`，
数据库为 `/var/lib/linxbox/gateway.db`。更新时停止旧服务并替换二进制，保留已有配置和数据库；
程序启动自动执行数据库迁移。不要删除旧数据库，不需要传入 MAC。

已有配置中的旧 `device_id` 和 `pairing.rules` 可以保留，不再限制新设备 hello 上报的 MAC。
设备与 Desktop 的 token 需要不同，`subject` 需要相同。修改配置使用：

```sh
sudoedit /etc/linxbox/gateway.json
sudo systemctl restart linxbox-gateway
sudo journalctl -u linxbox-gateway -n 80 --no-pager
```

腾讯云安全组及已启用的 Ubuntu 防火墙须允许联调设备来源访问 TCP 8787。
WS 不加密，当前只使用专用联调凭据与模拟数据；生产使用 WSS。

## 先运行模拟两端

在服务器执行，模拟 MAC 可自行指定，无需修改配置：

```sh
sudo -u linxbox /opt/linxbox/bin/linxbox-smoke \
  --config /etc/linxbox/gateway.json \
  --url ws://127.0.0.1:8787 \
  --device-mac 34:00:00:00:00:77
```

期望 `SMOKE PASS: 15 项检查通过`。重复执行、重启服务后再执行均应通过。
smoke 会替换该 Desktop 的会话目录，并以相同身份连接；只在专用 mock 实例运行，
不要与真实 Desktop 同时使用同一身份。

从 Mac 验证公网：

```sh
curl --fail --show-error http://124.220.162.10:8787/readyz
umask 077
ssh ubuntu@124.220.162.10 'sudo cat /etc/linxbox/gateway.json' > server-smoke.local.json
./linxbox-smoke-macos-arm64 \
  --config ./server-smoke.local.json \
  --url ws://124.220.162.10:8787 \
  --device-mac 34:00:00:00:00:77
```

若 sudo 需要交互密码，在服务器终端查看配置后，将两端凭据填入本机权限 600 的配置，
不要将凭据提交 Git。公网 smoke 通过才验证了 WS 业务入口，HTTP 探测只验证就绪接口。

## 交给两端的信息

| 信息 | 固件端 | Desktop 端 |
| --- | --- | --- |
| WS 地址 | `ws://124.220.162.10:8787/v1/devices/connect` | `ws://124.220.162.10:8787/v1/clients/connect` |
| 专属 token | `credentials.devices[].token` | `credentials.desktops[].token` |
| 身份 | 固件自己的真实 MAC | 配置中的 `client_id` |
| 认证 | `Authorization: Bearer <token>` 或子协议 | 同左，使用 Desktop 自己的 token |

子协议认证发送 `linxbox.v1, linxbox-token.<token>`，服务端只回选 `linxbox.v1`。
两端均可先连 WS；固件在连接后 10 秒内发送：

```json
{"id":"hello-1","type":"device.hello","payload":{"mac_address":"34:00:00:00:00:01"}}
```

成功响应返回规范 MAC，未绑定时 `desktop_client_id=null`。固件把该 MAC 展示或通过产品配对流程交给 Desktop；
Gateway 不自动将所有设备的 MAC 广播给 Desktop，也不推导硬件 MAC。
Desktop 获得 MAC 后，按自己的帧结构发送：

```json
{"protocol_version":"1.0","message_id":"bind-1","sequence":1,"type":"device.bind","sent_at":"2026-10-25T10:00:00Z","payload":{"mac_address":"34:00:00:00:00:01"}}
```

`sent_at` 使用发送时的实际 RFC 3339 时间。绑定成功后验证目录同步、分页、详情、历史、正文分块、
新消息通知和断线重连；完整帧与应答见[联调接口](integration.md)。不同账号不能认领已登记的 MAC。

## 查看请求日志

默认 `log.level=info` 即可查看连接、认证、hello 登记、绑定、目录同步和业务请求日志。
心跳使用 debug 级别，避免淹没联调信息。日志为单行 JSON，不记录 token、HTTP 认证头、完整帧、
会话标题、正文或对端错误文案；Schema 校验失败也不输出原始数据。

```sh
# 实时查看
sudo journalctl -u linxbox-gateway -f -o cat

# 查找固件某个原始请求 ID，替换 hello-1 为实际帧 id
sudo journalctl -u linxbox-gateway --since "10 minutes ago" --no-pager -o cat \
  | grep -F '"request_id":"hello-1"'
```

| 字段 / 事件 | 排查用途 |
| --- | --- |
| `connection_id` | 进程内连接代次，重启后重新计数；配合 `device_id` / `client_id` 判断哪条连接 |
| `request_id` | 当前端原始帧 `id` 或 `message_id`；响应日志回指触发请求 |
| `query_id` | Gateway 转发查询的包 ID，关联固件原请求、Desktop 查询和 query.result |
| `request.received` / `request.dispatched` | 请求已通过 Schema 并分发；`duration_ms` 是当前处理器耗时，异步查询的总耗时见 query.completed |
| `response.enqueued` / `response.send_failed` | 响应已进入有界发送队列或入队失败；入队不代表对端已收到 |
| `device.registered` / `device.binding` | MAC 登记与绑定结果，失败时带 `error_code` |
| `catalog.synced` | 同步结果、目录条数和版本，不输出标题或内容 |
| `query.forwarded` / `query.result_received` / `query.completed` | 已转发、收到结果、查询结束；最终 `ok`、`error_code` 和 `duration_ms` 在 query.completed |
| `end_reason` | `desktop_result`、`timeout`、`desktop_disconnected` 或 `forward_failed`，区分查询结束原因 |
| `device.hello_timeout` / `connection.replaced` / `connection.closed` | hello 超时、重连替换、断线；替换记录前后连接代次，断线只记录关闭码 |

按 `query_id` 再过滤一次，即可把固件请求和 Desktop 应答串起来。先看 `ok` 和 `error_code`，
再看 `end_reason`；例如 `DEPENDENCY_UNAVAILABLE + timeout` 表示 Desktop 查询未在期限内完成。
`query.result_received` 的 `ok` 表示该应答是否被路由器接受，业务结果以 `query.completed.ok` 为准。

本机真实 TCP 验证使用模拟固件和 Desktop；交叉编译及校验和验证不代表 Linux 安装、公网连接或真机联调已通过。
