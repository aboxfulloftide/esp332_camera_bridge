#!/bin/bash
# Keep retrying the Pi radio test against the E6+ until one run succeeds
# (wake/join/list/download) or MAX_HOURS passes. Meant for unattended use when
# the Pi is near the camera and out of range of the uplink: everything is
# written to disk under radio_test_runs/ for later review.
#
#   tools/pi_wake_loop.sh                 # defaults: 4 hours, hci1, wlan1
#   MAX_HOURS=2 tools/pi_wake_loop.sh
#
# Stop it early:  pkill -f pi_wake_loop.sh
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$(dirname "$REPO")/radio_test_runs}"
LOG="$OUT/wake_loop.log"
MAX_HOURS="${MAX_HOURS:-4}"
BLE_ADAPTER="${BLE_ADAPTER:-hci1}"
CAM_IFACE="${CAM_IFACE:-wlan1}"
mkdir -p "$OUT"
rm -f "$OUT/WAKE_LOOP_SUCCESS" "$OUT/WAKE_LOOP_GAVE_UP"
end=$(( $(date +%s) + MAX_HOURS * 3600 ))
n=0
echo "=== wake loop start $(date -Is) pid=$$ max_hours=$MAX_HOURS adapter=$BLE_ADAPTER cam_iface=$CAM_IFACE" >> "$LOG"
while [ "$(date +%s)" -lt "$end" ]; do
  n=$((n + 1))
  echo "=== attempt $n $(date -Is)" >> "$LOG"
  if python3 "$REPO/tools/pi_radio_test.py" \
      --ble-adapter "$BLE_ADAPTER" --cam-iface "$CAM_IFACE" --allow-no-uplink \
      --ble-scan-seconds 20 --direct-connect-seconds 90 --out "$OUT" >> "$LOG" 2>&1; then
    echo "=== SUCCESS on attempt $n $(date -Is)" >> "$LOG"
    date -Is > "$OUT/WAKE_LOOP_SUCCESS"
    exit 0
  fi
  echo "=== attempt $n failed $(date -Is); retrying in 10s" >> "$LOG"
  sleep 10
done
echo "=== gave up after $n attempts $(date -Is)" >> "$LOG"
date -Is > "$OUT/WAKE_LOOP_GAVE_UP"
exit 1
