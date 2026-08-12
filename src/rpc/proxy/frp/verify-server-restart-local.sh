#!/usr/bin/env bash
set -euo pipefail

# 服务端重启场景验证：
#   1. 基线：注册/订阅 + 中继 echo 可用
#   2. kill -9 服务端 → 客户端（provider/accessor）必须在限时内检测到断开
#   3. 同端口重启服务端 → 客户端自动重连、重新注册/订阅
#   4. 中继 echo 再次可用
#
# 该脚本是对"信令传输死亡 → 立即触发重连"修复的回归测试：
# 修复前断开检测只靠 ping/pong 超时（约 120s），步骤 2 的 10s 限时必然失败。

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-linux}"

SERVER_BIN="${BUILD_DIR}/applications/frp_proxy_server/frp_proxy_server"
CLIENT_BIN="${BUILD_DIR}/applications/frp_proxy_client/frp_proxy_client"
ECHO_BIN="${BUILD_DIR}/applications/frp_echo_test/frp_echo_test"

for bin in "${SERVER_BIN}" "${CLIENT_BIN}" "${ECHO_BIN}"; do
    if [[ ! -x "${bin}" ]]; then
        echo "missing executable: ${bin} (build first?)" >&2
        exit 1
    fi
done

WORK_DIR="$(mktemp -d /tmp/frp-verify-restart-XXXXXX)"
SERVER_TCP_PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")
BACKEND_PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")
ACCESSOR_PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()")

SERVER_PID=""
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

cat > "${WORK_DIR}/config/public_server.json" <<EOF
{
  "threads": 2,
  "listen_tcp_port": ${SERVER_TCP_PORT},
  "listen_udp_port": 0,
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
  "public_server_udp_port": 0,
  "traffic_secret": "traffic-secret-demo",
  "nat_type": 0,
  "ssl": { "disable_ssl": true },
  "groups": [
    {
      "register_key": "demo-register-key",
      "services": [
        {
          "service_name": "demo-echo",
          "target_host": "127.0.0.1",
          "target_port": ${BACKEND_PORT}
        }
      ]
    }
  ]
}
EOF

cat > "${WORK_DIR}/config/accessor.json" <<EOF
{
  "threads": 2,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": ${SERVER_TCP_PORT},
  "public_server_udp_port": 0,
  "traffic_secret": "traffic-secret-demo",
  "nat_type": 0,
  "ssl": { "disable_ssl": true },
  "groups": [
    {
      "register_key": "demo-register-key",
      "listeners": [
        {
          "service_name": "demo-echo",
          "listen_host": "127.0.0.1",
          "listen_port": ${ACCESSOR_PORT}
        }
      ]
    }
  ]
}
EOF

echo "========================================"
echo "  FRP Server Restart Verification"
echo "========================================"
echo "work dir : ${WORK_DIR}"
echo "server   : TCP ${SERVER_TCP_PORT}"
echo "backend  : ${BACKEND_PORT}"
echo "accessor : ${ACCESSOR_PORT}"
echo ""

