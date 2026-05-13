#!/bin/bash
set -e

BUILD_DIR="${1:-build-linux}"
DEPLOY_SUBDIR="${2:-install/frp-deploy}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEPLOY_DIR="${PROJECT_DIR}/${BUILD_DIR}/${DEPLOY_SUBDIR}"

echo "==> FRP Deploy Package"
echo "    Build dir:   ${BUILD_DIR}"
echo "    Output dir:  ${BUILD_DIR}/${DEPLOY_SUBDIR}"

# Clean and create deploy directory
rm -rf "${DEPLOY_DIR}"
mkdir -p "${DEPLOY_DIR}"

# Copy binaries
echo "==> Copying binaries..."
BINARIES=("frp_proxy_server" "frp_proxy_client" "frp_proxy_accessor")
for bin in "${BINARIES[@]}"; do
    src="${PROJECT_DIR}/${BUILD_DIR}/applications/${bin}/${bin}"
    if [ ! -f "${src}" ]; then
        echo "ERROR: ${src} not found. Please build first."
        exit 1
    fi
    cp "${src}" "${DEPLOY_DIR}/"
    chmod +x "${DEPLOY_DIR}/${bin}"
    echo "    ${bin}"
done

# Generate default configs for local testing (SSH 22 → proxy 42222, all 127.0.0.1)
echo "==> Generating default configs..."

cat > "${DEPLOY_DIR}/server.json" << 'SERVER_JSON'
{
    "threads": 8,
    "listen_tcp_port": 32000,
    "listen_udp_port": 32001,
    "traffic_secret": "dev-secret",
    "allowed_register_keys": ["dev-key"],
    "ssl": {
        "certificate_path": "local.crt",
        "private_key_path": "local.key",
        "tmp_dh_path": "",
        "ca_certificate_path": "ca_root.crt",
        "verify_client": false,
        "disable_ssl": false
    }
}
SERVER_JSON

cat > "${DEPLOY_DIR}/provider.json" << 'PROVIDER_JSON'
{
    "threads": 8,
    "public_server_host": "127.0.0.1",
    "public_server_tcp_port": 32000,
    "public_server_udp_port": 32001,
    "traffic_secret": "dev-secret",
    "register_key": "dev-key",
    "nat_type": 2,
    "local_ip": "127.0.0.1",
    "ssl": {
        "certificate_path": "local.crt",
        "private_key_path": "local.key",
        "ca_certificate_path": "ca_root.crt",
        "disable_ssl": false
    },
    "services": [
        {
            "service_name": "ssh",
            "target_host": "127.0.0.1",
            "target_port": 22,
            "enable_p2p": true
        }
    ]
}
PROVIDER_JSON

cat > "${DEPLOY_DIR}/accessor.json" << 'ACCESSOR_JSON'
{
    "threads": 8,
    "public_server_host": "127.0.0.1",
    "public_server_tcp_port": 32000,
    "public_server_udp_port": 32001,
    "traffic_secret": "dev-secret",
    "register_key": "dev-key",
    "nat_type": 2,
    "local_ip": "127.0.0.1",
    "ssl": {
        "certificate_path": "local.crt",
        "private_key_path": "local.key",
        "ca_certificate_path": "ca_root.crt",
        "disable_ssl": false
    },
    "listeners": [
        {
            "service_name": "ssh",
            "listen_host": "127.0.0.1",
            "listen_port": 42222,
            "enable_p2p": true
        }
    ]
}
ACCESSOR_JSON

echo "    server.json  provider.json  accessor.json"

# Copy certificates
echo "==> Copying certificates..."
CERT_DIR="${PROJECT_DIR}/assets"
cp "${CERT_DIR}/ca_root.crt" "${DEPLOY_DIR}/"
cp "${CERT_DIR}/local.crt"   "${DEPLOY_DIR}/"
cp "${CERT_DIR}/local.key"   "${DEPLOY_DIR}/"
chmod 600 "${DEPLOY_DIR}/local.key"
echo "    ca_root.crt  local.crt  local.key"

# Generate launch scripts
echo "==> Generating launch scripts..."

