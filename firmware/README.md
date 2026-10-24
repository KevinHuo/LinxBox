# LinxBox 固件

这里保留 LinX 应用、自有资源、测试和必要的硬件接入。当前设备为 **FoloToy AI Passport**：ESP32-C3、8 MB Flash、无 PSRAM，使用 ESP-IDF 5.5.3。

当前 **LinX 0.2 离线界面演示**使用模拟任务、回复、图片、连接、电量和语音转写，尚未实现设备间对讲；操作和未验证项见 [main/README.md](main/README.md)。

## 目录总览

| 目录 | 用途与入口 |
| --- | --- |
| [main/](main/README.md) | 应用入口、页面、内容、按键、导航与阅读 |
| [hardware/](hardware/folotoy/ai-passport/README.md) | 按品牌／型号组织硬件接入，当前为 FoloToy AI Passport |
| [assets/](#资源与生成) | 字体、许可、字形清单和生成的图片 |
| [tests/](tests/README.md) | 应用逻辑、依赖接入及真实 LVGL 主机测试 |
| [tools/](tools/README.md) | 固定依赖准备、资源生成／检查与验证入口 |

根目录的 `CMakeLists.txt`、`sdkconfig.defaults`、`partitions.csv` 和 `dependencies.lock` 管理构建、默认配置、分区及组件版本；`upstream.json` 记录上游来源，`AGENTS.md` 约束固件开发。改变这些约定时同步更新相关目录说明。

说明只保留在总览、应用、具体硬件、测试和工具这些入口；品牌中间层、资源子目录和测试子目录不单独重复放置 README。

## 上游来源与本地定制

固件基于 **[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)**，锁定提交 [`006c2279852e352a8433b2c21e5e09be2ca36f77`](https://github.com/FoloToy/ai-passport/commit/006c2279852e352a8433b2c21e5e09be2ca36f77)，采用 [MIT 许可证](LICENSE)。本地维护 LinX 应用和资源；BSP、按键保护及原有验证工具从固定上游只读加载，不复制维护。上游[中文说明](https://github.com/FoloToy/ai-passport/blob/006c2279852e352a8433b2c21e5e09be2ca36f77/docs/README.zh_CN.md)和[硬件指南](https://github.com/FoloToy/ai-passport/blob/006c2279852e352a8433b2c21e5e09be2ca36f77/docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)均指向该版本。本工程不是小智固件。

依赖缓存位于主仓库 `.local/dependencies/ai-passport/<提交号>/`。验证入口会自动准备并校验；首次获取通常需要网络，完整缓存可离线复用，也可用 `python3 tools/prepare_upstream.py --source <本机仓库> --print-path` 从包含同一提交的 Git 仓库初始化。

AI Passport 所需的硬件适配已由上游 `components/bsp/` 提供。本地 `hardware/folotoy/ai-passport/board.cmake` 将这些驱动接入构建；当前没有另行维护的驱动差异，具体对应关系见[硬件说明](hardware/folotoy/ai-passport/README.md)。

## 资源与生成

- `assets/fonts/` 保存 Noto CJK 简体中文 Regular／Bold 原始字体、字形清单、生成的 `linx_font_*.c` 和[来源校验清单](assets/fonts/manifest.json)。原始字体约 36.6 MB，供电脑重新生成使用；固件编译使用生成的字形子集。字体及生成字形遵循 [SIL 开放字体许可证 1.1](assets/fonts/OFL.txt)。
- `assets/images/linx_image_data.c` 保存生成器绘制的三张原创 LinX 示例图，每张包含总览和两个局部，共九个 RGB565 视图、546,336 字节只读像素。图片为模拟素材；生成器及示例代码遵循本工程 [MIT 许可证](LICENSE)，图片文字沿用上述字体许可。

新增中文、调整字号或示例图片时修改来源、字形清单或 `tools/generate_linx_assets.py`，重新生成后检查实际字形、行高及资源预算，不直接手改生成数组。生成环境和命令见[工具说明](tools/README.md)。

## 构建与验证

激活 ESP-IDF 5.5.3 环境后，在 `firmware/` 执行：

```bash
./tools/validate.sh --static    # 本地与上游检查、主机测试
./tools/validate.sh --firmware  # 构建、合并镜像校验与调试归档
./tools/validate.sh             # 完整门禁
./tools/validate_linx_host.sh   # 真实 LVGL 主机测试与截图
```

工具详情见 [tools/README.md](tools/README.md)。合并镜像为 `build/FoloToy-AI-Passport-full.bin`，调试归档位于 `build/firmware/`，截图位于 `build/linx-host/`。构建和主机截图不代表真机验收，以上命令均不烧录。

`build/`、`managed_components/`、本地 `sdkconfig`、Python 缓存和上游缓存均可按配置重新生成，不属于保留的源码目录。重新获取尚未缓存的组件可能需要联网。升级上游必须明确修改锁定提交、审查差异，并运行完整门禁和截图比较；不要直接编辑缓存。
