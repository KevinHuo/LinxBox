#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="${LINX_COVERAGE_DIR:-${repo_root}/build/coverage}"
coverage_cc="${CLANG:-clang}"
coverage_cxx="${CLANGXX:-clang++}"
llvm_cov="${LLVM_COV:-llvm-cov}"
llvm_profdata="${LLVM_PROFDATA:-llvm-profdata}"
if [[ "$(uname -s)" == "Darwin" ]]; then
    llvm_cov="${LLVM_COV:-$(xcrun --find llvm-cov)}"
    llvm_profdata="${LLVM_PROFDATA:-$(xcrun --find llvm-profdata)}"
fi
for tool in "${coverage_cc}" "${coverage_cxx}" "${llvm_cov}" "${llvm_profdata}" cmake ninja python3; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "缺少覆盖率工具：${tool}；请使用 Clang 和同版本 LLVM 工具。" >&2
        exit 1
    fi
done
cd "${repo_root}"
UPSTREAM_DIR="$(python3 tools/prepare_upstream.py --print-path)"
cjson_dir="${IDF_PATH:-}/components/json/cJSON"
if [[ ! -f "${cjson_dir}/cJSON.c" ]]; then
    echo "缺少 ESP-IDF 自带 cJSON，请先激活 ESP-IDF 5.5.3。" >&2
    exit 1
fi
if [[ ! -f managed_components/lvgl__lvgl/CMakeLists.txt ]]; then
    echo "缺少已锁定的 LVGL 组件，请先执行 ./tools/validate.sh --firmware。" >&2
    exit 1
fi
mkdir -p "${output_dir}"
output_dir="$(cd -- "${output_dir}" && pwd)"
# 每次独立构建和采样，避免旧 profile 使未运行的用例看似已覆盖。
run_dir="$(mktemp -d "${output_dir}/run.XXXXXX")"
# CI 容器以 root 构建，宿主 runner 仍需读取公开的验证报告。
chmod 755 "${run_dir}"
mkdir -p "${run_dir}/profiles" "${run_dir}/screens" "${run_dir}/screens-missing-image"
export LLVM_PROFILE_FILE="${run_dir}/profiles/%m-%p.profraw"

PYTHONDONTWRITEBYTECODE=1 python3 tests/linx_host/test_main_contract.py
cmake -S tests/linx_host -B "${run_dir}/host" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="${coverage_cc}" \
    -DCMAKE_CXX_COMPILER="${coverage_cxx}" -DAI_PASSPORT_DIR="${UPSTREAM_DIR}" \
    -DLINX_ENABLE_COVERAGE=ON
cmake --build "${run_dir}/host" --parallel 4
"${run_dir}/host/linx_host" "${run_dir}/screens"
"${run_dir}/host/linx_host_missing_image" "${run_dir}/screens-missing-image" --missing-image
"${run_dir}/host/linx_ui_boundaries"

binaries=("${run_dir}/host/linx_host" "${run_dir}/host/linx_host_missing_image" "${run_dir}/host/linx_ui_boundaries")
compile_test() {
    local name="$1"
    shift
    "${coverage_cc}" -std=c11 -O0 -g -Wall -Wextra -Werror \
        -fprofile-instr-generate -fcoverage-mapping -Imain "$@" -o "${run_dir}/${name}"
    binaries+=("${run_dir}/${name}")
}
compile_test test_linx_input tests/test_linx_input.c main/linx_input.c
compile_test test_linx_wifi tests/test_linx_wifi.c main/linx_wifi.c
compile_test test_linx_demo tests/test_linx_demo.c main/linx_demo.c main/linx_content.c
compile_test test_linx_navigation tests/test_linx_navigation.c main/linx_navigation.c
compile_test test_linx_reader tests/test_linx_reader.c main/linx_reader.c main/linx_navigation.c main/linx_content.c
for test in test_linx_input test_linx_wifi test_linx_demo test_linx_navigation test_linx_reader; do
    "${run_dir}/${test}"
done
for variant in offline lingxi; do
    runtime_flags=(-Itests/device_stubs -I"${UPSTREAM_DIR}/main" -Ihardware/folotoy/ai-passport)
    if [[ "${variant}" == "lingxi" ]]; then
        runtime_flags+=(-DCONFIG_LINX_LINGXI_ENABLE=1)
    fi
    compile_test "test_main_runtime_${variant}" "${runtime_flags[@]}" \
        tests/test_main_runtime.c main/main.c main/linx_input.c
    scenarios="$("${run_dir}/test_main_runtime_${variant}" --list)"
    while IFS= read -r scenario; do
        "${run_dir}/test_main_runtime_${variant}" "${scenario}"
    done <<< "${scenarios}"
done
# 第三方 JSON 库不进入自有代码统计；两组语音测试仍调用其真实实现。
"${coverage_cc}" -std=c11 -O0 -g -c "${cjson_dir}/cJSON.c" -o "${run_dir}/cJSON.o"
compile_test test_linx_lingxi_protocol -I"${cjson_dir}" \
    tests/test_linx_lingxi_protocol.c main/linx_lingxi_protocol.c "${run_dir}/cJSON.o" -lm
"${run_dir}/test_linx_lingxi_protocol"
compile_test test_linx_lingxi_device -Itests/lingxi_stubs \
    -Ihardware/folotoy/ai-passport -I"${cjson_dir}" \
    tests/test_linx_lingxi_device.c main/linx_lingxi_protocol.c main/linx_wifi.c "${run_dir}/cJSON.o" -lm
"${run_dir}/test_linx_lingxi_device"
compile_test test_linx_wifi_portal -Itests/lingxi_stubs -Itests/device_stubs \
    -Ihardware/folotoy/ai-passport -I"${cjson_dir}" \
    tests/test_linx_wifi_portal.c main/linx_wifi.c "${run_dir}/cJSON.o" -lm
"${run_dir}/test_linx_wifi_portal"

"${llvm_profdata}" merge -sparse "${run_dir}"/profiles/*.profraw -o "${run_dir}/combined.profdata"
coverage_args=("${binaries[0]}")
for binary in "${binaries[@]:1}"; do
    coverage_args+=(-object "${binary}")
done
source_list="$(python3 -c 'from pathlib import Path; print("\n".join(str(path.resolve()) for folder in ("main", "hardware") for path in sorted(Path(folder).rglob("*.c"))))')"
source_files=()
while IFS= read -r source; do
    source_files+=("${source}")
done <<< "${source_list}"
coverage_args+=(-instr-profile="${run_dir}/combined.profdata" -sources "${source_files[@]}")
"${llvm_cov}" export "${coverage_args[@]}" > "${run_dir}/coverage.json"
"${llvm_cov}" report "${coverage_args[@]}" > "${run_dir}/report.txt"
"${llvm_cov}" show "${coverage_args[@]}" -format=html -show-branches=count \
    -output-dir="${run_dir}/html"
# llvm-cov 为 HTML 创建私有子目录；报告上传由容器外的 runner 执行。
chmod -R a+rX "${run_dir}/html"
echo "报告目录：${run_dir}"
python3 tools/check_c_coverage.py "${run_dir}/coverage.json" --summary "${run_dir}/summary.json"
