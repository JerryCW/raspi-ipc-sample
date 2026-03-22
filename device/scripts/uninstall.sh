#!/usr/bin/env bash
# Smart Camera - Uninstallation Script
# Stops the service, removes binary, config, logs, cache, and systemd unit.
# Must be run as root or with sudo.

set -euo pipefail

BINARY_NAME="smart-camera"
INSTALL_DIR="/usr/local/bin"
CONFIG_DIR="/etc/smart-camera"
LOG_DIR="/var/log/smart-camera"
CACHE_DIR="/var/cache/smart-camera"
SERVICE_FILE="smart-camera.service"
SYSTEMD_DIR="/etc/systemd/system"

# --- Helpers ---

info()  { echo "[INFO]  $*"; }
error() { echo "[ERROR] $*" >&2; }

check_root() {
    if [[ $EUID -ne 0 ]]; then
        error "This script must be run as root (use sudo)."
        exit 1
    fi
}

# --- Steps ---

stop_service() {
    info "Stopping and disabling systemd service..."
    if systemctl is-active --quiet "${SERVICE_FILE}" 2>/dev/null; then
        systemctl stop "${SERVICE_FILE}"
    fi
    if systemctl is-enabled --quiet "${SERVICE_FILE}" 2>/dev/null; then
        systemctl disable "${SERVICE_FILE}"
    fi
    info "Service stopped and disabled."
}

remove_service() {
    info "Removing systemd service file..."
    rm -f "${SYSTEMD_DIR}/${SERVICE_FILE}"
    systemctl daemon-reload
    info "Service file removed."
}

remove_binary() {
    info "Removing binary..."
    rm -f "${INSTALL_DIR}/${BINARY_NAME}"
    info "Binary removed."
}

remove_directories() {
    info "Removing configuration, log, and cache directories..."
    rm -rf "${CONFIG_DIR}"
    rm -rf "${LOG_DIR}"
    rm -rf "${CACHE_DIR}"
    info "Directories removed."
}

# --- Main ---

main() {
    info "=== Smart Camera Uninstallation ==="
    check_root
    stop_service
    remove_service
    remove_binary
    remove_directories
    info "=== Uninstallation complete ==="
}

main "$@"