# 工具函数：等待文件中新出现的模式（从 offset 行之后开始找）
wait_tail() { # file offset pattern timeout_sec
    local file=$1 offset=$2 pattern=$3 timeout=$4
    local deadline=$(( $(date +%s) + timeout ))
    while (( $(date +%s) < deadline )); do
        if sed -n "$(( offset + 1 )),\$p" "${file}" 2>/dev/null | grep -q "${pattern}"; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

wait_log() { # file pattern timeout_sec
    wait_tail "$1" 0 "$2" "$3"
}

count_lines() { wc -l < "$1"; }

run_echo() { # label
    local label=$1
    if "${ECHO_BIN}" --mode client --host 127.0.0.1 --port "${ACCESSOR_PORT}" \
            --count 5 --delay 100 >"${WORK_DIR}/echo_client_${label}.log" 2>&1 \
        && grep -q "\[TEST PASSED\]" "${WORK_DIR}/echo_client_${label}.log"; then
        echo "✅ PASSED: relay path works (${label})"
        return 0
    fi
    echo "❌ FAILED: echo test did not pass (${label})"
    cat "${WORK_DIR}/echo_client_${label}.log"
    return 1
}

# 1. echo backend
echo "[1/7] starting echo backend on ${BACKEND_PORT}..."
"${ECHO_BIN}" --mode server --port "${BACKEND_PORT}" >"${WORK_DIR}/backend.log" 2>&1 &
PIDS+=("$!")
sleep 1

# 2. public_server
echo "[2/7] starting public_server on TCP ${SERVER_TCP_PORT}..."
"${SERVER_BIN}" --config "${WORK_DIR}/config/public_server.json" >"${WORK_DIR}/server.log" 2>&1 &
SERVER_PID=$!
PIDS+=("${SERVER_PID}")
sleep 1

# 3. provider
echo "[3/7] starting provider..."
"${CLIENT_BIN}" --config "${WORK_DIR}/config/provider.json" >"${WORK_DIR}/provider.log" 2>&1 &
PIDS+=("$!")
sleep 1

# 4. accessor
echo "[4/7] starting accessor on ${ACCESSOR_PORT}..."
"${CLIENT_BIN}" --config "${WORK_DIR}/config/accessor.json" >"${WORK_DIR}/accessor.log" 2>&1 &
PIDS+=("$!")
sleep 2

# 5. 基线：provider 注册成功 + accessor 已连上
echo "[5/7] waiting for baseline registration..."
if ! wait_log "${WORK_DIR}/provider.log" "register ok" 15; then
    echo "❌ FAILED: provider did not register (baseline)"
    cat "${WORK_DIR}/provider.log"
    exit 1
fi
if ! wait_log "${WORK_DIR}/accessor.log" "signal connected" 15; then
    echo "❌ FAILED: accessor did not connect (baseline)"
    cat "${WORK_DIR}/accessor.log"
    exit 1
fi

if ! run_echo "baseline"; then
    echo "=== server.log ==="; cat "${WORK_DIR}/server.log"
    echo "=== provider.log ==="; cat "${WORK_DIR}/provider.log"
    echo "=== accessor.log ==="; cat "${WORK_DIR}/accessor.log"
    exit 1
fi

# 6. kill -9 服务端：客户端必须在限时内检测到断开
echo "[6/7] killing public_server (kill -9) ..."
PROV_OFFSET=$(count_lines "${WORK_DIR}/provider.log")
ACCESS_OFFSET=$(count_lines "${WORK_DIR}/accessor.log")
DETECT_START=$(date +%s)

kill -9 "${SERVER_PID}" 2>/dev/null || true
for _ in $(seq 1 50); do
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then break; fi
    sleep 0.1
done

if ! wait_tail "${WORK_DIR}/provider.log" "${PROV_OFFSET}" "signal disconnected" 10; then
    echo "❌ FAILED: provider did NOT detect disconnect within 10s (regression: ping/pong fallback only)"
    tail -n 20 "${WORK_DIR}/provider.log"
    exit 1
fi
if ! wait_tail "${WORK_DIR}/accessor.log" "${ACCESS_OFFSET}" "signal disconnected" 10; then
    echo "❌ FAILED: accessor did NOT detect disconnect within 10s (regression: ping/pong fallback only)"
    tail -n 20 "${WORK_DIR}/accessor.log"
    exit 1
fi
DETECT_ELAPSED=$(( $(date +%s) - DETECT_START ))
echo "✅ PASSED: both clients detected disconnect within ${DETECT_ELAPSED}s"

# 7. 同端口重启服务端：客户端自动重连 + 重新注册/订阅
echo "[7/7] restarting public_server on same port ..."
sleep 1
"${SERVER_BIN}" --config "${WORK_DIR}/config/public_server.json" >"${WORK_DIR}/server2.log" 2>&1 &
SERVER_PID=$!
PIDS+=("${SERVER_PID}")

RECOVER_START=$(date +%s)
if ! wait_tail "${WORK_DIR}/provider.log" "${PROV_OFFSET}" "register ok" 20; then
    echo "❌ FAILED: provider did NOT reconnect + re-register within 20s"
    tail -n 30 "${WORK_DIR}/provider.log"
    exit 1
fi
if ! wait_tail "${WORK_DIR}/accessor.log" "${ACCESS_OFFSET}" "signal connected" 20; then
    echo "❌ FAILED: accessor did NOT reconnect within 20s"
    tail -n 30 "${WORK_DIR}/accessor.log"
    exit 1
fi
RECOVER_ELAPSED=$(( $(date +%s) - RECOVER_START ))
echo "✅ PASSED: both clients reconnected + re-registered within ${RECOVER_ELAPSED}s"

# 恢复后中继必须再次可用。accessor 可能先拿到 0 服务的快照、稍后经快速重试
# 重新订阅并恢复本地监听（最多 1-2s），因此 echo 带重试吸收该窗口。
AFTER_RESTART_OK=0
for attempt in $(seq 1 8); do
    if "${ECHO_BIN}" --mode client --host 127.0.0.1 --port "${ACCESSOR_PORT}" \
            --count 5 --delay 100 >"${WORK_DIR}/echo_client_after-restart.log" 2>&1 \
        && grep -q "\[TEST PASSED\]" "${WORK_DIR}/echo_client_after-restart.log"; then
        AFTER_RESTART_OK=1
        break
    fi
    sleep 1
done
if [[ "${AFTER_RESTART_OK}" == "1" ]]; then
    echo "✅ PASSED: relay path works (after-restart)"
else
    echo "❌ FAILED: echo test did not pass (after-restart)"
    cat "${WORK_DIR}/echo_client_after-restart.log"
    echo "=== server2.log ==="; cat "${WORK_DIR}/server2.log"
    echo "=== provider.log (tail) ==="; tail -n 30 "${WORK_DIR}/provider.log"
    echo "=== accessor.log (tail) ==="; tail -n 30 "${WORK_DIR}/accessor.log"
    exit 1
fi

echo ""
echo "========================================"
echo "  ALL PASSED (detect=${DETECT_ELAPSED}s, recover=${RECOVER_ELAPSED}s)"
echo "========================================"
echo "--- provider.log (tail) ---"
tail -n 8 "${WORK_DIR}/provider.log"
echo "--- accessor.log (tail) ---"
tail -n 8 "${WORK_DIR}/accessor.log"
exit 0
