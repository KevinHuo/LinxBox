#!/usr/bin/env bash
set -euo pipefail
firmware_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
contract_python="${LINX_CONTRACT_PYTHON:-python3}"
output_dir="${LINX_GATEWAY_TEST_OUTPUT:-${firmware_root}/build/gateway-mock}"
host_binary="${1:-${firmware_root}/build/linx-host/linx_gateway_host}"
if [[ ! -x "${host_binary}" ]]; then
    echo "缺少 C/LVGL host，请先运行 tools/validate_linx_host.sh。" >&2
    exit 1
fi
mkdir -p "${output_dir}"
make -C "${firmware_root}/../gateway" build
LINX_GATEWAY_GO_BINARY="${firmware_root}/../gateway/bin/linxbox-gateway" \
LINX_GATEWAY_HOST_BINARY="${host_binary}" LINX_GATEWAY_TEST_OUTPUT="${output_dir}" \
    node --test "${firmware_root}/tests/test_go_gateway.mjs"
"${contract_python}" "${firmware_root}/tools/check_gateway_trace.py" "${output_dir}/go-trace.json"
