# LinxBox

[![Desktop coverage](https://img.shields.io/badge/Desktop%20coverage-98.35%25-brightgreen)](desktop/README.md#测试覆盖率)
[![Firmware line coverage](https://img.shields.io/badge/Firmware%20line%20coverage-99.30%25-brightgreen)](firmware/README.md#测试覆盖率)

**把 AI 编程会话带到手边。**

LinxBox 是围绕 **FoloToy AI Passport** 构建的随身 AI 编程交互项目：希望让你暂时离开键盘后，也能在小屏上查看任务、阅读 Agent 的回复，并用一句话继续沟通。设备负责交互，代码仍由 Mac 上已授权的 Code Agent 执行。

![LinxBox 硬件概念效果图：透明外壳的 AI Passport 展示 LinX 任务列表](assets/readme/linxbox-hardware.png)

> AI 生成的产品效果图，依据 AI Passport 硬件外观和 LinX 界面制作；屏幕任务、状态与电量为示例，不是实物照片或真实任务执行记录。项目处于开发与联调阶段，完整语音执行链路尚未验收。

[产品体验](#产品体验) · [硬件与操作](#硬件与操作) · [当前进展](#当前进展) · [参与开发](#参与开发)

## 产品体验

当 Agent 正在修改代码、运行测试，或者需要你补充要求时，我们希望把“查看进展、读回复、说下一步”这几个动作放到一块随手可用的小屏上，让你减少回到电脑前查看会话的次数。

| 使用场景 | 希望带来的体验 |
| --- | --- |
| 等待任务执行 | 浏览任务列表，了解正在运行、等待回复或有新回复的会话 |
| 查看本轮结果 | 分页阅读 Agent 的文字回复，翻看最近对话，接上之前的思路 |
| 想到下一步要求 | 选择当前任务，用语音补充“继续优化这个页面”等指令，延续原会话 |

完整体验的目标是：

1. **选任务**：在设备上找到电脑端正在进行的会话，进入概览。
2. **读回复**：打开“阅读本轮回复”或“最近对话”，用上下键翻页。
3. **说下一步**：回到概览，选择“继续沟通”；麦克风开启后说话，按一次 OK 结束并发送。
4. **等新结果**：由电脑上的 Agent 继续执行，再回到设备查看新回复。

这条体验涉及 **AI Passport → 灵矽 AI → Cloud MCP Gateway → macOS 客户端 → 本机 Code Agent**。目前各模块已有部分实现，服务端任务投递尚待实现，真实语音闭环待联调验收；可体验范围见[当前进展](#当前进展)。

## 硬件与操作

![LinxBox 语音交互概念效果图：工作桌面上的 AI Passport 显示录音波形和计时](assets/readme/linxbox-voice.png)

> AI 生成的场景效果图。录音界面参考 LinX 主机渲染，计时为示意；背景电脑仅表达使用场景，不代表已完成端到端联调。图片来源与生成提示词见[素材说明](assets/readme/README.md)。

当前使用 **FoloToy AI Passport** 硬件，运行本项目的 **LinX 0.2** 界面。透明外壳、顶部挂绳孔和侧边实体键沿用原设备设计。

| 项目 | 当前配置与交互 |
| --- | --- |
| 硬件平台 | ESP32-C3，8 MB Flash，无 PSRAM |
| 显示 | 240 × 320 竖屏；任务列表、文字阅读和语音状态 |
| 操作 | UP / DOWN / OK 三个实体功能键；上下键选择或翻页，短按 OK 进入或确认 |
| 返回 | 默认长按 OK；可在设备设置中改为双击，并保存偏好 |
| 语音 | 选择“继续沟通”才开启麦克风；按 OK 停录并发送，反馈播完后返回概览 |
| 配网 | 用手机连接设备临时热点，在网页中选择 2.4 GHz Wi-Fi 并保存 |

首版阅读页仅展示文字，阅读时不会开启录音；图片浏览和设备间对讲尚未提供。详细操作见[固件使用说明](firmware/README.md#灵矽配置与操作)。

## 当前进展

| 范围 | 已有能力与验证边界 |
| --- | --- |
| 硬件交互 | 已有 LinX 0.2 界面、任务概览、文字分页、最近对话和返回手势设置；默认任务、回复与电量使用演示数据 |
| 配网与语音 | 已有手机网页配网和灵矽语音入口；直接灵矽路径完成过短流程真机联调，长期稳定性仍待验证 |
| Gateway 内容接入 | 服务端已实现会话目录、历史与正文读取路由及新回复通知；设备侧接入仍在开发 |
| macOS 客户端 | 已有本地项目与会话浏览、Codex 接入及 Gateway 客户端实现；真实云端联调和持久化仍待完成 |
| 完整执行链路 | 服务端任务投递尚待实现；灵矽逐轮语音透传及真实 Gateway／Desktop 联动待完成与验收 |

语音反馈播完只表示本轮录放音结束，不能据此认定电脑已接收或任务已完成。各模块的具体实现与验收记录见[固件](firmware/README.md)、[Gateway](gateway/README.md)和[桌面客户端](desktop/README.md)；功能范围及跨模块协议以对应 Issue、设计与 PR 的评审结果为准。

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

当前硬件为 **FoloToy AI Passport**，ESP32-C3、8 MB Flash、无 PSRAM，使用 ESP-IDF 5.5.3。`firmware/hardware/folotoy/ai-passport/` 维护品牌型号接入配置；`firmware/main/` 维护 LinX 界面、导航、阅读、手机配网、联网与语音运行。语音协议部分参考固定版本的小智源码，保留来源及许可；关闭真实语音开关可恢复离线演示，尚未实现设备间对讲。来源与边界见[固件说明](firmware/README.md#上游来源与本地定制)。

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
| `desktop/` | macOS 桌面客户端：Electron UI、Gateway WSS client、Codex app-server adapter、任务与结果视图 | 开发中；语句覆盖率 98.35%，尚未接真实云端 Gateway 或持久化；安装、检查、测试、覆盖率与打包命令见 [desktop/README.md](desktop/README.md) |

## 共享协议契约

固件、Gateway、Desktop 与 MCP 的字段定义及离线校验命令见 [contracts/README.md](contracts/README.md)。Gateway 已实现只读路由及 [MCP 文字提案接收](gateway/docs/mcp.md)；固件和 Desktop 的新增消息接线、灵矽逐轮透传及完整执行链路仍待实现和联调，不代表真实语音闭环已经完成。
