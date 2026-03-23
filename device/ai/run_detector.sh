#!/usr/bin/env bash
# Start the Activity Detector Python process.
# Reads configuration from device/config/default.ini [ai_summary] section.
#
# Requirements: 2.1, 3.1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_PATH="${AI_SUMMARY_CONFIG_PATH:-$DEVICE_DIR/config/default.ini}"

export AI_SUMMARY_CONFIG_PATH="$CONFIG_PATH"

exec python3 -m device.ai.activity_detector
