# LinxBox 固件

可选 Gateway 模式按正式共享契约读取会话、历史、正文和请求状态，按通知刷新。录音前准备固定目标，录音和反馈由同一灵矽会话处理：Agent 经 MCP 转发原话并根据真实结果回复，设备不直传识别文字、不另行指定播报。交互、内存预算、模拟验证和真实联调限制见 [Gateway 接入说明](docs/gateway-client.md)。

这里保留 LinX 应用、自有资源、测试和必要的硬件接入。当前设备为 **FoloToy AI Passport**：ESP32-C3、8 MB Flash、无 PSRAM，使用 ESP-IDF 5.5.3。

默认独立语音模式下，当前 **LinX 0.2** 保留任务、文字回复和电量的离线演示，首版不支持图片浏览。阅读页只翻页和使用配置手势返回，单击 OK 不录音、不发送；任务概览会在后台准备灵矽连接，选择“继续沟通”才开启麦克风。每次只录入一条指令，结束上传后等待并播放反馈，播完让音频休眠并返回概览。同一目标的语音连接空闲保留 30 秒，Wi-Fi 在正常操作后保持连接。关闭 `CONFIG_LINX_VOICE_ENABLE` 可恢复原来的模拟语音输入，不启动网络和音频。真实语音不会变成演示任务的发送回执，尚未实现设备间对讲；操作和未验证项见 [main/README.md](main/README.md)。

## 目录总览

