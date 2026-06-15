#!/bin/bash
# Build and upload for HT-HC33 with OPI PSRAM enabled (required for UXGA 1600x1200)
set -e

SKETCH="${1:-/home/matheau/esp32_camera/take_photo}"
PORT="${2:-/dev/ttyUSB0}"
FQBN="heltec:esp_halow:HT-HC33:PSRAM=opi"

arduino-cli compile --upload \
  --fqbn "$FQBN" \
  --port "$PORT" \
  "$SKETCH"
