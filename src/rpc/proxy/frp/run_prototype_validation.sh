#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== FRP prototype validation ==="
echo

echo "[1/2] relay local validation"
"${SCRIPT_DIR}/verify-relay-local.sh"
echo

echo "[2/2] p2p local validation"
"${SCRIPT_DIR}/verify-p2p-local.sh"
echo

echo "prototype validation passed"
