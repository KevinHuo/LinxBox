#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
contract_python="${LINX_CONTRACT_PYTHON:-python3}"
output_dir="${LINX_GATEWAY_TEST_OUTPUT:-${repo_root}/build/gateway-mock}"
if ! "${contract_python}" -c 'import jsonschema, referencing' >/dev/null 2>&1; then
    echo "请在独立虚拟环境安装 contracts/requirements.txt，并设置 LINX_CONTRACT_PYTHON。" >&2
    exit 1
fi
cd "${repo_root}"
if [[ $# == 0 ]]; then
    ./tools/validate_linx_host.sh
    host_binary="${LINX_HOST_BUILD_DIR:-${repo_root}/build/linx-host}/linx_gateway_host"
else
    host_binary="$1"
fi
if [[ ! -x "${host_binary}" ]]; then
    echo "缺少真实 LVGL Gateway host：${host_binary}" >&2
    exit 1
fi
mkdir -p "${output_dir}"
LINX_GATEWAY_HOST_BINARY="${host_binary}" LINX_GATEWAY_TEST_OUTPUT="${output_dir}" \
    node --test --experimental-test-coverage --test-coverage-lines=90 \
        --test-coverage-branches=90 --test-coverage-functions=90 tests/test_gateway_socket.mjs
"${contract_python}" tools/check_gateway_trace.py "${output_dir}/trace.json"
PYTHONDONTWRITEBYTECODE=1 "${contract_python}" -m coverage run --branch \
    --include='*/tools/generate_gateway_contract.py,*/tools/check_gateway_trace.py' \
    --data-file="${output_dir}/python.coverage" \
    -m unittest discover -s tests -p 'test_gateway_*.py'
"${contract_python}" -m coverage report --data-file="${output_dir}/python.coverage" --fail-under=90
