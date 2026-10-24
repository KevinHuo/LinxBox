# 模块与负责人

本文档定义 LinxBox 的逻辑模块、协作边界和负责人槽位。技术基线和目标目录已经冻结，但业务代码尚未 bootstrap；不要为了占位提前创建空实现。

`OWNERS.md` 只分配维护和评审责任，不改变 Proposal、开发 Issue、契约或 ADR 的优先级。

## 模块边界

| 模块 ID | 模块 | 负责范围 | 不负责 |
| --- | --- | --- | --- |
| `device` | 设备接入 | 三键任务/回复/图片 UI、草稿确认、回执、恢复和真机验收 | Codex 凭据、项目路径和风险决策 |
| `linx` | 灵矽 AI 交互 | 意图、澄清、MCP Client、Presentation Bridge、progress 和设备路由 | 用户权限、本地执行和 ID 猜测 |
| `gateway` | Cloud MCP Gateway | 入口认证、绑定、Task/Outbox、Desktop WSS、设备安全投影和审计 | 本地文件、Codex 凭据和完整 diff |
| `desktop` | Desktop UI/Daemon | 出站 WSS、项目 Registry、本地策略、任务所有权、UI 和恢复 | 公共 MCP 语义和设备身份判定 |
| `codex` | Codex app-server 适配 | app-server 进程、thread/turn、事件映射、取消和终态 | Gateway 路由、产品权限和云端存储 |
| `contracts` | 契约与领域 | MCP、Gateway/Desktop、状态机、结果 schema 和契约 fixtures | transport、UI 和具体存储实现 |
| `quality` | 集成与质量 | 端到端验收、契约测试、CI、安装配置和故障排查文档 | 代替各业务模块的单元测试和内部质量责任 |

当前主链路为：

```text
device -> linx -> gateway -> desktop -> codex
          contracts 跨边界定义 schema
          quality 验证整体链路
```

## 负责人矩阵

分配成员时，将“待分配”替换为 GitHub 用户名。每个模块至少需要一名主负责人和一名备份评审人，避免只有一人理解该模块。

负责人是具体个人，与使用 Claude Code、Codex 或其他工具无关；使用哪个助手不改变职责归属和评审责任。

| 模块 ID | 主负责人 | 备份/评审人 |
| --- | --- | --- |
| `device` | 待分配 | 待分配 |
| `linx` | 待分配 | 待分配 |
| `gateway` | 待分配 | 待分配 |
| `desktop` | 待分配 | 待分配 |
| `codex` | 待分配 | 待分配 |
| `contracts` | 待分配 | 待分配 |
| `quality` | 待分配 | 待分配 |

## 协作规则

- 每个 Task/Bug Issue 指定一个主模块，并列出所有受影响模块。
- 主负责人负责模块内部实现和单元测试；备份评审人负责知识共享和代码审查。
- 单模块内部改动由该模块负责人主导。跨模块改动在 Task 中拆清交付项，不在一个模块内复制另一模块的逻辑。
- 修改模块边界上的契约时，由边界两侧的模块负责人共同评审，并同步契约测试。
- 同一模块的公共契约文件同一时间只由该模块主负责人修改；多人或多助手并行推进时按模块分工，避免同时修改同一份契约、schema 或状态定义。
- `quality` 模块验证端到端结果，但不替代业务模块对其自有测试和故障的责任。
- 在首批代码目录落地且 GitHub 用户名确定后，再用 `CODEOWNERS` 将本矩阵转成自动评审规则。
