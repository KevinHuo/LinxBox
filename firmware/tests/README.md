# 自有测试

`test_linx_demo.c`、`test_linx_input.c`、`test_linx_navigation.c`、`test_linx_reader.c` 检查应用状态、按键、导航和阅读逻辑；`test_prepare_upstream.py`、`test_check_repo.py` 检查依赖准备和本地门禁。

`linx_host/` 在电脑上运行同一套应用界面与真实 LVGL，使用 240 × 320 虚拟屏幕自动执行按键、检查集成行为并保存截图。`renderer.c` 包含正常、缺图及重复操作场景，`lv_conf.h` 配置图形库，`CMakeLists.txt` 声明电脑上的构建，`test_main_contract.py` 检查设备入口的按键与页面处理顺序。

在固件根目录运行 `./tools/validate_linx_host.sh`，默认结果位于 `build/linx-host/`。这是自动测试与截图方式；先核对画面差异，再决定是否更新基线，不自动接受截图变化。屏幕实际配色和实体键仍需真机检查。

行为、边界或缺陷发生变化时更新相关测试，在固件根目录运行 `./tools/validate.sh --static`。上游原有测试直接从固定依赖运行，不在这里复制；主机通过不能替代真机验收。
