# 请求恢复与记录缺失诊断

Refs #14、#1。DRI：BoleLiu（Gateway）。本次只优化 Gateway 的错误文案、日志和排查说明；固件共享 Schema、错误码、请求状态及执行重试语义不变，不新增数据库结构。

## NOT_FOUND 的准确含义

request.get 返回 NOT_FOUND，表示当前 Gateway 数据库没有该 request_id 的记录，不能据此判断任务在其他实例、其他数据库或 Desktop 上从未执行。可能原因包括 prepare 未成功、固件恢复了旧 ID、配置切换了数据库、恢复了旧备份或连到了其他实例；单凭查不到记录不能确认其中哪一种。

超时记录不会删除：awaiting_text/awaiting_confirmation 到期后保存 failed 及原因；sending/running 在断线或重启时保存 unknown，不自动重投。不要把上述情况都当成 NOT_FOUND，也不要用设备最新请求代替明确指定的查询 ID。

错误响应沿用原有 payload 的 ok/error 结构及 NOT_FOUND 代码，message 明确提示停止轮询缺失 ID，曾确认的任务先在 Desktop 核对。固件应按错误码处理恢复分支，不依赖 message 的字面匹配。Gateway 不会自动补造请求、自动重新 prepare 或重发已经可能执行的任务。

## 日志改进

| 字段 | 含义 |
| --- | --- |
| gateway_instance_id | 每次创建 Gateway 实例生成的新编号，可关联该实例的启动、MCP 与固件日志；重启会变化 |
| database_config_id | 数据库配置路径字符串的短指纹，不打印路径；不是数据库文件或内容的唯一身份，同一路径恢复旧备份时可能不变 |
| request_lookup=missing | 当前记录查找失败 |
| data_source=gateway_database | 本次查询的数据来源，不是 Desktop 查询应答 |
| recovery_action=verify_original_task | 日志中的恢复建议：先核对原任务 |
| retry_same_request=false | 日志中的建议：停止重复查询缺失 ID，不是新协议字段 |

request.state 的 response_body 现在与实际发送的业务响应同构：成功是 ok/data，失败是 ok=false/error；失败不再记录成空 Request 快照，不输出虚假的空 status 或 agent_session_id。已知恢复错误文案不会再被误标成“Desktop 返回未分类错误”。

## 处理步骤

1. 按完整 request_id 检索 prepare 及后续确认/执行记录。没有检索到日志不等于确认未收到，仍可能是日志范围、实例或旧日志已轮转。
2. 比较 gateway_instance_id 与 database_config_id，并核对 systemd 实际启动配置、数据库路径及备份恢复记录。指纹不能取代数据库和备份核对。
3. 固件若确认从未发送过 request.confirm，可结束旧录音恢复，由用户开始新一轮 prepare。若曾确认或状态不确定，先在 Desktop 核对原任务，不能自动重放任务。
4. 固件收到明确 NOT_FOUND 后应停止旧 ID 的轮询并展示恢复入口；此次未修改固件，Gateway 的日志建议不会强制旧固件停止主动查询。

服务器检查命令：

```sh
sudo systemctl show linxbox-gateway -p ExecStart -p WorkingDirectory
sudo journalctl -u linxbox-gateway -b --no-pager -o cat | grep -F '完整的request_id'
sudo journalctl -u linxbox-gateway -b --no-pager -o cat | grep -F 'server.starting'
```

## 验证边界

回归验证缺失请求保持 NOT_FOUND、不创建记录/重投、不改变其他设备权限、超时仍返回 failed，以及日志中的真实错误结构、实例/配置标识和路径不输出。既有 Streamable HTTP、缺头联调关联、确认投递与 Desktop 事实处理仍须通过完整 Gateway 门禁。所有测试为主机模拟，真实固件对 NOT_FOUND 的 UI/轮询处理仍需单独验收。

2026-10-26：make check build release 的完整重跑通过，含格式、静态检查、全部测试、原覆盖率门禁与 race；另基于相同 coverage.out 通过增量门禁。首次完整检查有一次 smoke 包无详情 FAIL，同一源码下定向 JSON 测试事件与完整重跑均通过，未修改测试配置或门槛，首次失败原因未确定。此记录不等于真实服务器或固件已更新。
