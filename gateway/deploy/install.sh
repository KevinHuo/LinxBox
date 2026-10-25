#!/usr/bin/env bash
# 发布包根目录执行；Ubuntu 24.04 首次安装或更新，保留已有配置和数据库。
set -euo pipefail

usage() {
  echo '用法：sudo bash install.sh [--dynamic-mcp]'
  echo '无需指定 MAC，固件连接后通过 device.hello 自动登记。'
  echo '已有配置和数据库保留；新装生成独立账号接入凭据。'
  echo '--dynamic-mcp：依据旧设备登记，将已有 MCP token 显式转换为同账号多设备授权。'
}

if [[ "${1:-}" == '-h' || "${1:-}" == '--help' ]]; then
  usage
  exit 0
fi
LINX_DYNAMIC_MCP=false
if [[ $# -eq 1 && "$1" == '--dynamic-mcp' ]]; then
  LINX_DYNAMIC_MCP=true
elif [[ $# -gt 0 ]]; then
  usage
  exit 2
fi
if [[ $EUID -ne 0 ]]; then
  echo '请使用 sudo bash install.sh 执行。' >&2
  exit 1
fi
if ! command -v python3 >/dev/null || ! command -v systemctl >/dev/null; then
  echo '需要 Python 3 和 systemd；Ubuntu 可先执行 sudo apt install -y python3。' >&2
  exit 1
fi

LINX_INSTALL_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
LINX_EXPECTED_ARCH=$(sed -n 's/^GOARCH=//p' "$LINX_INSTALL_DIR/VERSION")
case "$(uname -m)" in
  x86_64) LINX_ACTUAL_ARCH=amd64 ;;
  aarch64|arm64) LINX_ACTUAL_ARCH=arm64 ;;
  *) echo '不支持当前 CPU 架构。' >&2; exit 1 ;;
esac
if [[ "$LINX_ACTUAL_ARCH" != "$LINX_EXPECTED_ARCH" ]]; then
  echo "安装包架构 $LINX_EXPECTED_ARCH 与服务器 $LINX_ACTUAL_ARCH 不匹配。" >&2
  exit 1
fi
(cd "$LINX_INSTALL_DIR" && sha256sum -c SHA256SUMS)

if ! id linxbox >/dev/null 2>&1; then
  useradd --system --user-group --no-create-home --shell /usr/sbin/nologin linxbox
fi
install -d -o root -g root -m 0755 /opt/linxbox/bin
install -d -o root -g linxbox -m 0750 /etc/linxbox
install -d -o linxbox -g linxbox -m 0750 /var/lib/linxbox
chown -R linxbox:linxbox /var/lib/linxbox

if [[ -e /etc/linxbox/gateway.json ]]; then
  echo '保留已有配置 /etc/linxbox/gateway.json；请核对监听地址和 TLS 设置。'
else
  python3 - "$LINX_INSTALL_DIR/config.example.json" <<'PY'
import json
import os
import secrets
import sys
cfg = json.load(open(sys.argv[1]))
cfg['listen'] = {'addr': '0.0.0.0:8787', 'tls_cert_file': '', 'tls_key_file': ''}
cfg['database']['path'] = '/var/lib/linxbox/gateway.db'
cfg['credentials'] = {
    'devices': [{'token': secrets.token_hex(24), 'subject': 'ws-integration'}],
    'desktops': [{'token': secrets.token_hex(24), 'client_id': 'desktop-ws-integration', 'subject': 'ws-integration'}],
}
cfg.pop('pairing', None)
fd = os.open('/etc/linxbox/gateway.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'w') as output:
    json.dump(cfg, output, ensure_ascii=False, indent=2)
    output.write('\n')
PY
  echo '已生成 WS 配置及独立随机凭据；未输出 token。'
fi
chown root:linxbox /etc/linxbox/gateway.json
chmod 0640 /etc/linxbox/gateway.json

if systemctl is-active --quiet linxbox-gateway.service; then
  systemctl stop linxbox-gateway.service
fi
if [[ "$LINX_DYNAMIC_MCP" == true ]]; then
  # 同一文件系统内生成受限临时配置，校验成功后原子替换；不直接覆写原文件。
  LINX_MCP_TEMP_DIR=$(mktemp -d /etc/linxbox/.mcp-upgrade.XXXXXX)
  trap 'rm -rf -- "$LINX_MCP_TEMP_DIR"' EXIT
  (cd /opt/linxbox && "$LINX_INSTALL_DIR/linxbox-gateway" \
    --config /etc/linxbox/gateway.json \
    --write-dynamic-mcp-config "$LINX_MCP_TEMP_DIR/gateway.json")
  chown root:linxbox "$LINX_MCP_TEMP_DIR/gateway.json"
  chmod 0640 "$LINX_MCP_TEMP_DIR/gateway.json"
  mv -- "$LINX_MCP_TEMP_DIR/gateway.json" /etc/linxbox/gateway.json
  rmdir -- "$LINX_MCP_TEMP_DIR"
  trap - EXIT
  echo '已有 MCP 凭据已改为各自登记账号范围；token 和 agent_id 保持不变。'
fi
install -o root -g root -m 0755 "$LINX_INSTALL_DIR/linxbox-gateway" /opt/linxbox/bin/linxbox-gateway
install -o root -g root -m 0755 "$LINX_INSTALL_DIR/linxbox-smoke" /opt/linxbox/bin/linxbox-smoke
install -o root -g root -m 0644 "$LINX_INSTALL_DIR/linxbox-gateway.service" /etc/systemd/system/linxbox-gateway.service
systemctl daemon-reload
systemctl enable --now linxbox-gateway.service
systemctl --no-pager --full status linxbox-gateway.service
echo '安装完成。请按 README.md 检查 readyz 并执行 smoke，确认业务就绪。'
