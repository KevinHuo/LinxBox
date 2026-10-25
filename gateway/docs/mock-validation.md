# Gateway 模拟联调验证记录

验证日期：2026-10-25。关联任务：Refs #14。DRI：BoleLiu。
本记录对应只读版本 `7609c4b`；后续审查修复与当前检查结果见[审查修复验证](review-fixes.md)。
主模块：Gateway；接口使用方：Desktop、固件；跨端评审：yydounai1234、KevinHuo。
本次未修改 Desktop、固件或共享 Schema。

## 范围与验收

使用固定模拟会话、模拟固件与模拟 Desktop，通过真实回环 HTTP / WebSocket 服务和
临时 SQLite 数据库验证只读接口。MCP、语音、真实 Codex、请求确认和任务执行不在范围内。

验收入口（从 `gateway/` 执行）：

```sh
make smoke-local
INCREMENTAL_BASE=origin/main make check
make build release
```

`smoke-local` 无需配置文件或已有服务；不会读取真实会话或调用模型。
它执行首次连接、同一数据库重复连接、Gateway 重启后三轮模拟，每轮 15 项检查。
重启后在重新同步之前验证绑定和会话目录仍保存在数据库中。

## 实际结果

| 检查 | 结果 |
| --- | --- |
| gofmt、go vet | 通过 |
| 全部单元和集成测试 | 通过 |
| 全部包 race 检测 | 通过 |
| HTTP 存活 / 就绪检查、WebSocket 认证 | 本机 TCP 模拟通过 |
| 设备绑定、绑定快照、目录更新通知 | 本机 TCP 模拟通过 |
| 目录 5+1 分页、会话详情、未知会话错误 | 本机 TCP 模拟通过 |
| 历史、中文 / emoji 正文连续分块和结束偏移 | 本机 TCP 模拟通过 |
| 新消息通知的会话和消息标识 | 本机 TCP 模拟通过 |
| 重复执行、重启后绑定和目录恢复 | 本机 TCP 模拟通过 |
| 离线、查询中断、超时、错误应答、旧连接和迟到结果 | 自动化测试通过 |
| 网关故障应答、畸形数据、转发参数篡改的 smoke 误报保护 | 自动化测试通过 |
| 本机构建、Linux amd64 / arm64 无 CGO 交叉构建 | 通过 |

覆盖率使用全部自有 Go 代码的语句覆盖统计，包含未命中源码；跨测试二进制的重复块取并集，
没有下调 90% 门槛或增加排除项。服务和模拟命令均加入关键包检查。

| 统计范围 | 覆盖率 |
| --- | ---: |
| 整体 | 92.82% |
| cmd/linxbox-gateway | 92.31% |
| cmd/linxbox-smoke | 92.54% |
| internal/config | 99.19% |
| internal/contracts | 97.22% |
| internal/domain | 100.00% |
| internal/lifecycle | 95.12% |
| internal/service | 94.17% |
| internal/storage | 91.53% |
| internal/transport | 92.40% |

本次 Go 缓存放在临时目录，完整网络测试在允许本机端口监听的环境执行。
本记录为本地验证证据；提交后的 Git 增量包检查和远端 CI 状态以对应 PR 的实际运行结果为准。

## 限制与提交说明

- 真实 Desktop 和固件尚未完成 Gateway 接线；本记录不能作为真实三端验收证据。
- Docker 不在当前环境中，未执行容器构建和运行；未验证目标 Linux 主机、公网 TLS 或板卡。
- 未执行部署或烧录；创建 PR 不代表已提供公网联调实例。
- 建议提交标题：`feat(gateway): 支持只读接口路由与模拟联调验收`。
- 提交或 PR 说明应引用本记录与[接线清单](integration.md)，保留上述未验证项。
