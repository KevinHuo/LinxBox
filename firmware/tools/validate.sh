#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "用法：$0 [--all|--static|--firmware]" >&2
}

run_static_checks() (
    local actionlint_bin
    local git_root
    local test_dir
    local upstream_cc="${CC:-cc}"
    local cjson_dir="${IDF_PATH:-}/components/json/cJSON"

    if [[ ! -f "${cjson_dir}/cJSON.c" ]]; then
        echo "错误：缺少 ESP-IDF 自带 cJSON，请先激活 ESP-IDF 5.5.3。" >&2
        return 1
    fi

    python3 tools/check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_prepare_upstream.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_c_coverage.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$("${UPSTREAM_DIR}/tools/install-actionlint.sh")"
    fi
    actionlint_bin="$(cd -- "$(dirname -- "${actionlint_bin}")" && pwd)/$(basename -- "${actionlint_bin}")"
    git_root="$(git rev-parse --show-toplevel)"
    "${actionlint_bin}" -color "${git_root}/.github/workflows/firmware.yml"

    test_dir="$(mktemp -d /tmp/linx-host-tests.XXXXXX)"
    trap 'case "${test_dir}" in /tmp/linx-host-tests.*) rm -rf -- "${test_dir}" ;; esac' EXIT
    if [[ "${upstream_cc}" == */* && "${upstream_cc}" != /* ]]; then
        upstream_cc="${repo_root}/${upstream_cc}"
    fi
    if [[ "$(uname -s)" == "Darwin" ]]; then
        # 上游固定版本使用 GNU 链接选项；仅转换为 macOS 的等价选项。
        cat > "${test_dir}/upstream-cc" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
args=()
for arg in "$@"; do
    if [[ "${arg}" == "-Wl,--gc-sections" ]]; then
        arg="-Wl,-dead_strip"
    fi
    args+=("${arg}")
done
exec "${AI_PASSPORT_HOST_CC}" "${args[@]}"
EOF
        chmod +x "${test_dir}/upstream-cc"
        export AI_PASSPORT_HOST_CC="${upstream_cc}"
        upstream_cc="${test_dir}/upstream-cc"
    fi
    (
        cd "${UPSTREAM_DIR}"
        ACTIONLINT_BIN="${actionlint_bin}" CC="${upstream_cc}" \
            PYTHONDONTWRITEBYTECODE=1 ./tools/validate.sh --static
    )

    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_linx_input.c main/linx_input.c \
        -o "${test_dir}/test_linx_input"
    "${test_dir}/test_linx_input"
    "${CC:-cc}" -std=c11 -O0 -Wall -Wextra -Werror -Itests/sdk_stubs \
        -Imain tests/test_linx_input_settings.c -o "${test_dir}/test_linx_input_settings"
    "${test_dir}/test_linx_input_settings"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_linx_wifi.c main/linx_wifi.c \
        -o "${test_dir}/test_linx_wifi"
    "${test_dir}/test_linx_wifi"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_linx_demo.c main/linx_demo.c main/linx_content.c \
        -o "${test_dir}/test_linx_demo"
    "${test_dir}/test_linx_demo"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_linx_navigation.c main/linx_navigation.c \
        -o "${test_dir}/test_linx_navigation"
    "${test_dir}/test_linx_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_linx_reader.c main/linx_reader.c main/linx_navigation.c main/linx_content.c \
        -o "${test_dir}/test_linx_reader"
    "${test_dir}/test_linx_reader"
    local scenarios
    local variant
    local runtime_flags
    for variant in offline voice; do
        runtime_flags=(-Itests/device_stubs -I"${UPSTREAM_DIR}/main")
        if [[ "${variant}" == "voice" ]]; then
            runtime_flags+=(-DCONFIG_LINX_VOICE_ENABLE=1)
        fi
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain "${runtime_flags[@]}" \
            tests/test_main_runtime.c main/main.c main/linx_input.c \
            -o "${test_dir}/test_main_runtime_${variant}"
        scenarios="$("${test_dir}/test_main_runtime_${variant}" --list)"
        while IFS= read -r scenario; do
            "${test_dir}/test_main_runtime_${variant}" "${scenario}"
        done <<< "${scenarios}"
    done
    # 使用当前 ESP-IDF 的真实 JSON 实现；第三方对象与自有代码分开编译。
    "${CC:-cc}" -std=c11 -O0 -g -c "${cjson_dir}/cJSON.c" -o "${test_dir}/cJSON.o"
    "${CC:-cc}" -std=c11 -O0 -Wall -Wextra -Werror -Imain -I"${cjson_dir}" \
        tests/test_linx_protocol.c main/linx_protocol.c \
        "${test_dir}/cJSON.o" -lm -o "${test_dir}/test_linx_protocol"
    "${test_dir}/test_linx_protocol"
    "${CC:-cc}" -std=c11 -O0 -Wall -Wextra -Werror -Itests/sdk_stubs \
        -Imain -I"${cjson_dir}" \
        tests/test_linx_device.c main/linx_protocol.c main/linx_wifi.c \
        "${test_dir}/cJSON.o" -lm -o "${test_dir}/test_linx_device"
    "${test_dir}/test_linx_device"
    "${CC:-cc}" -std=c11 -O0 -Wall -Wextra -Werror -Itests/sdk_stubs \
        -Itests/device_stubs -Imain -I"${cjson_dir}" \
        tests/test_linx_wifi_portal.c main/linx_wifi.c "${test_dir}/cJSON.o" \
        -lm -o "${test_dir}/test_linx_wifi_portal"
    "${test_dir}/test_linx_wifi_portal"
    python3 tools/check_linx_resources.py
    echo "LinX 主机测试：通过"
)

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "错误：未找到 idf.py，请先激活 ESP-IDF 5.5.3。" >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 "${UPSTREAM_DIR}/tools/verify_firmware.py" "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 "${UPSTREAM_DIR}/tools/archive_firmware.py" create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "固件构建：通过"
)

cd "${repo_root}"
case "${mode}" in
    --all|--static|--firmware) ;;
    *)
        usage
        exit 2
        ;;
esac
UPSTREAM_DIR="$(python3 tools/prepare_upstream.py --print-path)"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
esac
