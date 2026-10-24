# ADR-0001: MVP 使用网关预配置的设备—客户端绑定

> Status: accepted  
> Date: 2026-10-24

## Context

端到端 MVP 需要把灵矽设备请求准确投递到指定 Desktop。产品账号、配对码和扫码会显著扩大首版范围。

## Decision

运维预配置：

```text
(installation_id?, linx_device_id) → client_id
```

Gateway 创建 `client_id` 和至少 256 bit 随机的 `client_token`。Desktop 只导入 token；Gateway 保存 token hash，并在 WSS 鉴权后反查 client ID。设备请求必须同时通过 Gateway 入口认证，设备 ID 不能单独作为身份凭据。

## Consequences

优点：无账号、配对码、扫码、入站端口和本地 MCP；最快验证真实链路。限制：换机、解绑和新增设备需要运维；不适合开放注册；共享设备不能识别旁边的具体自然人。

## Acceptance

- 两个 device ID 只路由到各自 client；
- 未绑定、禁用或缺失 device ID 的请求被拒绝；
- 错误 token 不能建立 WSS；
- 离线 Delivery 只进入对应 client Outbox；
- `message_id` 重放不重复执行；
- 日志不包含原始 token。

## Evolution

产品化账号和自助配对只替换 binding 的创建和管理方式，不改变任务协议。
