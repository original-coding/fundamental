#!/usr/bin/env bash
# upload.sh — upload a file to TestHttpServer
# usage: bash upload.sh <file> [host:port] [remote_name]
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <file> [host:port] [remote_name]" >&2
    exit 1
fi

FILE="$1"
HOST="${2:-127.0.0.1:9000}"
REMOTE="${3:-$(basename "${FILE}")}"

if [[ ! -f "${FILE}" ]]; then
    echo "file not found: ${FILE}" >&2
    exit 1
fi

curl -fS -T "${FILE}" "http://${HOST}/upload?path=${REMOTE}"
echo ""
