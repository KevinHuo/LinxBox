# 自有测试

`test_linx_demo.c`、`test_linx_input.c`、`test_linx_navigation.c`、`test_linx_reader.c` 检查应用状态、按键、导航和阅读逻辑；`test_prepare_upstream.py`、`test_check_repo.py`、`test_check_c_coverage.py` 检查依赖准备、本地门禁及覆盖率门槛。

`test_linx_wifi.c` 检查密码长度和格式边界；`test_linx_wifi_portal.c` 检查手机网页、HTTP 参数、状态和有界凭据传递。设备运行测试还模拟 Wi-Fi 扫描、临时热点、取消、同一网页多次 RAM 试连及成功后保存，确认错误密码不会覆盖旧网络；真实 LVGL 测试覆盖手机配网指引、热点凭据校验与清理、错误重试及回到原语音页。这些测试不替代实际路由器与设备配网验证。

`test_main_runtime.c` 运行真实 `main/main.c` 和上游按键保护逻辑，`device_stubs/` 仅提供 ESP-IDF／BSP 接口替身。每个场景在独立进程中调用一次设备入口，模拟初始化失败、唤醒保护、队列溢出、切页屏障和定期内存日志，检查调用顺序及结果；这部分不连接板卡，也不能证明真实驱动行为。

`test_linx_lingxi_protocol.c` 使用 ESP-IDF 中的真实 cJSON，检查设备接入消息、激活码、WSS 地址、会话事件、长度上限及 WebSocket 拆包，并回归小智最小 Hello、可选音频参数和开始／停止／打断消息；`test_linx_lingxi_device.c` 运行本地 `hardware/folotoy/ai-passport/linx_lingxi_device.c`，通过 `lingxi_stubs/` 模拟 Wi-Fi、时间、HTTPS、WebSocket、Opus 与 BSP 接口，检查录放音、结束上传、取消及错误清理。替身不访问真实灵矽、网络、麦克风或账号，不能证明 Opus 库的真实解码效果或真机语音链路。

冷启动校时回归模拟最多 5 秒启动延迟、首台服务器无应答后 15 秒轮换，覆盖超过原 10 秒期限才成功、三个时间源分别成功、全部失败后 60 秒退出、等待期间取消与断网，以及已有有效时间时跳过校时。测试只验证应用能正确等待 SDK，不代替真实 DNS、UDP、备用服务器切换和断电重启验证。

`linx_host/` 在电脑上运行同一套应用界面与真实 LVGL，使用 240 × 320 虚拟屏幕自动执行按键、检查集成行为并保存截图。`renderer.c` 包含正常、缺图及重复操作场景，`lv_conf.h` 配置图形库，`CMakeLists.txt` 声明电脑上的构建，`test_main_contract.py` 检查设备入口的按键与页面处理顺序。

`linx_host/test_ui_boundaries.c` 直接包含真实界面实现，单独测试内部对象容量、文字测量及导航边界，断言容量用尽后不会越界并可重置复用；不为测试增加业务 API。

在固件根目录运行 `./tools/validate_linx_host.sh`，默认结果位于 `build/linx-host/`。这是自动测试与截图方式；先核对画面差异，再决定是否更新基线，不自动接受截图变化。屏幕实际配色和实体键仍需真机检查。

行为、边界或缺陷发生变化时更新相关测试，激活 ESP-IDF 5.5.3 后在固件根目录运行 `./tools/validate.sh --static`；静态检查中的协议测试需要 IDF 自带的真实 cJSON 源码。上游原有测试直接从固定依赖运行，不在这里复制；主机通过不能替代真机验收。

`./tools/validate_coverage.sh` 会重新构建并运行以上 C 测试，统一统计 `main/` 与 `hardware/` 内全部自有 C 源码（包括设备入口和灵矽设备运行实现）的行、分支和函数覆盖率；整体及每个文件三项均要求至少 90%。第三方 BSP／LVGL／cJSON／音频解码库、生成资源和测试替身不计入应用指标，新增自有源码若未进入报告会直接失败。环境、报告位置见[工具说明](../tools/README.md)。
