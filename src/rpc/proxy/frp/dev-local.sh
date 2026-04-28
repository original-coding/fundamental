#!/usr/bin/env bash
# dev-local.sh — 本地开发用启动脚本
# 代理 127.0.0.1:50051，accessor 监听 127.0.0.1:15051
# 用法: bash src/rpc/proxy/frp/dev-local.sh [stop]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-linux}"

SERVER_BIN="${BUILD_DIR}/applications/frp_proxy_server/frp_proxy_server"
PROVIDER_BIN="${BUILD_DIR}/applications/frp_proxy_client/frp_proxy_client"
ACCESSOR_BIN="${BUILD_DIR}/applications/frp_proxy_accessor/frp_proxy_accessor"

WORK_DIR="${REPO_ROOT}/.dev-local-frp"
PID_FILE="${WORK_DIR}/pids"

# ── stop ──────────────────────────────────────────────────────────────────────
if [[ "${1:-}" == "stop" ]]; then
    if [[ -f "${PID_FILE}" ]]; then
        while IFS= read -r pid; do
            kill -TERM "${pid}" 2>/dev/null || true
        done < "${PID_FILE}"
        sleep 1
        while IFS= read -r pid; do
            kill -KILL "${pid}" 2>/dev/null || true
        done < "${PID_FILE}"
        rm -f "${PID_FILE}"
        echo "[dev-local] stopped"
    else
        echo "[dev-local] not running"
    fi
    exit 0
fi

# ── check binaries ────────────────────────────────────────────────────────────
for bin in "${SERVER_BIN}" "${PROVIDER_BIN}" "${ACCESSOR_BIN}"; do
    if [[ ! -x "${bin}" ]]; then
        echo "missing executable: ${bin}" >&2
        exit 1
    fi
done

# ── ports ─────────────────────────────────────────────────────────────────────
SERVER_TCP_PORT=32100
SERVER_UDP_PORT=32101
ACCESSOR_PORT=15051
BACKEND_PORT=50051
LOCAL_IP="127.0.0.1"

mkdir -p "${WORK_DIR}/config" "${WORK_DIR}/logs"

# ── configs ───────────────────────────────────────────────────────────────────
cat > "${WORK_DIR}/config/server.json" <<EOF
{
  "threads": 2,
  "listen_tcp_port": ${SERVER_TCP_PORT},
  "listen_udp_port": ${SERVER_UDP_PORT},
  "traffic_secret": "dev-secret",
  "allowed_register_keys": ["dev-key"],
  "ssl": { "disable_ssl": true }
}
EOF

cat > "${WORK_DIR}/config/provider.json" <<EOF
{
  "threads": 2,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": ${SERVER_TCP_PORT},
  "public_server_udp_port": ${SERVER_UDP_PORT},
  "traffic_secret": "dev-secret",
  "register_key": "dev-key",
  "enable_p2p": true,
  "local_ip": "${LOCAL_IP}",
  "ssl": { "disable_ssl": true },
  "services": [
    {
      "service_name": "grpc-backend",
      "target_host": "127.0.0.1",
      "target_port": ${BACKEND_PORT}
    }
  ]
}
EOF

cat > "${WORK_DIR}/config/accessor.json" <<EOF
{
  "threads": 2,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": ${SERVER_TCP_PORT},
  "public_server_udp_port": ${SERVER_UDP_PORT},
  "traffic_secret": "dev-secret",
  "register_key": "dev-key",
  "enable_p2p": true,
  "local_ip": "${LOCAL_IP}",
  "ssl": { "disable_ssl": true },
  "listeners": [
    {
      "service_name": "grpc-backend",
      "listen_host": "127.0.0.1",
      "listen_port": ${ACCESSOR_PORT},
      "enable_p2p": true
    }
  ]
}
EOF

# ── start ─────────────────────────────────────────────────────────────────────
echo "========================================"
echo "  FRP dev-local"
echo "========================================"
echo "  server   : TCP ${SERVER_TCP_PORT}  UDP ${SERVER_UDP_PORT}"
echo "  provider -> 127.0.0.1:${BACKEND_PORT}"
echo "  accessor -> 127.0.0.1:${ACCESSOR_PORT}"
echo "  logs     : ${WORK_DIR}/logs/"
echo "  stop     : bash $0 stop"
echo "========================================"

> "${PID_FILE}"

"${SERVER_BIN}"   --config "${WORK_DIR}/config/server.json"   > "${WORK_DIR}/logs/server.log"   2>&1 & echo $! >> "${PID_FILE}"
sleep 0.5
"${PROVIDER_BIN}" --config "${WORK_DIR}/config/provider.json" > "${WORK_DIR}/logs/provider.log" 2>&1 & echo $! >> "${PID_FILE}"
sleep 0.5
"${ACCESSOR_BIN}" --config "${WORK_DIR}/config/accessor.json" > "${WORK_DIR}/logs/accessor.log" 2>&1 & echo $! >> "${PID_FILE}"

echo ""
echo "[dev-local] started (pids: $(tr '\n' ' ' < "${PID_FILE}"))"
echo "[dev-local] connect your client to 127.0.0.1:${ACCESSOR_PORT}"
