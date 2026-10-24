# 自有测试

`test_linx_demo.c`、`test_linx_input.c`、`test_linx_navigation.c`、`test_linx_reader.c` 检查应用状态、按键、导航和阅读逻辑；`test_prepare_upstream.py`、`test_check_repo.py`、`test_check_c_coverage.py` 检查依赖准备、本地门禁及覆盖率门槛。

`test_main_runtime.c` 运行真实 `main/main.c` 和上游按键保护逻辑，`device_stubs/` 仅提供 ESP-IDF／BSP 接口替身。每个场景在独立进程中调用一次设备入口，模拟初始化失败、唤醒保护、队列溢出、切页屏障和定期内存日志，检查调用顺序及结果；这部分不连接板卡，也不能证明真实驱动行为。

`linx_host/` 在电脑上运行同一套应用界面与真实 LVGL，使用 240 × 320 虚拟屏幕自动执行按键、检查集成行为并保存截图。`renderer.c` 包含正常、缺图及重复操作场景，`lv_conf.h` 配置图形库，`CMakeLists.txt` 声明电脑上的构建，`test_main_contract.py` 检查设备入口的按键与页面处理顺序。

`linx_host/test_ui_boundaries.c` 直接包含真实界面实现，单独测试内部对象容量、文字测量及导航边界，断言容量用尽后不会越界并可重置复用；不为测试增加业务 API。

在固件根目录运行 `./tools/validate_linx_host.sh`，默认结果位于 `build/linx-host/`。这是自动测试与截图方式；先核对画面差异，再决定是否更新基线，不自动接受截图变化。屏幕实际配色和实体键仍需真机检查。

行为、边界或缺陷发生变化时更新相关测试，在固件根目录运行 `./tools/validate.sh --static`。上游原有测试直接从固定依赖运行，不在这里复制；主机通过不能替代真机验收。

`./tools/validate_coverage.sh` 会重新构建并运行以上 C 测试，统一统计`main/` 内全部自有 C 源码（包括设备入口）的行、分支和函数覆盖率；整体及每个文件均要求至少 90%。第三方 BSP／LVGL、生成资源和测试替身不计入应用指标，新增应用源码若未进入报告会直接失败。环境、报告位置见[工具说明](../tools/README.md)。
