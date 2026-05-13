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
            "target_port": 22
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
            "nat_type": 2
        }
    ]
}
ACCESSOR_JSON

echo "    server.json  provider.json  accessor.json"

# Copy certificates
echo "==> Copying certificates..."
CERT_DIR="${PROJECT_DIR}/frp/frp-linux"
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

# Copy README
echo "==> Copying README..."
cp "${PROJECT_DIR}/frp/README.md" "${DEPLOY_DIR}/"
echo "    README.md"

# Summary
echo ""
echo "==> Deploy package ready at: ${DEPLOY_DIR}"
echo ""
ls -lh "${DEPLOY_DIR}"
