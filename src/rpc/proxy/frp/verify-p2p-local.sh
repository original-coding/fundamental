#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-linux}"

SERVER_BIN="${BUILD_DIR}/applications/frp_proxy_server/frp_proxy_server"
PROVIDER_BIN="${BUILD_DIR}/applications/frp_proxy_client/frp_proxy_client"
ACCESSOR_BIN="${BUILD_DIR}/applications/frp_proxy_accessor/frp_proxy_accessor"
ECHO_BIN="${BUILD_DIR}/applications/frp_echo_test/frp_echo_test"

for bin in "${SERVER_BIN}" "${PROVIDER_BIN}" "${ACCESSOR_BIN}" "${ECHO_BIN}"; do
    if [[ ! -x "${bin}" ]]; then
        echo "missing executable: ${bin}" >&2
        exit 1
    fi
done

WORK_DIR="$(mktemp -d /tmp/frp-verify-p2p-XXXXXX)"
SERVER_TCP_PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")
SERVER_UDP_PORT=$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")
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

mkdir -p "${WORK_DIR}/config"

# Server config: p2p enabled (listen_udp_port non-zero)
cat > "${WORK_DIR}/config/public_server.json" <<EOF
{
  "threads": 2,
  "listen_tcp_port": ${SERVER_TCP_PORT},
  "listen_udp_port": ${SERVER_UDP_PORT},
  "traffic_secret": "traffic-secret-demo",
  "allowed_register_keys": ["demo-register-key"],
  "ssl": { "disable_ssl": true }
}
EOF

# Provider config: full-cone NAT (nat_type=2), p2p enabled
cat > "${WORK_DIR}/config/provider.json" <<EOF
{
  "threads": 2,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": ${SERVER_TCP_PORT},
  "public_server_udp_port": ${SERVER_UDP_PORT},
  "traffic_secret": "traffic-secret-demo",
  "register_key": "demo-register-key",
  "nat_type": 2,
  "ssl": { "disable_ssl": true },
  "services": [
    {
      "service_name": "demo-echo",
      "target_host": "127.0.0.1",
      "target_port": ${BACKEND_PORT}
    }
  ]
}
EOF

# Accessor config: full-cone NAT (nat_type=2), p2p enabled
cat > "${WORK_DIR}/config/accessor.json" <<EOF
{
  "threads": 2,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": ${SERVER_TCP_PORT},
  "public_server_udp_port": ${SERVER_UDP_PORT},
  "traffic_secret": "traffic-secret-demo",
  "register_key": "demo-register-key",
  "nat_type": 2,
  "ssl": { "disable_ssl": true },
  "listeners": [
    {
      "service_name": "demo-echo",
      "listen_host": "127.0.0.1",
      "listen_port": ${ACCESSOR_PORT},
      "nat_type": 2
    }
  ]
}
EOF

echo "========================================"
echo "  FRP P2P Upgrade Local Verification"
echo "========================================"
echo "work dir : ${WORK_DIR}"
echo "server   : TCP ${SERVER_TCP_PORT}  UDP ${SERVER_UDP_PORT}"
echo "backend  : ${BACKEND_PORT}"
echo "accessor : ${ACCESSOR_PORT}"
echo ""

# 1. echo backend
echo "[1/4] starting echo backend on ${BACKEND_PORT}..."
"${ECHO_BIN}" --mode server --port "${BACKEND_PORT}" >"${WORK_DIR}/backend.log" 2>&1 &
PIDS+=("$!")
sleep 1

# 2. public_server
echo "[2/4] starting public_server on TCP ${SERVER_TCP_PORT} UDP ${SERVER_UDP_PORT}..."
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

# Allow extra time for relay to establish
sleep 3

echo ""
echo "[warmup] triggering first connection to start p2p upgrade..."
if "${ECHO_BIN}" --mode client --host 127.0.0.1 --port "${ACCESSOR_PORT}" \
        --count 3 --delay 100 >"${WORK_DIR}/warmup.log" 2>&1; then
    echo "   warmup connection ok"
else
    echo "   warmup connection failed (ignoring)"
fi

# Wait for p2p upgrade to complete
sleep 3

echo ""
echo "[test] running echo client against accessor ${ACCESSOR_PORT} ..."

for i in $(seq 1 30); do
    if "${ECHO_BIN}" --mode client --host 127.0.0.1 --port "${ACCESSOR_PORT}" \
            --count 20 --delay 500 >"${WORK_DIR}/echo_client.log" 2>&1; then
        if grep -q "\[TEST PASSED\]" "${WORK_DIR}/echo_client.log"; then
            echo "✅ PASSED: data path works"
            echo ""

            # Check log evidence for p2p upgrade
            P2P_OK=false
            if grep -q "switched to p2p" "${WORK_DIR}/provider.log" 2>/dev/null || \
               grep -q "switched to p2p" "${WORK_DIR}/accessor.log" 2>/dev/null; then
                P2P_OK=true
            fi
            if [[ "${P2P_OK}" == "true" ]]; then
                echo "   p2p upgrade          : confirmed"
            else
                echo "   p2p upgrade          : not observed"
            fi
            echo ""
            echo "=== P2P evidence ==="
            echo "--- udp_punch succeeded ---"
            grep "udp_punch succeeded\|switched to p2p\|punch confirmed" "${WORK_DIR}/provider.log" "${WORK_DIR}/accessor.log" || echo "   (none)"
            echo ""
            echo "=== tail logs ==="
            echo "--- server.log (last 8 lines) ---"
            tail -n 8 "${WORK_DIR}/server.log"
            echo "--- provider.log (last 8 lines) ---"
            tail -n 8 "${WORK_DIR}/provider.log"
            echo "--- accessor.log (last 8 lines) ---"
            tail -n 8 "${WORK_DIR}/accessor.log"
            exit 0
        fi
    fi
    sleep 1
done

echo "❌ FAILED: echo test did not pass"
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
