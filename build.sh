#!/bin/bash
# Build and upload for HT-HC33 with OPI PSRAM enabled (required for UXGA 1600x1200)
set -e

SKETCH="${1:-/home/matheau/esp32_camera/take_photo}"
PORT="${2:-/dev/ttyUSB0}"
FQBN="heltec:esp_halow:HT-HC33"

arduino-cli compile --upload \
  --fqbn "$FQBN" \
  --build-property "build.psram_type=opi" \
  --build-property "build.memory_type=qio_opi" \
  --port "$PORT" \
  "$SKETCH"
