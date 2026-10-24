# FoloToy AI Passport

品牌 **FoloToy**，型号 **AI Passport**；ESP32-C3、8 MB Flash、无 PSRAM，使用 ESP-IDF 5.5.3。

## 硬件适配代码在哪里

这款设备需要屏幕、按键、音频、电量及引脚适配；固定上游本身就是 AI Passport 工程，已经提供这些代码。构建从这里编译并链接应用需要的驱动。本目录只维护本项目的接入配置，当前没有额外的本地驱动差异，因此不再复制上游实现。

上游 [components/bsp/](https://github.com/FoloToy/ai-passport/tree/006c2279852e352a8433b2c21e5e09be2ca36f77/components/bsp) 中的主要文件：

| 文件 | 适配内容 |
| --- | --- |
| `include/bsp_pins.h` | 引脚、屏幕尺寸、按键电压窗口和音频等板级参数 |
| `src/bsp_display.c`、`src/bsp_display_lvgl.c` | 屏幕、背光与图形库接入 |
| `src/bsp_button.c` | ADC 三键读取与按键事件 |
| `src/bsp_audio.c` | ES8311 音频芯片、录音和播放接口 |
| `src/bsp_battery.c` | CW2017 电量计 |
| `src/bsp_i2c.c` | 音频芯片与电量计共用的 I2C 总线 |

当前离线演示启动屏幕和按键，没有启用真实录音、扬声器或电量采集；已有底层驱动不代表对讲功能已经实现。

## 本目录做什么

[`board.cmake`](board.cmake) 将已校验上游的 `components/bsp/` 注册为 `bsp` 组件，应用通过 `bsp_*` 接口调用。下载后的实际代码位于主仓库 `.local/dependencies/ai-passport/<固定提交>/components/bsp/`；从固件根目录运行 `python3 tools/prepare_upstream.py --print-path` 可以查询依赖位置。来源与版本见 [`upstream.json`](../../../upstream.json)。

需要更改本型号接入或维护确有必要的适配差异时，在这里实现并调整构建接入，记录理由与验证；不直接编辑上游缓存。引脚与参数须核对固定上游的 [`bsp_pins.h`](https://github.com/FoloToy/ai-passport/blob/006c2279852e352a8433b2c21e5e09be2ca36f77/components/bsp/include/bsp_pins.h)、相邻实现、产品规格和实测结果。应用在 [main/](../../../main/README.md)，工程默认配置和分区仍由固件根目录管理。
