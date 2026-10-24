# LinxBox

通过协议连接电脑，以灵矽 AI 作为语音交互入口，通过 MCP 工具控制桌面应用，逐步支持 Claude Code 或其他 Code Agent 的编程开发。

## 首个 MVP

使用 AI Passport 和灵矽 AI 远程控制 macOS 上的 ChatGPT 客户端，支持发送问题、在当前会话连续追问和新建对话。用户在电脑端查看回答。

交互链路：**AI Passport → 灵矽 AI → MCP 工具 → 电脑端控制服务 → ChatGPT 客户端**。

运行范围为一台 AI Passport、一台 macOS 电脑和一个 ChatGPT 账号。电脑在线、控制服务运行，ChatGPT 客户端已登录且可操作；用户手动完成必要的系统权限授权，并在上一轮回答完成后发起下一轮提问。AI Passport 沿用当前硬件可用的语音方式，不依赖 AEC 或 realtime 模式。

本阶段聚焦语音控制链路，结果回传、回答播报、任务状态查询和异常语音反馈留待后续完善。Code Agent 接入及编程任务执行属于后续迭代。

## 工程状态

已接入 FoloToy AI Passport 固件与 LinX 0.2 离线界面演示，提供固件构建、应用逻辑测试和电脑上的真实 LVGL 界面验证。任务、回复、图片和语音输入使用模拟数据，尚未实现设备间对讲或完整的灵矽控制链路。

云端、电脑端与客户端控制按首个 MVP 范围另行推进；固件的来源、硬件和验证入口见下文。

## 模块职责

| 模块 | 职责 |
| --- | --- |
| 设备接入 | AI Passport 接入灵矽 AI；固件源码在本仓库 `firmware/` 统一维护 |
| MCP 接入 | 定义工具契约、校验参数并连接电脑端服务 |
| 电脑端控制服务 | 接收请求、路由操作并维护当前会话 |
| 客户端适配 | 操作 ChatGPT 客户端输入、发送问题和新建对话 |

客户端操作细节与 MCP 工具契约分离。

## 代码目录与固件

| 入口 | 内容 |
| --- | --- |
| [`firmware/`](firmware/README.md) | LinX 自有固件应用、硬件接入配置、字体图片和主机测试 |

固件与云端、电脑端在同一 Git 仓库管理。固件使用 **[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)** 作为固定上游依赖，锁定提交 [`006c2279852e352a8433b2c21e5e09be2ca36f77`](https://github.com/FoloToy/ai-passport/commit/006c2279852e352a8433b2c21e5e09be2ca36f77)，采用 MIT 许可证；驱动、原有测试和构建校验工具从主仓库 `.local/dependencies/ai-passport/<提交号>/` 缓存读取，不重复复制维护。

当前硬件为 **FoloToy AI Passport**，ESP32-C3、8 MB Flash、无 PSRAM，使用 ESP-IDF 5.5.3。`firmware/hardware/folotoy/ai-passport/` 保留品牌型号的接入声明；`firmware/main/` 维护 **LinX 0.2 离线界面演示**，包含导航、阅读、字体图片及自有测试，尚未实现设备间对讲。此前讨论的小智固件只作方案参考。来源与边界见[固件说明](firmware/README.md#上游来源与本地定制)。

本仓库维护的说明与工程规范统一使用中文；代码标识、命令、必要技术名称和许可证原文保持原样。上游资料通过固定版本的中文文档链接查阅，不在本仓库保存副本。

激活 ESP-IDF 5.5.3 环境后，从 `firmware/` 运行：

```sh
cd firmware
./tools/validate.sh --static
./tools/validate.sh             # 完整门禁，包含固件构建
```

验证入口和普通 `idf.py build` 会自动准备固定上游。首次获取通常需要网络，完整且校验通过的上游缓存可离线复用，也可从已有本机 Git 仓库初始化。工具链或其他管理组件尚未下载时仍可能需要联网。

单独构建使用 `./tools/validate.sh --firmware`；真实 LVGL 主机验证使用 `./tools/validate_linx_host.sh`。SDK、上游缓存、下载组件、构建和发布包均不入 Git，配置、获取方式及产物位置见[固件构建指引](firmware/README.md#构建与验证)。构建命令不烧录设备。

## 参与开发

- [AGENTS.md](AGENTS.md)：Claude Code、Codex 等 Agent 的工作要求。
- [CODING_STANDARDS.md](CODING_STANDARDS.md)：代码组织、输入与错误处理、测试及覆盖率要求。
- [CONTRIBUTING.md](CONTRIBUTING.md)：Issue、分支、Commit、PR 和审查流程。
- [Issue 模板](.github/ISSUE_TEMPLATE/)及 [PR 模板](.github/pull_request_template.md)：后续协作入口。

项目范围与验收标准以 [Proposal #1](https://github.com/KevinHuo/LinxBox/issues/1) 为准，初始化任务由 [Issue #2](https://github.com/KevinHuo/LinxBox/issues/2) 跟踪。初始化完成后，优先开展关键链路验证。

开发规范参考 [llcppg](https://github.com/goplus/llcppg) 与 [llgo](https://github.com/xgo-dev/llgo) 的工程实践，并按本项目范围调整。
