# 工程设计规范

> Status: active  
> Version: 1.0  
> Last updated: 2026-10-24

## 1. 分层

分层方向见 [`docs/architecture.md`](../architecture.md) §4.1，目录落位见 [`docs/README.md`](../README.md)；本节只补充实现纪律。

1. Domain 是纯业务代码，不依赖 HTTP、MCP SDK、WebSocket、Electron、文件系统、子进程或 Codex。
2. Application 编排用例并依赖 ports。
3. Inbound Adapter 负责 DTO、认证上下文、运行时校验和错误映射。
4. Outbound Adapter 实现存储、Broker、Codex、日志和通知端口。
5. 组合根是唯一组装具体实现的位置。

## 2. 契约

- 外部输入使用 Zod/JSON Schema 严格校验；安全请求拒绝未知字段。
- 领域 ID 使用不同值对象，禁止混用 device/client/project/task/thread ID。
- 状态转换集中管理，禁止任意赋值。
- 公共 JSON 使用 `snake_case`，内部 TypeScript 使用 `camelCase`。
- 时间使用 UTC ISO 8601；持续时间字段带单位。
- 公共协议、持久化和 prompt 均有版本。

## 3. 错误

`PublicError` 结构与标准 code 集合见 [`docs/contracts/domain.md`](../contracts/domain.md) §8 与 [`docs/contracts/mcp.md`](../contracts/mcp.md) §10，不在此复述。禁止向公网返回原始异常、绝对路径、命令行或环境变量。

## 4. 任务与并发

- 写请求先持久化再确认。
- 队列原子 claim；同一任务不能被两个 worker 执行。
- 取消是状态转换，不只是进程信号。
- 重试必须定义幂等范围、最大次数、退避和最终失败。
- 终态清理计时器、监听器、连接和子进程。
- ACK、started、completed 使用独立事件。

## 5. 安全编码

- 子进程 `shell: false`，参数数组，环境 allowlist，输出上限，超时和进程树清理。
- 项目路径从本地 Registry 解析并 `realpath`，防止符号链接逃逸。
- token 使用高熵随机值、常量时间比较、hash 存储、轮换和撤销。
- 日志结构化且不记录 Authorization、cookie、prompt 全文、代码或绝对路径。
- 公网端点设置请求体、并发、速率和超时上限。
- 每个安全控制必须有拒绝路径测试。

## 6. Adapter

每个 Agent Adapter 必须声明和测试：可用性、start/resume/interrupt、结构化事件、超时、退出码、输出上限、脱敏、session 所有权和进程树终止。领域层的 cancel 请求由 adapter 映射为 provider 支持的 interrupt 操作。

app-server adapter 必须维护 `task_id/thread_id/turn_id/project_id` 映射，使用公开协议事件判断终态，区分用户可见输出、执行元数据和内部推理，并实现版本探测、握手超时、崩溃清理和协议不兼容的 fail-closed 路径。

## 7. 测试

1. Domain 单元测试：状态、授权、幂等、风险。
2. Port Contract Test：存储、Broker 和 Agent Adapter。
3. Integration：真实 MCP Transport、WSS、临时数据库和 fake process。
4. Security：路径逃逸、越权、重复、脱敏、请求限制。
5. E2E：真实 ESP32/灵矽 → Gateway → Desktop → app-server → 专用测试仓库。

完成定义：契约和失败语义已记录；成功、拒绝、失败、重复和恢复有测试；真实目标 Profile 验收；文档和迁移同步；日志足以定位问题且不泄密。

## 8. 变更纪律

- 协议变更先写 schema 和兼容测试。
- 持久化格式必须有 `schema_version` 和向前迁移。
- 不在一个变更中同时做协议迁移、存储迁移、provider 替换和大重构。
- 身份、权限、状态机、Codex 集成或部署 Profile 变化必须新增 ADR。
- Mock 通过不代表真实 Linx、ESP32、WSS、Electron 或 app-server 可用。
