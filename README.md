# LinxBox

[![Desktop coverage](https://img.shields.io/badge/Desktop%20coverage-97.54%25-brightgreen)](desktop/README.md#测试覆盖率)

LinxBox 的目标是让 AI Passport/ESP32 通过灵矽 AI 发起本地编码任务，经 Cloud MCP Gateway 路由到 macOS 客户端，再由本机已授权的 Code Agent 执行。

当前仓库已提供 FoloToy AI Passport 上的 LinX 0.2 离线演示及主机侧验证；任务、回复和语音转写使用模拟数据，真机验收仍待完成。Gateway、macOS 客户端和端到端联网固件尚未形成可发布的完整实现。具体协议、状态机、数据结构和恢复策略必须由相关模块负责人通过独立 Issue、设计评审和 PR 确认；讨论稿不代表已经实现或验收通过。

## 当前分工

| 范围                  | 负责人                                          |
| --------------------- | ----------------------------------------------- |
| 项目 Leader、设备固件 | [KevinHuo](https://github.com/KevinHuo)         |
| MCP Gateway           | [BoleLiu](https://github.com/BoleLiu)           |
| macOS 客户端          | [yydounai1234](https://github.com/yydounai1234) |

跨模块改动由开发 Issue 指定 DRI、受影响模块和必要 reviewer。未在表中列出的契约、质量或集成职责不自动归属于任何人。

## 模块边界

| 模块         | 高层职责                                            |
| ------------ | --------------------------------------------------- |
| 设备固件     | 设备交互与固件侧接入；源码在本仓库 `firmware/` 维护 |
| MCP Gateway  | MCP 接入、云端路由及与客户端的连接                  |
| macOS 客户端 | 本地项目授权、任务展示及本机 Agent 接入             |

上表只描述协作边界，不冻结接口字段或实现方案。每个模块的实现、契约和验收证据分别提交，不能用文档描述代替可运行实现或真实联调。

## 代码目录与固件

| 入口                              | 内容                                                |
| --------------------------------- | --------------------------------------------------- |
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

- [AGENTS.md](AGENTS.md)：Claude Code、Codex 等 AI 助手的工作要求。
- [CODING_STANDARDS.md](CODING_STANDARDS.md)：代码组织、输入与错误处理、测试及覆盖率要求。
- [CONTRIBUTING.md](CONTRIBUTING.md)：Issue、分支、Commit、PR 和审查流程。
- [Issue 模板](.github/ISSUE_TEMPLATE/)及 [PR 模板](.github/pull_request_template.md)：协作入口。

功能范围、技术方案和跨模块契约以对应 Issue 与独立 PR 的评审结果为准。

## 代码

| 路径       | 内容                                                                                        | 状态                                                                                                                                    |
| ---------- | ------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `desktop/` | macOS 桌面客户端：Electron UI、Gateway WSS client、Codex app-server adapter、任务与结果视图 | 开发中；语句覆盖率 97.54%，尚未接真实云端 Gateway 或持久化；安装、检查、测试、覆盖率与打包命令见 [desktop/README.md](desktop/README.md) |