| 目录 | 用途与入口 |
| --- | --- |
| [main/](main/README.md) | 应用入口、页面、导航、阅读、配网、联网与灵矽语音 |
| [hardware/](hardware/folotoy/ai-passport/README.md) | FoloToy AI Passport 的型号接入配置及必要硬件差异 |
| [assets/](#资源与生成) | 字体、许可、字形清单和生成的图片 |
| [tests/](tests/README.md) | 应用逻辑、依赖接入及真实 LVGL 主机测试 |
| [tools/](tools/README.md) | 固定依赖准备、资源生成／检查与验证入口 |

根目录的 `CMakeLists.txt`、`sdkconfig.defaults`、`partitions.csv` 和 `dependencies.lock` 管理构建、默认配置、分区及组件版本；`upstream.json` 记录上游来源，`AGENTS.md` 约束固件开发。改变这些约定时同步更新相关目录说明。

说明只保留在总览、应用、具体硬件、测试和工具这些入口；品牌中间层、资源子目录和测试子目录不单独重复放置 README。

## 上游来源与本地定制

固件基于 **[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)**，锁定提交 [`006c2279852e352a8433b2c21e5e09be2ca36f77`](https://github.com/FoloToy/ai-passport/commit/006c2279852e352a8433b2c21e5e09be2ca36f77)，采用 [MIT 许可证](LICENSE)。本地维护 LinX 应用和资源；BSP、按键保护及原有验证工具从固定上游只读加载，不复制维护。上游[中文说明](https://github.com/FoloToy/ai-passport/blob/006c2279852e352a8433b2c21e5e09be2ca36f77/docs/README.zh_CN.md)和[硬件指南](https://github.com/FoloToy/ai-passport/blob/006c2279852e352a8433b2c21e5e09be2ca36f77/docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)均指向该版本。硬件工程继续使用 FoloToy 基础；语音协议部分从小智源码移植，见下方来源说明。

依赖缓存位于主仓库 `.local/dependencies/ai-passport/<提交号>/`。验证入口会自动准备并校验；首次获取通常需要网络，完整缓存可离线复用，也可用 `python3 tools/prepare_upstream.py --source <本机仓库> --print-path` 从包含同一提交的 Git 仓库初始化。

AI Passport 所需的硬件适配由上游 `components/bsp/` 提供，本地 `hardware/folotoy/ai-passport/board.cmake` 接入这些驱动。应用层的 `main/linx_device.*` 使用 BSP 接口管理联网、灵矽会话与录放音，`main/linx_wifi_portal.*` 提供手机配网页；不复制或修改上游 BSP，具体对应关系见[硬件说明](hardware/folotoy/ai-passport/README.md)。灵矽相关代码统一使用 `linx_` 前缀。

语音协议复用 **[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32/tree/39d23aee5b8682f77bbdf9cdc66799456b5ad408)**，固定提交 `39d23aee5b8682f77bbdf9cdc66799456b5ad408`。将其 `protocol.cc` 的开始／停止监听、打断，以及 `websocket_protocol.cc` 的 Hello 消息与默认参数规则，适配到现有 `main/linx_protocol.c`，保留 [MIT 版权与许可](LICENSE)。例如最小 Hello 或缺少 format/channels 的合法响应会按小智默认的 24 kHz、单声道 Opus 处理，显式不支持的参数仍拒绝。具体来源文件、函数和适配差异记录在 `upstream.json`。

小智使用 Opus 上行；本设备采用灵矽[官方支持的 PCM 上行](https://linx.qiniu.com/docs/xrobot/platform/websocket)，减少 ESP32-C3 的编码开销，语音回复仍能解码 Opus。未声明设备侧 MCP、AEC 或动态字形等尚未实现的能力；不直接引入小智整套 C++ 应用框架及额外依赖。

语音回复解码使用本次授权新增的唯一运行依赖 **`espressif/esp_audio_codec` 2.6.2**，由 `main/idf_component.yml` 精确固定；只调用 Opus 解码接口。来源和 API 见[官方组件说明](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.6.2/readme)，该组件使用 Espressif 产品适用的许可证，保留其随包许可。LVGL 保持 **9.5.0**，其余驱动组件沿用现有锁定版本。

## 灵矽配置与操作

默认启用真实灵矽对话，使用手机网页配网。上下键选择或翻页，单击 OK 进入或确认。默认长按 OK 约 0.5 秒返回，任务列表用当前返回手势打开设备设置。在“返回方式”按 OK 可切换长按／双击，保存成功后生效，重启保留。长按模式短按松手立即确认；双击模式使用 300 ms 识别窗口，完整双击只返回一次，长按松手不补发确认。下文返回或取消操作均使用当前配置的手势。

操作流程：

1. 在任务列表**长按 OK（双击模式则双击）→ 设备设置 → Wi-Fi 网络**。语音页提示“请先设置 Wi-Fi”时，短按 OK 也会进入手机配网。
2. 设备准备好后显示临时热点名称和八位热点密码。用手机连接该热点，若提示没有互联网，保留连接；然后在手机浏览器打开 **http://192.168.4.1**。
3. 网页选择家中或办公室的 2.4 GHz Wi-Fi，用手机键盘输入密码，点击“连接并保存”。新配置先在内存中试连，取得网络地址后才写入设备 NVS。密码错误可在同一网页修改并重试，原来的保存配置保留；网页不会回传密码或将其写入浏览器本地存储。
4. 保存成功后临时热点关闭。从语音页进入配网，设备会自动继续连接灵矽；从设置进入则显示保存成功，按 OK 返回。返回操作可取消配网；临时热点最长保留五分钟，每次重新进入会生成新热点密码。
5. 任务概览只保留“阅读本轮回复”“继续沟通”“最近对话”。进入概览后，设备在后台连接 Wi-Fi、必要时同步时间，并通过灵矽 HTTPS OTA 网关和 WSS 握手准备语音；此时麦克风和扬声器不启动。阅读页用上下键翻页，使用当前手势返回，再选择“继续沟通”进入语音页；再次打开同一条文字保留上次页码。历史只展示 Agent 回复和用户指令，不提供图片附件入口。OTA 请求只获取设备接入信息，不下载或安装新固件。
6. 未绑定设备时显示网关真实返回的六位激活码，在灵矽平台完成设备绑定及智能体配置，再短按 OK 重试。没有有效激活码时不会显示模拟号码。
7. 选择“继续沟通”后才唤醒音频、开始录音。“发送指令”页提示麦克风已开启后说话，**短按 OK 结束并发送**，随后等待并播放灵矽反馈。等待和播放时单击 OK 不触发新录音或重复发送；反馈正常播完、音频休眠后自动回到概览。短时间内再次选择“继续沟通”，同一目标可复用已准备的语音连接；预热尚未完成时仍会等待连接。返回操作可取消本条并返回，上下键不会取消。

语音连接在握手就绪、本条正常结束或退出本条后空闲保留 **30 秒**。第 29 秒再次开始录音时，旧空闲期限暂停；本条结束后重新计满 30 秒。普通页面操作、重复绘制和服务端心跳不会续期，也不主动关闭连接。到期只关闭语音通道，保留 Wi-Fi；普通返回任务列表、阅读或查看文字历史不会开启录音。同目标预热期间的退出、重进或开始录入会接管原有连接过程。目标的场景、任务、来源轮次或图片标识任一变化，仍重建正确上下文；连接异常按真实故障处理，配网前完整移交网络资源。选择录入时先显示“正在准备录音”，只有实际重建通道才显示连接提示。设备不主动断开正常的 STA 连接，真实断网后下一次准备或录入会重新连接。

用返回手势退出已经开始的指令只停止本地采集和播放，发送 `abort` 并保留 WSS；不通过 `listen.stop` 取消，避免将未发送内容提交。旧轮字幕、TTS 和音频静音排空，收到真实 `tts.stop` 后才允许下一条录音；在此之前重进显示“正在结束上一条指令”。[灵矽协议](https://linx.qiniu.com/docs/xrobot/platform/websocket)没有独立取消确认或每轮请求标识，不能把发送 `abort` 成功当作旧轮已结束。没有收到结束事件时报告超时并按空闲期限释放，不自动重发录音或在保留期内偷偷重连。配网、替换后端和页面资源销毁使用单独的硬释放命令。

设备每次只录入一条消息；服务端应将原话转发给 Codex，语音反馈仅为“已发送”“已发送 codex”或“未能成功发送，原因 xxx”，并关闭主动问候。这些话语由灵矽后台智能体提示词和实际工具结果决定，本次固件修改不更新后台配置，也不新增真实 Codex 任务绑定。无论反馈表达成功还是失败，只要完整播放结束都返回原页面；连接中断、超时、回复被中止或音频错误则停留错误页，短按 OK 重新录入。录音采用流式上传，不保留完整录音，不自动重发；取消也不能撤回已上传内容。设备的播放结束状态只表示本轮录放音结束，不代表 Codex 已接收。

网页列出进入配网时扫描到的最多八个可见网络，按信号强度去重；需要重新扫描时退出设备配网页后重新进入。支持开放网络、WPA2／WPA3 个人网络及 WPA／WPA2 混合网络，企业认证、WPA-only 等不支持。不提供隐藏网络名称输入；个人网络密码支持 8–63 个可打印 ASCII 字符或 64 位十六进制密钥。网页直接显示原始网络名称，不受设备中文字库子集限制。当前需手动打开网页，未实现自动弹出配网页。

开发时可激活 ESP-IDF 5.5.3，在 `firmware/` 运行 `idf.py menuconfig`，进入“LinX 灵矽语音”设置开关及开发用 Wi-Fi。设备优先使用 NVS 已保存的网络，仅在没有保存网络时回退到编译配置。本地 `sdkconfig`、生成配置和镜像会包含填写的凭据，不应提交或公开分享；清空开发配置不会清除设备已保存的网络。关闭 `CONFIG_LINX_VOICE_ENABLE` 保留原离线演示，不显示配网入口。

开发 Wi-Fi 使用 `CONFIG_LINX_WIFI_SSID` 和 `CONFIG_LINX_WIFI_PASSWORD`；`main/sdkconfig.rename` 让 ESP-IDF 自动迁移命名整理前的三项旧配置。迁移表只包含配置键名，不包含凭据，也不会改动设备已经保存在 NVS 的网络。

HTTPS 和 WSS 使用证书与主机名校验；网络时间无法满足校验条件、鉴权失败、超时或断线时显示错误，短按 OK 手动重试，不自动重传上一轮语音。灵矽 Token 只留在当前运行内存，不写源码或 NVS。设备标识及已保存 Wi-Fi 会保留在 NVS。

关机重开后可能需要重新校时。固件依次配置阿里云、腾讯云和 NTP Pool 三个时间源，由 SDK 在无应答时自动轮换；校时单独等待最多 60 秒，期间仍可取消，断网会结束等待。时间源依据[阿里云公网 NTP 说明](https://help.aliyun.com/zh/ecs/user-guide/alibaba-cloud-ntp-server/)和[腾讯云公网 NTP 说明](https://www.tencentcloud.com/document/product/213/32379)。原先仅等 10 秒，短于 SDK 的 15 秒首包应答超时，可能在重试之前就报错；本次修复保留证书与主机名校验，不使用虚构日期绕过校时。

音频采用分块处理：上行固定为 **16 kHz、16 bit、单声道，每 20 ms／640 字节 PCM**；下行 Opus 解码为同一 PCM 格式，固定 **3840 字节**输出缓冲可容纳最长 120 ms 包，不随说话或回复时长累计整段音频。网络消息另有容量上限。已完成一次真机配网、录音与回复播放验证，并记录短流程内存采样，见[应用验收记录](main/README.md#验收状态)；延迟、连续对话及长期内存稳定性仍需验证。

语音页保留波形和大号读秒，短按一次 OK 停录后冻结动效，再等待并播放灵矽 Agent 的回复，音频收尾后返回概览。Gateway 模式通过业务 WSS 查询内容、准备目标及取消未执行请求；OK 停录成功后授权本轮确认，等原文到达再发送一次 `request.confirm`，设备不上传识别文字。图片入口已移除；真实 MCP 逐轮透传和任务投递需按正式服务的完成情况分别验收。

## 资源与生成

- `assets/fonts/` 保存 Noto CJK 简体中文 Regular／Bold 原始字体、字形清单、生成的 `linx_font_*.c` 和[来源校验清单](assets/fonts/manifest.json)。原始字体约 36.6 MB，供电脑重新生成使用；固件编译使用生成的字形子集。字体及生成字形遵循 [SIL 开放字体许可证 1.1](assets/fonts/OFL.txt)。
- `assets/images/linx_image_data.c` 保留生成器绘制的三张原创 LinX 示例图，每张包含总览和两个局部，共九个 RGB565 视图、546,336 字节像素。它们仅作为资源校验素材，本版不链接到应用、不提供图片入口；生成器及示例代码遵循本工程 [MIT 许可证](LICENSE)，图片文字沿用上述字体许可。

新增中文、调整字号或示例图片时修改来源、字形清单或 `tools/generate_linx_assets.py`，重新生成后检查实际字形、行高及资源预算，不直接手改生成数组。生成环境和命令见[工具说明](tools/README.md)。

## 构建与验证

激活 ESP-IDF 5.5.3 环境后，在 `firmware/` 执行：

```bash
./tools/validate.sh --static    # 本地与上游检查、主机测试，使用 IDF 的真实 cJSON
./tools/validate.sh --firmware  # 构建、合并镜像校验与调试归档
./tools/validate.sh             # 完整门禁
./tools/validate_linx_host.sh   # 真实 LVGL 主机测试与截图
./tools/validate_coverage.sh    # main/ 和 hardware/ 自有 C 覆盖率，整体及每个文件至少 90%
```

工具详情见 [tools/README.md](tools/README.md)。合并镜像为 `build/FoloToy-AI-Passport-full.bin`，调试归档位于 `build/firmware/`，截图位于 `build/linx-host/`。构建和主机截图不代表真机验收，以上命令均不烧录。

静态检查新增灵矽协议与设备运行测试，协议解析使用 ESP-IDF 中的真实 cJSON，因此 `--static` 也需要 ESP-IDF 环境。覆盖率另需同版本的 Clang、LLVM 工具，以及 CMake、Ninja；先完成固件构建以解析锁定的 LVGL 组件。验证合并逻辑单测、真实 LVGL 场景、设备入口和灵矽运行模拟测试，检查 `main/`、`hardware/` 内全部自有 C 源码的行、分支及函数覆盖率，不遗漏未命中文件；整体及每个文件三项均要求至少 90%。第三方 BSP、LVGL、cJSON、音频解码库与生成资源不计入自有代码指标。报告保存在 `build/coverage/run.*/`，打开其中 `html/index.html` 查看未覆盖位置。CI 会执行同一门槛，并保存 `firmware-c-coverage` 报告供下载。

`build/`、`managed_components/`、本地 `sdkconfig`、Python 缓存和上游缓存均可按配置重新生成，不属于保留的源码目录。重新获取尚未缓存的组件可能需要联网。升级上游必须明确修改锁定提交、审查差异，并运行完整门禁和截图比较；不要直接编辑缓存。

## 烧录与串口查看

以下命令会实际写入设备，应由操作者确认设备、镜像版本和数据影响后执行。设备需通过 USB 数据线连接；先激活 ESP-IDF **5.5.3** 环境，检查 `idf.py --version`，不要直接使用其他版本的工具链。

在仓库根目录进入固件工程，生成可用于烧录的本地构建目录：

```sh
cd firmware
idf.py -B build/device build
```

默认目标由 `sdkconfig.defaults` 固定为 ESP32-C3、8 MB Flash；如已有本地 `sdkconfig`，需确认没有覆盖这些硬件配置。`validate.sh --firmware` 使用临时构建并清理，不能假设它会保留可直接用于 `idf.py flash` 的构建目录。

在 macOS 用 `ls /dev/cu.usbmodem*` 查看设备串口；Linux 使用实际的 `/dev/ttyACM*` 或 `/dev/ttyUSB*`。将下方变量替换为真实设备路径。

首次安装或需要更新启动程序、分区表时：

```sh
LINX_PORT=/dev/cu.usbmodemXXXX
idf.py -B build/device -p "$LINX_PORT" flash monitor
```

已安装同一分区布局、只更新应用时，使用下面的命令替代上面的 `flash monitor`：

```sh
idf.py -B build/device -p "$LINX_PORT" app-flash monitor
```

当前 [partitions.csv](partitions.csv) 中 NVS 位于 `0x9000`，应用 `factory` 分区位于 `0x10000`。`app-flash` 只更新应用，保留 NVS 中的 Wi-Fi 和返回手势；前提是设备原有分区表与本工程一致。首次 `flash` 会写入启动程序、分区表和应用，已有其他固件的设备不能据此保证旧设置仍可用。

退出串口监视器使用 `Ctrl+]`。只查看运行日志而不重新写入时：

```sh
idf.py -B build/device -p "$LINX_PORT" monitor
```

烧录后按[灵矽配置与操作](#灵矽配置与操作)完成配网和录放音验证。写入成功不代表已接通 Cloud MCP Gateway 或真实 Codex 任务链路。合并镜像 `build/FoloToy-AI-Passport-full.bin` 包含多个区域及中间填充，直接写入可能覆盖 NVS，不应作为保留设备设置的应用更新方式。

命令依据 [ESP-IDF 5.5.3 的 idf.py 说明](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/tools/idf-py.html)及对应版本的[烧录目标定义](https://github.com/espressif/esp-idf/blob/v5.5.3/components/esptool_py/project_include.cmake)核对。本节补充操作说明，不新增真机烧录或验收记录；历史真机结果见[应用验收记录](main/README.md#验收状态)。

## 测试覆盖率

根目录的 Firmware line coverage 徽章是最近记录的**行覆盖率快照**，不会随 CI 自动改写。下表来自 2026-10-25 主线提交 `53f73185a10db549e013dd1f70b1d8942d9c8f64` 的 [GitHub Actions 成功运行](https://github.com/KevinHuo/LinxBox/actions/runs/40168389839)，使用 ESP-IDF 5.5.3、Clang／LLVM 18：

| 指标 | 覆盖率 | 已覆盖 / 总数 |
| --- | --- | --- |
| 行 | 99.30% | 3988 / 4016 |
| 分支 | 93.65% | 3409 / 3640 |
| 函数 | 100.00% | 242 / 242 |

该次采样包含 12 个自有 C 文件，整体及每文件的行、分支、函数覆盖率均达到 90% 门槛。统计范围为 `main/`、`hardware/` 的全部自有 C 源码，排除第三方依赖、生成资源和测试代码；主机模拟覆盖率不代表真机、真实灵矽或完整任务链路验收。

查看最新数据：打开 [Firmware checks](https://github.com/KevinHuo/LinxBox/actions/workflows/firmware.yml) 中对应提交的运行，在摘要查看整体及每文件覆盖率，在 Artifacts 下载 `firmware-c-coverage`（保留 14 天），解压后进入 `run.*` 目录，打开其中的 `html/index.html` 查看未覆盖位置。门禁失败时摘要仍展示已生成的数据；未生成报告时会明确提示查看构建与测试日志。本地复现命令与工具要求见上方[构建与验证](#构建与验证)。
