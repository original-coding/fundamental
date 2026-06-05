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
BINARIES=("frp_proxy_server" "frp_proxy_client" "traceroute")
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

# Generate default configs
echo "==> Generating default configs..."

cat > "${DEPLOY_DIR}/server.json" << 'SERVER_JSON'
{
    "threads": 8,
    "listen_tcp_port": 15000,
    "listen_udp_port": 15000,
    "traffic_secret": "dev-secret",
    "allowed_register_keys": ["dev-key"],
    "data_channel_idle_timeout_seconds": 120,
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

cat > "${DEPLOY_DIR}/client.json" << 'CLIENT_JSON'
{
    "threads": 8,
    "public_server_host": "server.example.com",
    "public_server_tcp_port": 15000,
    "public_server_udp_port": 15000,
    "traffic_secret": "dev-secret",
    "nat_type": 2,
    "nat_ttl": 3,
    "local_ip": "127.0.0.1",
    "data_channel_idle_timeout_seconds": 120,
    "ssl": {
        "certificate_path": "local.crt",
        "private_key_path": "local.key",
        "ca_certificate_path": "ca_root.crt",
        "disable_ssl": false
    },
    "groups": [
        {
            "register_key": "dev-key",
            "services": [
                {
                    "service_name": "ssh",
                    "service_type": 0,
                    "target_host": "127.0.0.1",
                    "target_port": 22,
                    "enable_p2p": true
                }
            ],
            "listeners": [
                {
                    "service_name": "ssh",
                    "service_type": 0,
                    "listen_host": "127.0.0.1",
                    "listen_port": 42222,
                    "enable_p2p": true
                }
            ]
        }
    ]
}
CLIENT_JSON

echo "    server.json  client.json"

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
if [ -f "${CONFIG}.overlay" ]; then
    CONFIG="${CONFIG}.overlay"
fi
cd "$(dirname "$0")"
exec ./frp_proxy_server --config "${CONFIG}"
EOF
chmod +x "${DEPLOY_DIR}/launch_server.sh"

cat > "${DEPLOY_DIR}/launch_client.sh" << 'EOF'
#!/bin/bash
# FRP Unified Client Launch Script
# Usage: ./launch_client.sh [config_path]

CONFIG="${1:-client.json}"
if [ -f "${CONFIG}.overlay" ]; then
    CONFIG="${CONFIG}.overlay"
fi
cd "$(dirname "$0")"
exec ./frp_proxy_client --config "${CONFIG}"
EOF
chmod +x "${DEPLOY_DIR}/launch_client.sh"

echo "    launch_server.sh  launch_client.sh"

# Copy service install script
echo "==> Copying service install script..."
cp "${SCRIPT_DIR}/install_frp_service.sh" "${DEPLOY_DIR}/"
chmod +x "${DEPLOY_DIR}/install_frp_service.sh"
echo "    install_frp_service.sh"

# Generate README
echo "==> Generating README..."
cat > "${DEPLOY_DIR}/README.md" << 'README_EOF'
# FRP 内网穿透部署说明

## 概述

FRP (Fast Reverse Proxy) 是一个基于 TCP 的内网穿透工具，支持 TCP/UDP 中继转发和 UDP NAT 穿透（P2P）两种模式。

## 架构说明

| 角色 | 程序 | 部署位置 | 说明 |
|------|------|----------|------|
| 公网服务器 | `frp_proxy_server` | 具有公网 IP 的服务器 | 信令协调 + 流量中继 |
| 统一客户端 | `frp_proxy_client` | 任意位置 | 同时承担服务注册（provider）和服务访问（accessor） |

一个客户端可配置多个 `groups`，每组有独立的 `register_key`，可同时提供 `services` 和 `listeners`。

## 文件清单

| 文件 | 说明 |
|------|------|
| `frp_proxy_server` | 公网服务器程序 |
| `frp_proxy_client` | 统一客户端程序 |
| `traceroute` | 路径追踪工具 |
| `ca_root.crt` | CA 根证书 |
| `local.crt` | 证书文件 |
| `local.key` | 证书私钥 |
| `launch_server.sh` | 服务器启动脚本 |
| `launch_client.sh` | 客户端启动脚本 |
| `install_frp_service.sh` | systemd 服务安装管理脚本 |
| `server.json` | 服务器配置示例 |
| `client.json` | 客户端配置示例（含 groups） |

## 配置说明

### 服务器配置 (`server.json`)

| 字段 | 说明 |
|------|------|
| `listen_tcp_port` | TCP 监听端口 |
| `listen_udp_port` | UDP 端口基址，绑定 port 和 port+1；0=仅中继 |
| `traffic_secret` | 流量加密密钥，三方必须一致 |
| `allowed_register_keys` | 允许的注册密钥列表 |
| `data_channel_idle_timeout_seconds` | 数据通道空闲超时，0=禁用 |

### 客户端配置 (`client.json`)

顶层字段：

| 字段 | 说明 |
|------|------|
| `public_server_host` | 公网服务器地址 |
| `public_server_tcp_port` | 服务器 TCP 端口 |
| `public_server_udp_port` | 服务器 UDP 端口，0=仅中继 |
| `traffic_secret` | 流量加密密钥 |
| `nat_type` | 0=禁用P2P, 1=Symmetric, 2=Cone |
| `nat_ttl` | TTL 预探测值，默认 3，仅本地使用 |
| `groups` | 服务组列表，每组有独立的 `register_key` |

每组 (`groups[]`) 字段：

| 字段 | 说明 |
|------|------|
| `register_key` | 注册密钥，须在服务器允许列表中，跨组不可重复 |
| `services` | 提供的服务列表（可选） |
| `listeners` | 监听的访问入口列表（可选） |

服务 (`services[]`) 字段：

| 字段 | 说明 |
|------|------|
| `service_name` | 服务名称 |
| `target_host` / `target_port` | 后端服务地址和端口 |
| `service_type` | 0=TCP, 1=UDP |
| `enable_p2p` | 是否允许 P2P 升级 |

监听 (`listeners[]`) 字段：

| 字段 | 说明 |
|------|------|
| `service_name` | 要访问的服务名称 |
| `listen_host` / `listen_port` | 本地监听地址和端口 |
| `service_type` | 0=TCP, 1=UDP |
| `enable_p2p` | 是否尝试 P2P 升级 |

### NAT 类型说明

- **0 (disabled)**: 禁用 P2P，所有流量走中继
- **1 (symmetric)**: 对称型 NAT
- **2 (cone)**: 锥形 / 非对称 NAT，P2P 成功率最高

## 快速开始

### 部署公网服务器

编辑 `server.json`，修改 `traffic_secret` 和 `allowed_register_keys`：
```bash
./launch_server.sh
```

需开放端口：TCP `listen_tcp_port`，UDP `listen_udp_port` 和 `listen_udp_port + 1`（可选）。

### 部署客户端

`client.json` 示例包含一个 group，同时提供 `ssh` 服务（内网 22 端口）并监听本地 42222 端口：
```bash
./launch_client.sh
```

生产环境中可按需拆分 groups——纯 provider 只配 `services`，纯 accessor 只配 `listeners`，也可以混配。

## 安装 systemd 服务（用户级，无需 root）

使用 `install_frp_service.sh` 脚本管理 `custom_frp_proxy` 服务：

```bash
# 生成服务文件并安装
./install_frp_service.sh install

# 指定配置文件（默认为 client.json）
./install_frp_service.sh install my_config.json

# 启动 / 停止 / 重启
./install_frp_service.sh start
./install_frp_service.sh stop
./install_frp_service.sh restart

# 设置开机自启 / 禁用开机自启
./install_frp_service.sh enable
./install_frp_service.sh disable

# 查看状态
./install_frp_service.sh status

# 卸载服务
./install_frp_service.sh remove
```

> 用户级服务默认在登录时启动。若需系统引导时启动（未登录状态），执行：`sudo loginctl enable-linger $USER`

## 命令行参考

| 参数 | 简写 | 说明 |
|------|------|------|
| `--config <path>` | `-c` | 指定 JSON 配置文件路径 |
| `--print-example-config` | `-p` | 打印示例配置并退出 |
| `--version` | `-v` | 显示版本信息 |
| `--help` | `-h` | 显示帮助信息 |

## UDP 代理说明

UDP 服务代理使用 KCP 协议通过 TCP 中继通道传输，可选 P2P 升级为直连。
README_EOF
echo "    README.md"

# Summary
echo ""
echo "==> Deploy package ready at: ${DEPLOY_DIR}"
echo ""
ls -lh "${DEPLOY_DIR}"
