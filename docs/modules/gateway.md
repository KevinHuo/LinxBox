# `gateway`：Cloud MCP Gateway

> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24  
> Owner: `OWNERS.md` 中的 `gateway`

## 子模块

| 子模块 | 责任与权威数据 |
| --- | --- |
| `mcp-transport` | MCP 认证、DTO、progress 和工具结果；不拥有业务状态 |
| `identity` | token hash、device-client binding、禁用和轮换 |
| `projects` | 绑定范围内的项目别名和 availability 投影 |
| `tasks` | Task、Turn lineage、TaskEvent 和幂等写入 |
| `presentation` | 任务卡片、回复页、图片描述和回执的脱敏设备投影 |
| `broker` | Desktop WSS presence、sequence、ACK 和重放 |
| `outbox` | Task/Presentation Delivery 的事务投递事实 |
| `policy` | 身份、绑定、能力、内容分级和风险预检 |
| `audit` | 不含代码、路径、正文和凭据的脱敏审计 |

## 规则

- 写操作以已验证身份和 idempotency key 去重；Task 与首个 Delivery 同事务提交；
- Device 不能提交 client ID、本地路径或 provider ID；续接只携带父 Task ID；
- Presentation 只包含 Desktop 明确生成的 device-safe 内容：标题、短摘要、分页正文、受限图片衍生物和回执；
- 完整 diff、原始 stdout/stderr、内部推理、绝对路径、token 和未脱敏源码不得进入云端；
- 内容保存期限、最多八轮设备历史、页数/字节上限和删除必须由 schema/配置固定，不能无限增长；
- ACK、receipt、turn terminal 和 task terminal 是不同事实。

## 依赖与门禁

依赖 contracts、PostgreSQL 和 WSS transport，不依赖本地文件系统或 Codex 凭据。身份隔离、幂等、Outbox、ACK/重放、乱序、限流、内容上限、删除和脱敏必须有单元、契约及真实存储集成测试。
