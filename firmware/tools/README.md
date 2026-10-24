# 构建与检查工具

本目录只保留固件自有验证入口和资源工具：

| 文件 | 作用 |
| --- | --- |
| `prepare_upstream.py` | 获取并校验 `upstream.json` 锁定的上游，支持完整缓存离线复用 |
| `check_repo.py` | 检查中文说明、本地链接、敏感信息及仓库约定 |
| `generate_linx_assets.py` | 从字体源文件和字形清单生成字体、示例图片；`--check` 检查精确复现 |
| `check_linx_resources.py` | 检查资源清单及应用使用的字体图片 |
| `validate.sh` | `--static` 检查与主机测试，`--firmware` 构建和镜像校验，无参数运行完整门禁 |
| `validate_linx_host.sh` | 运行真实 LVGL 主机验证并输出截图 |

构建流程、依赖接入、资源生成或检查规则变化时修改对应工具，并更新 [tests/](../tests/README.md)。命令从固件根目录运行；重新生成资源需要 Python 3.12、Pillow 11.3.0 和 fonttools 4.60.1。上游原始测试、镜像校验与调试归档工具直接从锁定缓存使用。

普通 `idf.py build` 在配置／重新配置时校验上游，增量构建不一定复查整个缓存；交付使用 `validate.sh` 入口。构建、主机验证和依赖准备均不烧录设备。
