#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${LINX_HOST_BUILD_DIR:-${repo_root}/build/linx-host}"
UPSTREAM_DIR="$(python3 "${repo_root}/tools/prepare_upstream.py" --print-path)"
if [[ ! -f "${repo_root}/managed_components/lvgl__lvgl/CMakeLists.txt" ]]; then
    echo "缺少已锁定的 LVGL 组件，请先解析固件依赖。" >&2
    exit 1
fi
python3 "${repo_root}/tests/linx_host/test_main_contract.py"
cmake -S "${repo_root}/tests/linx_host" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DAI_PASSPORT_DIR="${UPSTREAM_DIR}"
cmake --build "${build_dir}" --parallel 4
mkdir -p "${build_dir}/screens"
"${build_dir}/linx_host" "${build_dir}/screens"

mkdir -p "${build_dir}/screens-missing-image"
"${build_dir}/linx_host_missing_image" "${build_dir}/screens-missing-image" --missing-image
"${build_dir}/linx_ui_boundaries"
