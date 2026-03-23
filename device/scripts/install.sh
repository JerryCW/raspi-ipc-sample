#!/usr/bin/env bash
# Smart Camera - Installation Script
# Installs dependencies, builds C++ project, deploys binary,
# sets up Python AI components, and configures all systemd services.
# Must be run as root or with sudo on Linux aarch64.

set -euo pipefail

BINARY_NAME="smart-camera"
INSTALL_DIR="/usr/local/bin"
CONFIG_DIR="/etc/smart-camera"
LOG_DIR="/var/log/smart-camera"
CACHE_DIR="/var/cache/smart-camera"
CAPTURE_DIR="/var/lib/smart-camera/captures"
AI_INSTALL_DIR="/opt/smart-camera/ai"
VENV_DIR="/opt/smart-camera/.venv"
SYSTEMD_DIR="/etc/systemd/system"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# --- Helpers ---

info()  { echo "[INFO]  $*"; }
warn()  { echo "[WARN]  $*"; }
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
        warn "Target platform is aarch64, current architecture is ${arch}."
    fi
}

# --- C++ Steps ---

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
        libcamera-dev \
        python3-venv \
        python3-pip
    info "Dependencies installed."
}

build_project() {
    info "Building C++ project (Release)..."

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
    info "C++ build complete."
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
        # Ensure [ai_summary] section exists in existing config
        if ! grep -q "\[ai_summary\]" "${CONFIG_DIR}/config.ini"; then
            info "Adding [ai_summary] section to existing config..."
            cat >> "${CONFIG_DIR}/config.ini" << 'AIEOF'

[ai_summary]
export_fps = 2
shm_name = /smart_camera_frames
shm_size_mb = 20
socket_path = /run/smart-camera/ai.sock
detect_classes = person,cat,dog,bird
confidence_threshold = 0.5
session_timeout_sec = 60
capture_dir = /var/lib/smart-camera/captures/
capture_max_files = 500
capture_max_size_mb = 200
disk_min_free_mb = 100
s3_bucket = smart-camera-captures
s3_prefix = captures
upload_retry_count = 5
upload_retry_interval_sec = 1
dynamodb_table = smart-camera-events
event_ttl_days = 90
AIEOF
        fi
        info "Configuration file already exists, updated if needed."
    fi
}

create_directories() {
    info "Creating runtime directories..."
    mkdir -p "${LOG_DIR}"
    mkdir -p "${CACHE_DIR}"
    mkdir -p "${CAPTURE_DIR}"
    # Set capture dir ownership to the user running the AI processes
    chown -R root:root "${CAPTURE_DIR}"
    chmod 755 "${CAPTURE_DIR}"
    info "Directories created."
}

# --- Python AI Steps ---

install_python_ai() {
    info "Setting up Python AI components..."

    # Create venv if it doesn't exist
    if [[ ! -d "${VENV_DIR}" ]]; then
        info "Creating Python virtual environment at ${VENV_DIR}..."
        python3 -m venv "${VENV_DIR}"
    fi

    # Install Python dependencies
    info "Installing Python dependencies..."
    "${VENV_DIR}/bin/pip" install --quiet --upgrade pip
    "${VENV_DIR}/bin/pip" install --quiet -r "${PROJECT_DIR}/ai/requirements.txt"
    info "Python dependencies installed."

    # Deploy AI module code
    info "Deploying AI modules to ${AI_INSTALL_DIR}..."
    mkdir -p "${AI_INSTALL_DIR}"
    # Copy Python source files
    cp -f "${PROJECT_DIR}/ai/__init__.py" "${AI_INSTALL_DIR}/"
    cp -f "${PROJECT_DIR}/ai/config.py" "${AI_INSTALL_DIR}/"
    cp -f "${PROJECT_DIR}/ai/ipc_client.py" "${AI_INSTALL_DIR}/"
    cp -f "${PROJECT_DIR}/ai/session.py" "${AI_INSTALL_DIR}/"
    cp -f "${PROJECT_DIR}/ai/activity_detector.py" "${AI_INSTALL_DIR}/"
    cp -f "${PROJECT_DIR}/ai/s3_uploader.py" "${AI_INSTALL_DIR}/"
    cp -f "${PROJECT_DIR}/ai/requirements.txt" "${AI_INSTALL_DIR}/"
    # Copy launch scripts
    cp -f "${PROJECT_DIR}/ai/run_detector.sh" "${AI_INSTALL_DIR}/"
    cp -f "${PROJECT_DIR}/ai/run_uploader.sh" "${AI_INSTALL_DIR}/"
    chmod +x "${AI_INSTALL_DIR}/run_detector.sh" "${AI_INSTALL_DIR}/run_uploader.sh"

    # Create device/ai symlink for PYTHONPATH resolution
    mkdir -p /opt/smart-camera/device
    ln -sf "${AI_INSTALL_DIR}" /opt/smart-camera/device/ai

    info "Python AI components deployed."
}

# --- Systemd Services ---

install_services() {
    info "Installing systemd services..."

    # Main smart-camera service
    install -m 0644 "${PROJECT_DIR}/deploy/smart-camera.service" "${SYSTEMD_DIR}/smart-camera.service"

    # Activity Detector service
    install -m 0644 "${PROJECT_DIR}/deploy/activity-detector.service" "${SYSTEMD_DIR}/activity-detector.service"

    # S3 Uploader service
    install -m 0644 "${PROJECT_DIR}/deploy/s3-uploader.service" "${SYSTEMD_DIR}/s3-uploader.service"

    systemctl daemon-reload

    # Enable all services
    systemctl enable smart-camera.service
    systemctl enable activity-detector.service
    systemctl enable s3-uploader.service

    # Restart smart-camera (FrameExporter creates the socket)
    info "Starting smart-camera..."
    systemctl restart smart-camera.service

    # Wait for socket to be created
    info "Waiting for FrameExporter socket..."
    local retries=10
    while [[ $retries -gt 0 ]]; do
        if [[ -S /run/smart-camera/ai.sock ]]; then
            info "FrameExporter socket ready."
            break
        fi
        sleep 1
        retries=$((retries - 1))
    done

    if [[ ! -S /run/smart-camera/ai.sock ]]; then
        warn "FrameExporter socket not found after 10s. Activity Detector will retry on its own."
    fi

    # Start AI services
    info "Starting AI services..."
    systemctl restart activity-detector.service
    systemctl restart s3-uploader.service

    info "All services installed and started."
}

show_status() {
    info "=== Service Status ==="
    systemctl --no-pager status smart-camera.service || true
    echo ""
    systemctl --no-pager status activity-detector.service || true
    echo ""
    systemctl --no-pager status s3-uploader.service || true
}

# --- Main ---

main() {
    info "========================================="
    info "  Smart Camera Full Installation"
    info "========================================="
    check_root
    check_platform

    info ""
    info "--- Step 1/6: System Dependencies ---"
    install_dependencies

    info ""
    info "--- Step 2/6: Build C++ ---"
    build_project

    info ""
    info "--- Step 3/6: Deploy Binary & Config ---"
    deploy_binary
    deploy_config
    create_directories

    info ""
    info "--- Step 4/6: Python AI Components ---"
    install_python_ai

    info ""
    info "--- Step 5/6: Systemd Services ---"
    install_services

    info ""
    info "--- Step 6/6: Verify ---"
    show_status

    info ""
    info "========================================="
    info "  Installation complete!"
    info "  Logs: journalctl -u smart-camera -f"
    info "        journalctl -u activity-detector -f"
    info "        journalctl -u s3-uploader -f"
    info "========================================="
}

main "$@"