cat > "${DEPLOY_DIR}/launch_server.sh" << 'EOF'
#!/bin/bash
# FRP Public Server Launch Script
# Usage: ./launch_server.sh [config_path]

CONFIG="${1:-server.json}"
cd "$(dirname "$0")"
exec ./frp_proxy_server --config "${CONFIG}"
EOF
chmod +x "${DEPLOY_DIR}/launch_server.sh"

cat > "${DEPLOY_DIR}/launch_provider.sh" << 'EOF'
#!/bin/bash
# FRP Provider Launch Script
# Usage: ./launch_provider.sh [config_path]

CONFIG="${1:-provider.json}"
cd "$(dirname "$0")"
exec ./frp_proxy_client --config "${CONFIG}"
EOF
chmod +x "${DEPLOY_DIR}/launch_provider.sh"

cat > "${DEPLOY_DIR}/launch_accessor.sh" << 'EOF'
#!/bin/bash
# FRP Accessor Launch Script
# Usage: ./launch_accessor.sh [config_path]

CONFIG="${1:-accessor.json}"
cd "$(dirname "$0")"
exec ./frp_proxy_accessor --config "${CONFIG}"
EOF
chmod +x "${DEPLOY_DIR}/launch_accessor.sh"

echo "    launch_server.sh  launch_provider.sh  launch_accessor.sh"

# Generate README
echo "==> Generating README..."
cat > "${DEPLOY_DIR}/README.md" << 'README_EOF'
# FRP 内网穿透部署说明

## 概述

FRP (Fast Reverse Proxy) 是一个基于 TCP/UDP 的内网穿透工具，支持 TCP 中继转发和 UDP NAT 穿透（P2P）两种模式。

## 架构说明

| 角色 | 程序 | 部署位置 | 说明 |
|------|------|----------|------|
| 公网服务器 | `frp_proxy_server` | 具有公网 IP 的服务器 | 信令协调 + 流量中继 |
| Provider | `frp_proxy_client` | 内网机器 | 将内网服务注册到公网服务器 |
| Accessor | `frp_proxy_accessor` | 访问端 | 通过公网服务器访问内网服务 |

## 文件清单

| 文件 | 说明 |
|------|------|
| `frp_proxy_server` | 公网服务器程序 |
| `frp_proxy_client` | Provider 程序 |
| `frp_proxy_accessor` | Accessor 程序 |
| `ca_root.crt` | CA 根证书 |
| `local.crt` | 证书文件 |
| `local.key` | 证书私钥 |
| `launch_server.sh` | 服务器启动脚本 |
| `launch_provider.sh` | Provider 启动脚本 |
| `launch_accessor.sh` | Accessor 启动脚本 |
| `server.json` | 服务器配置示例 |
| `provider.json` | Provider 配置示例 |
| `accessor.json` | Accessor 配置示例 |

## 快速开始

### 部署公网服务器

编辑 `server.json`，修改 `traffic_secret` 和 `allowed_register_keys`，然后启动：
```bash
./launch_server.sh
```

需要开放端口：TCP `listen_tcp_port`、UDP `listen_udp_port` 和 `listen_udp_port + 1`（P2P 可选）。

### 部署 Provider（内网）

编辑 `provider.json`，修改 `public_server_host`、`traffic_secret`、`register_key` 和 `services`，然后启动：
```bash
./launch_provider.sh
```

### 部署 Accessor（访问端）

编辑 `accessor.json`，修改 `public_server_host`、`traffic_secret`、`register_key` 和 `listeners`，然后启动：
```bash
./launch_accessor.sh
```

## 命令行参考

| 参数 | 简写 | 说明 |
|------|------|------|
| `--config <path>` | `-c` | 指定 JSON 配置文件路径 |
| `--print-example-config` | `-p` | 打印示例配置并退出 |
| `--version` | `-v` | 显示版本信息 |
| `--help` | `-h` | 显示帮助信息 |
README_EOF
echo "    README.md"

# Summary
echo ""
echo "==> Deploy package ready at: ${DEPLOY_DIR}"
echo ""
ls -lh "${DEPLOY_DIR}"
