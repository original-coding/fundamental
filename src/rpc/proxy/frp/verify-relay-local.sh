#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-linux}"

SERVER_BIN="${BUILD_DIR}/applications/frp_proxy_server/frp_proxy_server"
PROVIDER_BIN="${BUILD_DIR}/applications/frp_proxy_client/frp_proxy_client"
ACCESSOR_BIN="${BUILD_DIR}/applications/frp_proxy_accessor/frp_proxy_accessor"
BACKEND_BIN="${BUILD_DIR}/applications/frp_test_http_backend/frp_test_http_backend"

for bin in "${SERVER_BIN}" "${PROVIDER_BIN}" "${ACCESSOR_BIN}" "${BACKEND_BIN}"; do
    if [[ ! -x "${bin}" ]]; then
        echo "missing executable: ${bin}" >&2
        exit 1
    fi
done

WORK_DIR="$(mktemp -d /tmp/frp-verify-relay-XXXXXX)"
SERVER_TCP_PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")
BACKEND_PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")
ACCESSOR_PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")

PIDS=()
cleanup() {
    echo "[verify] cleaning up..."
    for pid in "${PIDS[@]+${PIDS[@]}}"; do
        kill -TERM "${pid}" 2>/dev/null || true
    done
    sleep 1
    for pid in "${PIDS[@]+${PIDS[@]}}"; do
        kill -KILL "${pid}" 2>/dev/null || true
    done
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

mkdir -p "${WORK_DIR}/config" "${WORK_DIR}/backend"
echo "relay-ok" > "${WORK_DIR}/backend/index.html"

cat > "${WORK_DIR}/config/public_server.json" <<EOF
{
  "threads": 2,
  "listen_tcp_port": ${SERVER_TCP_PORT},
  "listen_udp_ports": [],
  "traffic_secret": "traffic-secret-demo",
  "allowed_register_keys": ["demo-register-key"],
  "ssl": { "disable_ssl": true }
}
EOF

cat > "${WORK_DIR}/config/provider.json" <<EOF
{
  "threads": 2,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": ${SERVER_TCP_PORT},
  "public_server_udp_ports": [],
  "traffic_secret": "traffic-secret-demo",
  "register_key": "demo-register-key",
  "enable_p2p": false,
  "ssl": { "disable_ssl": true },
  "services": [
    {
      "service_name": "demo-web",
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
  "public_server_udp_ports": [],
  "traffic_secret": "traffic-secret-demo",
  "register_key": "demo-register-key",
  "ssl": { "disable_ssl": true },
  "listeners": [
    {
      "service_name": "demo-web",
      "listen_host": "127.0.0.1",
      "listen_port": ${ACCESSOR_PORT},
      "enable_p2p": false
    }
  ]
}
EOF

echo "========================================"
echo "  FRP Relay Local Verification"
echo "========================================"
echo "work dir : ${WORK_DIR}"
echo "server   : TCP ${SERVER_TCP_PORT}"
echo "backend  : ${BACKEND_PORT}"
echo "accessor : ${ACCESSOR_PORT}"
echo ""

# 1. backend
echo "[1/4] starting backend on ${BACKEND_PORT}..."
"${BACKEND_BIN}" --port "${BACKEND_PORT}" --root "${WORK_DIR}/backend" >"${WORK_DIR}/backend.log" 2>&1 &
PIDS+=("$!")
sleep 1

# 2. public_server
echo "[2/4] starting public_server on TCP ${SERVER_TCP_PORT}..."
"${SERVER_BIN}" --config "${WORK_DIR}/config/public_server.json" >"${WORK_DIR}/server.log" 2>&1 &
PIDS+=("$!")
sleep 1

# 3. provider
echo "[3/4] starting provider..."
"${PROVIDER_BIN}" --config "${WORK_DIR}/config/provider.json" >"${WORK_DIR}/provider.log" 2>&1 &
PIDS+=("$!")
sleep 1

# 4. accessor
echo "[4/4] starting accessor on ${ACCESSOR_PORT}..."
"${ACCESSOR_BIN}" --config "${WORK_DIR}/config/accessor.json" >"${WORK_DIR}/accessor.log" 2>&1 &
PIDS+=("$!")
sleep 2

echo ""
echo "[test] curling http://127.0.0.1:${ACCESSOR_PORT}/ ..."

for i in $(seq 1 30); do
    if curl -sS --max-time 2 "http://127.0.0.1:${ACCESSOR_PORT}/" >"${WORK_DIR}/curl.out" 2>/dev/null; then
        RESPONSE="$(cat "${WORK_DIR}/curl.out")"
        if [[ "${RESPONSE}" == "relay-ok" ]]; then
            echo "✅ PASSED: relay path works"
            echo "   response: ${RESPONSE}"
            echo ""
            echo "=== tail logs ==="
            echo "--- server.log (last 6 lines) ---"
            tail -n 6 "${WORK_DIR}/server.log"
            echo "--- provider.log (last 6 lines) ---"
            tail -n 6 "${WORK_DIR}/provider.log"
            echo "--- accessor.log (last 6 lines) ---"
            tail -n 6 "${WORK_DIR}/accessor.log"
            exit 0
        fi
    fi
    sleep 1
done

echo "❌ FAILED: could not get expected response"
echo ""
echo "=== server.log ==="
cat "${WORK_DIR}/server.log"
echo "=== provider.log ==="
cat "${WORK_DIR}/provider.log"
echo "=== accessor.log ==="
cat "${WORK_DIR}/accessor.log"
exit 1
