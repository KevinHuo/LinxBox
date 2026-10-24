# 固件开发规范

本目录是 LinxBox 的固件工程；同时遵守[主仓库规范](../AGENTS.md)、[代码规范](../CODING_STANDARDS.md)和[协作规范](../CONTRIBUTING.md)。先读[固件说明](README.md)、[当前应用说明](main/README.md)，再按任务读取相关代码和测试。固件命令从本目录运行。

## 维护边界

- 当前硬件为 FoloToy AI Passport，ESP32-C3、8 MB Flash、无 PSRAM，ESP-IDF 5.5.3。当前 LinX 0.2 是离线界面演示，尚未实现对讲。
- 开始前检查 Git 状态，保留已有修改和未跟踪文件；只做当前任务，不擅自清理、覆盖或改动无关内容。
- 自有应用放在 `main/`，字体和图片资源分别放在 `assets/fonts/`、`assets/images/`，对应测试和工具留在 `tests/`、`tools/`；硬件接入配置及必要补丁归入 `hardware/folotoy/ai-passport/`，不得混入应用页面。
- 上游唯一版本来源为 `upstream.json`。使用 `python3 tools/prepare_upstream.py --print-path` 获取并校验依赖；缓存位于父仓库 `.local/dependencies/ai-passport/<提交号>/`，只读使用，不直接编辑或重新复制 BSP、上游文档、示例及技能。
- 现有工程不要求额外安装或维护上游五个技能。需要上游资料时读取固定提交的[中文项目说明](https://github.com/FoloToy/ai-passport/blob/006c2279852e352a8433b2c21e5e09be2ca36f77/docs/README.zh_CN.md)和[硬件指南](https://github.com/FoloToy/ai-passport/blob/006c2279852e352a8433b2c21e5e09be2ca36f77/docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)，不把上游维护流程扩展成本项目任务。
- 本仓库说明、规范、Issue 和提交／PR 正文使用中文；代码标识、命令、必要技术名称及许可证原文保持原样。

## 实现与安全

- 硬件事实以产品规格、实测结果及锁定上游的 `components/bsp/include/bsp_pins.h` 和相邻实现为依据。缺少任务所需硬件信息时先核实，不猜测引脚、时序或电源行为。
- LVGL 非线程安全，LVGL 任务外访问对象必须持有 `bsp_lvgl_lock()`。退出页面前停止所有可能访问页面的任务、定时器、回调和事件处理器。
- 按键回调不得阻塞，音频、存储、网络等慢操作放在工作任务。可测试的状态机、协议、计时和布局计算应与 ESP-IDF／LVGL 解耦，并有主机测试。
- 中文显示必须核对实际文案的字形覆盖、控件选用字体与行高；UTF-8 正确或编译成功不能证明真机显示正确。字体图片应修改来源或生成器后重新生成，并运行资源检查。
- 不擅自改变分区、组件版本、驱动行为或增加依赖。确需升级上游时明确锁定完整提交，审查差异并执行完整验证，保留来源与许可证。
- 不提交凭证、设备二维码秘密、私钥、个人数据、未脱敏日志、依赖缓存或生成的固件产物。
- 构建、设备检测与本机测试均不授权烧录、全片擦除、发布或远端 Git 操作；按用户明确授权执行，不擅自刷写设备。烧录不以读取设备旧固件为强制前提，但必须说明数据影响。

## 验证与交付

迭代时先运行最小相关检查，交付前按修改范围执行以下入口；三种 `validate.sh` 模式会自动准备固定上游依赖：

```bash
./tools/validate.sh --static
./tools/validate.sh --firmware
./tools/validate.sh
./tools/validate_linx_host.sh
```

完整门禁要求 ESP-IDF 5.5.3 环境；真实 LVGL 主机测试还需要已解析的管理组件。不得跳过检查、放宽门槛或把编译成功当成业务／真机验收通过。最终说明改动文件、简化内容及剩余风险，并分别报告：

```text
构建：通过／失败／未运行
主机测试：通过／失败／未运行
真机测试：通过／失败／未运行
未验证项：仍需板卡、仪器或用户确认的事项
```

纯文档修改说明不适用项；缺少环境或设备时报告实际阻碍，不编造结果。没有明确要求时，不提交、推送、发布或烧录。
