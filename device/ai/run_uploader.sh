#!/usr/bin/env bash
# Start the S3 Uploader Python process.
# Requirements: 2.1, 3.1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_PYTHON="/opt/smart-camera/.venv/bin/python3"
CONFIG_PATH="${AI_SUMMARY_CONFIG_PATH:-/etc/smart-camera/config.ini}"

export AI_SUMMARY_CONFIG_PATH="$CONFIG_PATH"
export PYTHONPATH="${SCRIPT_DIR}/../.."

exec "$VENV_PYTHON" -m device.ai.s3_uploader
