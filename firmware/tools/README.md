# 构建与检查工具

本目录只保留固件自有验证入口和资源工具：

| 文件 | 作用 |
| --- | --- |
| `prepare_upstream.py` | 获取并校验 `upstream.json` 锁定的上游，支持完整缓存离线复用 |
| `check_repo.py` | 检查中文说明、本地链接、敏感信息及仓库约定 |
| `generate_linx_assets.py` | 从字体源文件和字形清单生成字体、示例图片；`--check` 检查精确复现 |
| `check_linx_resources.py` | 检查资源清单及应用使用的字体图片 |
| `check_c_coverage.py` | 对照`main/` 内全部 C 源码 校验 LLVM 报告，整体及每个文件的行、分支、函数覆盖率均不得低于 90% |
| `validate.sh` | `--static` 检查与主机测试，`--firmware` 构建和镜像校验，无参数运行完整门禁 |
| `validate_linx_host.sh` | 运行真实 LVGL 主机验证并输出截图 |
| `validate_coverage.sh` | 用 Clang 合并逻辑、真实 LVGL 和设备入口测试的覆盖率，生成 HTML、原始 JSON 与门槛摘要 |

构建流程、依赖接入、资源生成或检查规则变化时修改对应工具，并更新 [tests/](../tests/README.md)。命令从固件根目录运行；重新生成资源需要 Python 3.12、Pillow 11.3.0 和 fonttools 4.60.1。上游原始测试、镜像校验与调试归档工具直接从锁定缓存使用。

普通 `idf.py build` 在配置／重新配置时校验上游，增量构建不一定复查整个缓存；交付使用 `validate.sh` 入口。构建、主机验证和依赖准备均不烧录设备。

覆盖率命令需要同版本的 Clang、LLVM 工具，以及 CMake、Ninja；先运行 `./tools/validate.sh --firmware` 获取锁定的 LVGL。macOS 默认使用命令行开发工具的 LLVM；Linux 可以显式选择配套版本，例如：

```bash
CLANG=clang-18 CLANGXX=clang++-18 \
LLVM_COV=llvm-cov-18 LLVM_PROFDATA=llvm-profdata-18 \
./tools/validate_coverage.sh
```

Linux 工具环境还需对应的 compiler-rt profile 运行库，CI 使用 Ubuntu 的 `libclang-rt-18-dev`。每次采样在 `build/coverage/run.*/` 新建独立构建和 profile，不复用旧覆盖数据；`LINX_COVERAGE_DIR` 可指定其他输出目录。HTML 位于 `html/index.html`，`coverage.json` 保留 LLVM 原始结果，`summary.json` 保留固定门槛摘要。统计包含设备入口，仅排除第三方 BSP／LVGL、生成的字体图片及测试代码；这些排除不代表真机驱动已验证。
