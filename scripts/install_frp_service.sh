#!/bin/bash
set -e

SERVICE_NAME="custom_frp_proxy"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LAUNCH_SCRIPT="${SCRIPT_DIR}/launch_client.sh"
SERVICE_FILE="${SCRIPT_DIR}/${SERVICE_NAME}.service"
SYSTEMD_USER_DIR="${HOME}/.config/systemd/user"
SYSTEMD_PATH="${SYSTEMD_USER_DIR}/${SERVICE_NAME}.service"

usage() {
    cat << 'EOF'
Usage: ./install_frp_service.sh <command> [config_file]

Commands:
  install  [config]  生成 service 文件并安装到 systemd (用户级)
  start              启动服务
  stop               停止服务
  restart            重启服务
  enable             设置开机自启 (需开启 lingering)
  disable            禁用开机自启
  status             查看服务状态
  remove             停止并卸载服务

Examples:
  ./install_frp_service.sh install
  ./install_frp_service.sh install my_config.json
  ./install_frp_service.sh start
EOF
    exit 0
}

generate_service() {
    local config="${1:-client.json}"

    cat > "${SERVICE_FILE}" << SERVICE_EOF
[Unit]
Description=Custom FRP Proxy Client
After=network.target

[Service]
Type=simple
WorkingDirectory=${SCRIPT_DIR}
ExecStart=${LAUNCH_SCRIPT} ${config}
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=default.target
SERVICE_EOF

    echo "Generated: ${SERVICE_FILE}"
}

cmd_install() {
    if [ ! -f "${LAUNCH_SCRIPT}" ]; then
        echo "ERROR: launch_client.sh not found at ${LAUNCH_SCRIPT}"
        exit 1
    fi

    generate_service "$@"

    mkdir -p "${SYSTEMD_USER_DIR}"
    cp "${SERVICE_FILE}" "${SYSTEMD_PATH}"
    systemctl --user daemon-reload
    systemctl --user enable "${SERVICE_NAME}"
    systemctl --user start "${SERVICE_NAME}"

    echo "Service '${SERVICE_NAME}' installed and started (user level)."
    echo ""
    echo "Tip: user services start at login by default. To start at system boot:"
    echo "  sudo loginctl enable-linger \$USER"
    echo ""
    echo "Status: systemctl --user status ${SERVICE_NAME}"
    echo "Logs:   journalctl --user -u ${SERVICE_NAME} -f"
}

cmd_start() {
    systemctl --user start "${SERVICE_NAME}"
    echo "Service '${SERVICE_NAME}' started."
}

cmd_stop() {
    systemctl --user stop "${SERVICE_NAME}"
    echo "Service '${SERVICE_NAME}' stopped."
}

cmd_restart() {
    systemctl --user restart "${SERVICE_NAME}"
    echo "Service '${SERVICE_NAME}' restarted."
}

cmd_enable() {
    systemctl --user enable "${SERVICE_NAME}"
    echo "Service '${SERVICE_NAME}' enabled (auto-start on login)."
}

cmd_disable() {
    systemctl --user disable "${SERVICE_NAME}"
    echo "Service '${SERVICE_NAME}' disabled."
}

cmd_status() {
    systemctl --user status "${SERVICE_NAME}" --no-pager
}

cmd_remove() {
    systemctl --user stop "${SERVICE_NAME}" 2>/dev/null || true
    systemctl --user disable "${SERVICE_NAME}" 2>/dev/null || true
    rm -f "${SYSTEMD_PATH}"
    systemctl --user daemon-reload
    echo "Service '${SERVICE_NAME}' removed."
}

case "${1:-}" in
    install) shift; cmd_install "$@";;
    start)   cmd_start;;
    stop)    cmd_stop;;
    restart) cmd_restart;;
    enable)  cmd_enable;;
    disable) cmd_disable;;
    status)  cmd_status;;
    remove)  cmd_remove;;
    -h|--help|help) usage;;
    *)       usage;;
esac
