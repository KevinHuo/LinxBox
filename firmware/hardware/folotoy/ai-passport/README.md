# FoloToy AI Passport

品牌 **FoloToy**，型号 **AI Passport**；ESP32-C3、8 MB Flash、无 PSRAM，使用 ESP-IDF 5.5.3。

## 硬件适配代码在哪里

这款设备需要屏幕、按键、音频、电量及引脚适配；固定上游本身就是 AI Passport 工程，已经提供这些驱动。本目录通过 `board.cmake` 接入上游 BSP，另用 `linx_lingxi_device.*` 维护本项目的联网、灵矽会话和录放音生命周期，不复制上游实现。

上游 [components/bsp/](https://github.com/FoloToy/ai-passport/tree/006c2279852e352a8433b2c21e5e09be2ca36f77/components/bsp) 中的主要文件：

| 文件 | 适配内容 |
| --- | --- |
| `include/bsp_pins.h` | 引脚、屏幕尺寸、按键电压窗口和音频等板级参数 |
| `src/bsp_display.c`、`src/bsp_display_lvgl.c` | 屏幕、背光与图形库接入 |
| `src/bsp_button.c` | ADC 三键读取与按键事件 |
| `src/bsp_audio.c` | ES8311 音频芯片、录音和播放接口 |
| `src/bsp_battery.c` | CW2017 电量计 |
| `src/bsp_i2c.c` | 音频芯片与电量计共用的 I2C 总线 |

应用启动屏幕和按键；真实灵矽功能启用时，配网页启动 Wi-Fi 扫描、临时热点与网页或试连，语音页按需启动 Wi-Fi、麦克风和扬声器。未启用真实电量采集；已有底层驱动不代表设备间对讲已经实现。关闭 `CONFIG_LINX_LINGXI_ENABLE` 时保留不启动网络和音频的离线演示。

## 本目录做什么

[`board.cmake`](board.cmake) 将已校验上游的 `components/bsp/` 注册为 `bsp` 组件，应用通过 `bsp_*` 接口调用。下载后的实际代码位于主仓库 `.local/dependencies/ai-passport/<固定提交>/components/bsp/`；从固件根目录运行 `python3 tools/prepare_upstream.py --print-path` 可以查询依赖位置。来源与版本见 [`upstream.json`](../../../upstream.json)。

`linx_lingxi_device.c` 在独立工作任务中管理 Wi-Fi、网络时间、HTTPS 设备接入、WSS、Opus 解码及 BSP 音频。页面只投递开始、结束说话和取消命令，再由主循环读取状态快照，网络与音频任务不持有界面对象。退出或失败后关闭连接、停止本次启动的服务并让音频休眠；不自动重传用户录音。

上行使用每 20 ms／640 字节的 16 kHz、16 bit、单声道 PCM，下行使用固定版本 `espressif/esp_audio_codec` 2.6.2 的 Opus 解码器，输出同一格式；最长 120 ms 包使用固定 3840 字节缓冲。缓冲不随对话时长增长；短流程已完成真机录音与回复验证并采样内存和栈水位，长期并行负载仍需板卡测量，详见[应用验收记录](../../../main/README.md#验收状态)。

保存网络时还需处理固定 SDK 的配置去重：试连成功后断开 STA 并排空旧事件，在 RAM 恢复本次配网前的配置，再切到 FLASH 写入新配置，以确保触发实际持久化。热点在这期间继续提供网页状态。回归须模拟同值写入被跳过，并验证重新初始化 Wi-Fi 后能读到已保存网络。

Wi-Fi 配网与语音共用同一个工作任务，扫描结果和命令使用固定容量。`linx_wifi_portal.*` 使用 ESP-IDF 自带 HTTP 服务提供手机网页，临时 WPA2 热点密码每次生成，配置只留 RAM，成功、取消或五分钟超时后关闭；HTTP 不与语音录放音同时运行。候选配置只在 RAM 试连，获得 IP 后才保存到 NVS，失败或取消保留旧配置；离开页面先停止网页服务和热点，再释放网络接口并清理扫描结果和密码缓冲。语音连接优先读取设备已保存网络，没有保存配置时才回退到 menuconfig 的开发配置。设备接入从 HTTPS 网关取得真实激活码或 WSS 鉴权信息；Token 只留在运行内存。HTTPS 和 WSS 保留证书及主机名验证，无法取得可用于校验的时间时报告失败。OTA 网关用于接入信息获取，本实现不执行固件更新。配置与操作见[固件说明](../../../README.md#灵矽配置与操作)。

需要更改本型号接入或维护确有必要的适配差异时，在这里实现并调整构建接入，记录理由与验证；不直接编辑上游缓存。引脚与参数须核对固定上游的 [`bsp_pins.h`](https://github.com/FoloToy/ai-passport/blob/006c2279852e352a8433b2c21e5e09be2ca36f77/components/bsp/include/bsp_pins.h)、相邻实现、产品规格和实测结果。应用在 [main/](../../../main/README.md)，工程默认配置和分区仍由固件根目录管理。
