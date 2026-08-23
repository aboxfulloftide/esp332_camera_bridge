#!/bin/bash
# Build and upload for HT-HC33 with OPI PSRAM enabled (required for UXGA 1600x1200)
set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SKETCH="${1:-$SCRIPT_DIR/gardepro_dual_radio_bridge}"
PORT="${2:-/dev/ttyUSB0}"
FQBN="heltec:esp_halow:HT-HC33:PSRAM=opi"

arduino-cli compile --upload \
  --fqbn "$FQBN" \
  --port "$PORT" \
  "$SKETCH"
