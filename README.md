# LinxBox

LinxBox 的目标是让 AI Passport/ESP32 通过灵矽 AI 发起本地编码任务，经 Cloud MCP Gateway 路由到 macOS 客户端，再由本机已授权的 Code Agent 执行。

当前仓库仍处于协作规范和关键链路验证阶段，没有可发布的 Gateway、客户端或固件实现。具体协议、状态机、数据结构和恢复策略必须由相关模块负责人通过独立 Issue、设计评审和 PR 确认；讨论稿不代表已经实现或验收通过。

## 当前分工

| 范围 | 负责人 |
| --- | --- |
| 项目 Leader、设备固件 | [KevinHuo](https://github.com/KevinHuo) |
| MCP Gateway | [BoleLiu](https://github.com/BoleLiu) |
| macOS 客户端 | [yydounai1234](https://github.com/yydounai1234) |

跨模块改动由开发 Issue 指定 DRI、受影响模块和必要 reviewer。未在表中列出的契约、质量或集成职责不自动归属于任何人。

## 模块边界

| 模块 | 高层职责 |
| --- | --- |
| 设备固件 | 设备交互与固件侧接入；源码在对应固件工程维护 |
| MCP Gateway | MCP 接入、云端路由及与客户端的连接 |
| macOS 客户端 | 本地项目授权、任务展示及本机 Agent 接入 |

上表只描述协作边界，不冻结接口字段或实现方案。每个模块的实现、契约和验收证据分别提交，不能用文档描述代替可运行实现或真实联调。

## 参与开发

- [AGENTS.md](AGENTS.md)：Claude Code、Codex 等 AI 助手的工作要求。
- [CODING_STANDARDS.md](CODING_STANDARDS.md)：代码组织、输入与错误处理、测试及覆盖率要求。
- [CONTRIBUTING.md](CONTRIBUTING.md)：Issue、分支、Commit、PR 和审查流程。
- [Issue 模板](.github/ISSUE_TEMPLATE/)及 [PR 模板](.github/pull_request_template.md)：协作入口。

功能范围、技术方案和跨模块契约以对应 Issue 与独立 PR 的评审结果为准。

## 代码

| 路径 | 内容 | 状态 |
| --- | --- | --- |
| `desktop/` | macOS 桌面客户端：Electron UI、Gateway WSS client、Codex app-server adapter、任务与结果视图 | 开发中，尚未接真实云端 Gateway 或持久化；安装、检查、测试、覆盖率与打包命令见 [desktop/README.md](desktop/README.md) |

