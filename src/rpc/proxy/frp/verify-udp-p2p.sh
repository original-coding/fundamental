#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-linux}"

SERVER_BIN="${BUILD_DIR}/applications/frp_proxy_server/frp_proxy_server"
CLIENT_BIN="${BUILD_DIR}/applications/frp_proxy_client/frp_proxy_client"
ECHO_BIN="${BUILD_DIR}/applications/frp_echo_test/frp_echo_test"

for bin in "${SERVER_BIN}" "${CLIENT_BIN}" "${ECHO_BIN}"; do
    if [[ ! -x "${bin}" ]]; then
        echo "missing executable: ${bin}" >&2
        exit 1
    fi
done

WORK_DIR="$(mktemp -d /tmp/frp-verify-udp-p2p-XXXXXX)"
SERVER_TCP_PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")
SERVER_UDP_PORT=$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")
BACKEND_PORT=$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")
ACCESSOR_PORT=$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")

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

mkdir -p "${WORK_DIR}/config"

# Server: P2P enabled (listen_udp_port non-zero)
cat > "${WORK_DIR}/config/public_server.json" <<EOF
{
  "threads": 2,
  "listen_tcp_port": ${SERVER_TCP_PORT},
  "listen_udp_port": ${SERVER_UDP_PORT},
  "traffic_secret": "traffic-secret-demo",
  "allowed_register_keys": ["k1"],
  "ssl": { "disable_ssl": true }
}
EOF

# Provider: UDP service, cone NAT, P2P enabled
cat > "${WORK_DIR}/config/provider.json" <<EOF
{
  "threads": 2,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": ${SERVER_TCP_PORT},
  "public_server_udp_port": ${SERVER_UDP_PORT},
  "traffic_secret": "traffic-secret-demo",
  "nat_type": 2,
  "ssl": { "disable_ssl": true },
  "groups": [
    {
      "register_key": "k1",
      "services": [
    {
      "service_name": "echo-udp",
      "service_type": 1,
      "target_host": "127.0.0.1",
      "target_port": ${BACKEND_PORT},
      "enable_p2p": true
    }
  ]
    }
  ]
}
EOF

# Accessor: UDP listener, cone NAT, P2P enabled
cat > "${WORK_DIR}/config/accessor.json" <<EOF
{
  "threads": 2,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": ${SERVER_TCP_PORT},
  "public_server_udp_port": ${SERVER_UDP_PORT},
  "traffic_secret": "traffic-secret-demo",
  "nat_type": 2,
  "ssl": { "disable_ssl": true },
  "groups": [
    {
      "register_key": "k1",
      "listeners": [
    {
      "service_name": "echo-udp",
      "service_type": 1,
      "listen_host": "127.0.0.1",
      "listen_port": ${ACCESSOR_PORT},
      "enable_p2p": true
    }
  ]
    }
  ]
}
EOF

echo "========================================"
echo "  FRP UDP Proxy P2P Local Verification"
echo "========================================"
echo "work dir : ${WORK_DIR}"
echo "server   : TCP ${SERVER_TCP_PORT}  UDP ${SERVER_UDP_PORT}"
echo "backend  : UDP ${BACKEND_PORT}"
echo "accessor : UDP ${ACCESSOR_PORT}"
echo ""

# 1. UDP echo backend
echo "[1/4] starting UDP echo backend on ${BACKEND_PORT}..."
"${ECHO_BIN}" --mode udp-server --port "${BACKEND_PORT}" >"${WORK_DIR}/backend.log" 2>&1 &
PIDS+=("$!")
sleep 1

# 2. public_server
echo "[2/4] starting public_server on TCP ${SERVER_TCP_PORT} UDP ${SERVER_UDP_PORT}..."
"${SERVER_BIN}" --config "${WORK_DIR}/config/public_server.json" >"${WORK_DIR}/server.log" 2>&1 &
PIDS+=("$!")
sleep 1

# 3. provider
echo "[3/4] starting provider..."
"${CLIENT_BIN}" --config "${WORK_DIR}/config/provider.json" >"${WORK_DIR}/provider.log" 2>&1 &
PIDS+=("$!")
sleep 1

# 4. accessor
echo "[4/4] starting accessor on UDP ${ACCESSOR_PORT}..."
"${CLIENT_BIN}" --config "${WORK_DIR}/config/accessor.json" >"${WORK_DIR}/accessor.log" 2>&1 &
PIDS+=("$!")

# Allow time for relay + P2P upgrade
sleep 4

echo ""
echo "[test] running UDP echo client against accessor ${ACCESSOR_PORT} ..."
"${ECHO_BIN}" --mode udp-client --host 127.0.0.1 --port "${ACCESSOR_PORT}" \
        --count 10 --delay 200 >"${WORK_DIR}/echo_client.log" 2>&1

PASSED=false
P2P_OK=false

if grep -q '\[TEST PASSED\]' "${WORK_DIR}/echo_client.log"; then
    PASSED=true
fi

if grep -q "switched to p2p" "${WORK_DIR}/provider.log" 2>/dev/null || \
   grep -q "switched to p2p" "${WORK_DIR}/accessor.log" 2>/dev/null; then
    P2P_OK=true
fi

if [[ "${PASSED}" == "true" ]] && [[ "${P2P_OK}" == "true" ]]; then
    echo "✅ PASSED: UDP proxy P2P upgrade works"
    echo ""
    echo "   data path            : ok"
    echo "   p2p upgrade          : confirmed"
    echo ""
    echo "=== P2P evidence ==="
    grep "switched to p2p" "${WORK_DIR}/provider.log" "${WORK_DIR}/accessor.log"
    echo ""
    echo "=== tail logs ==="
    echo "--- server.log (last 4 lines) ---"
    tail -4 "${WORK_DIR}/server.log"
    echo "--- provider.log (last 4 lines) ---"
    tail -4 "${WORK_DIR}/provider.log"
    echo "--- accessor.log (last 4 lines) ---"
    tail -4 "${WORK_DIR}/accessor.log"
    exit 0
fi

echo "❌ FAILED: UDP proxy P2P test did not pass"
echo "   data path            : $([[ "${PASSED}" == "true" ]] && echo 'ok' || echo 'BROKEN')"
echo "   p2p upgrade          : $([[ "${P2P_OK}" == "true" ]] && echo 'confirmed' || echo 'not observed')"
echo ""
echo "=== echo_client.log ==="
cat "${WORK_DIR}/echo_client.log"
echo "=== server.log ==="
cat "${WORK_DIR}/server.log"
echo "=== provider.log ==="
cat "${WORK_DIR}/provider.log"
echo "=== accessor.log ==="
cat "${WORK_DIR}/accessor.log"
exit 1
