# LinxBox

通过协议连接电脑，以灵矽 AI 作为语音交互入口，通过 MCP 工具控制桌面应用，逐步支持 Claude Code 或其他 Code Agent 的编程开发。

## 首个 MVP

使用 AI Passport 和灵矽 AI 远程控制 macOS 上的 ChatGPT 客户端，支持发送问题、在当前会话连续追问和新建对话。用户在电脑端查看回答。

交互链路：**AI Passport → 灵矽 AI → MCP 工具 → 电脑端控制服务 → ChatGPT 客户端**。

运行范围为一台 AI Passport、一台 macOS 电脑和一个 ChatGPT 账号。电脑在线、控制服务运行，ChatGPT 客户端已登录且可操作；用户手动完成必要的系统权限授权，并在上一轮回答完成后发起下一轮提问。AI Passport 沿用当前硬件可用的语音方式，不依赖 AEC 或 realtime 模式。

本阶段聚焦语音控制链路，结果回传、回答播报、任务状态查询和异常语音反馈留待后续完善。Code Agent 接入及编程任务执行属于后续迭代。

## 工程状态

目前处于工程与开发规范初始化阶段，尚未提供可运行的电脑端服务。开发语言、连接方式和客户端控制方案将在关键链路验证后确定。

后续业务代码需同时提供可复现的安装、配置、格式检查、静态检查、构建、测试及覆盖率命令，并接入 CI。本阶段只有文档及协作模板，没有业务构建或测试命令，也未启用 CI 或合并保护。

## 模块职责

| 模块 | 职责 |
| --- | --- |
| 设备接入 | AI Passport 接入灵矽 AI；固件修改在对应固件仓库维护 |
| MCP 接入 | 定义工具契约、校验参数并连接电脑端服务 |
| 电脑端控制服务 | 接收请求、路由操作并维护当前会话 |
| 客户端适配 | 操作 ChatGPT 客户端输入、发送问题和新建对话 |

代码目录在技术方案确定后建立；客户端操作细节与 MCP 工具契约分离。

## 参与开发

- [AGENTS.md](AGENTS.md)：Claude Code、Codex 等 Agent 的工作要求。
- [CODING_STANDARDS.md](CODING_STANDARDS.md)：代码组织、输入与错误处理、测试及覆盖率要求。
- [CONTRIBUTING.md](CONTRIBUTING.md)：Issue、分支、Commit、PR 和审查流程。
- [Issue 模板](.github/ISSUE_TEMPLATE/)及 [PR 模板](.github/pull_request_template.md)：后续协作入口。

项目范围与验收标准以 [Proposal #1](https://github.com/KevinHuo/LinxBox/issues/1) 为准，初始化任务由 [Issue #2](https://github.com/KevinHuo/LinxBox/issues/2) 跟踪。初始化完成后，优先开展关键链路验证。

开发规范参考 [llcppg](https://github.com/goplus/llcppg) 与 [llgo](https://github.com/xgo-dev/llgo) 的工程实践，并按本项目范围调整。
