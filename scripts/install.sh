#!/usr/bin/env bash
# Smart Camera - Installation Script
# Installs dependencies, builds the project, deploys binary and configures systemd service.
# Must be run as root or with sudo on Linux aarch64.

set -euo pipefail

BINARY_NAME="smart-camera"
INSTALL_DIR="/usr/local/bin"
CONFIG_DIR="/etc/smart-camera"
LOG_DIR="/var/log/smart-camera"
CACHE_DIR="/var/cache/smart-camera"
SERVICE_FILE="smart-camera.service"
SYSTEMD_DIR="/etc/systemd/system"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# --- Helpers ---

info()  { echo "[INFO]  $*"; }
error() { echo "[ERROR] $*" >&2; }

check_root() {
    if [[ $EUID -ne 0 ]]; then
        error "This script must be run as root (use sudo)."
        exit 1
    fi
}

check_platform() {
    local arch
    arch="$(uname -m)"
    if [[ "$(uname -s)" != "Linux" ]]; then
        error "This install script is intended for Linux only."
        exit 1
    fi
    if [[ "$arch" != "aarch64" ]]; then
        info "Warning: target platform is aarch64, current architecture is ${arch}."
    fi
}

# --- Steps ---

install_dependencies() {
    info "Installing build dependencies..."
    apt-get update -qq
    apt-get install -y --no-install-recommends \
        cmake \
        g++ \
        libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        libssl-dev \
        libcurl4-openssl-dev \
        libsystemd-dev \
        libcamera-dev
    info "Dependencies installed."
}

build_project() {
    info "Building project (Release)..."

    # KVS WebRTC SDK search paths (add yours if installed elsewhere)
    local cmake_extra_args=""
    local kvs_sdk_paths=(
        "/opt/amazon-kinesis-video-streams-webrtc-sdk-c"
        "/usr/local"
    )
    for sdk_path in "${kvs_sdk_paths[@]}"; do
        if [[ -d "${sdk_path}" ]]; then
            cmake_extra_args="-DCMAKE_PREFIX_PATH=${sdk_path}"
            info "Found KVS WebRTC SDK at ${sdk_path}"
            break
        fi
    done

    cmake -B "${PROJECT_DIR}/build" -S "${PROJECT_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        ${cmake_extra_args}
    cmake --build "${PROJECT_DIR}/build" --parallel "$(nproc)"
    info "Build complete."
}

deploy_binary() {
    info "Deploying binary to ${INSTALL_DIR}/${BINARY_NAME}..."
    install -m 0755 "${PROJECT_DIR}/build/${BINARY_NAME}" "${INSTALL_DIR}/${BINARY_NAME}"
    info "Binary deployed."
}

deploy_config() {
    info "Setting up configuration directory ${CONFIG_DIR}..."
    mkdir -p "${CONFIG_DIR}"
    mkdir -p "${CONFIG_DIR}/certs"

    if [[ ! -f "${CONFIG_DIR}/config.ini" ]]; then
        install -m 0644 "${PROJECT_DIR}/config/default.ini" "${CONFIG_DIR}/config.ini"
        info "Default configuration installed."
    else
        info "Configuration file already exists, skipping."
    fi
}

create_directories() {
    info "Creating runtime directories..."
    mkdir -p "${LOG_DIR}"
    mkdir -p "${CACHE_DIR}"
    info "Directories created."
}

install_service() {
    info "Installing systemd service..."
    install -m 0644 "${PROJECT_DIR}/deploy/${SERVICE_FILE}" "${SYSTEMD_DIR}/${SERVICE_FILE}"
    systemctl daemon-reload
    systemctl enable "${SERVICE_FILE}"
    systemctl start "${SERVICE_FILE}"
    info "Service installed and started."
}

# --- Main ---

main() {
    info "=== Smart Camera Installation ==="
    check_root
    check_platform
    install_dependencies
    build_project
    deploy_binary
    deploy_config
    create_directories
    install_service
    info "=== Installation complete ==="
}

main "$@"
