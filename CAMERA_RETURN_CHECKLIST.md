# Camera Return Checklist

Use this once the trail camera has power again.

## First Pass

1. Confirm the camera is actually powered and the battery issue is resolved.
2. Check board reachability:
   - `python3 /home/matheau/esp32_camera/gardepro_server_control.py status`
3. Run one clean session open:
   - `python3 /home/matheau/esp32_camera/gardepro_server_control.py session-open --timeout 60 --poll-interval 1`
4. Inspect `/status` fields if open fails:
   - `control_last_message`
   - `ble_stage`
   - `ble_scan_mode`
   - `ble_scan_results`
   - `ble_target_seen_count`

## Session Validation

Once `session-open` works:

1. Validate settings read path:
   - `setting-keys`
   - `setting-values`
2. Probe firmware-derived candidate keys carefully:
   - `standby_timeout`
   - `wifi`
   - `power_source`
   - `screen_timeout`
   - `cellular_transfer`
   - `instant_upload`
3. Validate verified action wrappers:
   - `take-picture`
   - `video-stop`
   - `format-start`
4. Validate media path and delete behavior:
   - `media-paths`
   - `media-delete`
   - confirm extension-form vs legacy delete paths

## Teardown Validation

1. Stop live view if it was started:
   - `python3 /home/matheau/esp32_camera/gardepro_server_control.py stream-stop --timeout 45 --poll-interval 1`
2. Close the short-lived session:
   - `python3 /home/matheau/esp32_camera/gardepro_server_control.py session-close --timeout 30 --poll-interval 1`
3. If intentionally keeping the camera awake briefly:
   - `python3 /home/matheau/esp32_camera/gardepro_server_control.py session-close --no-standby`

## If Problems Persist

1. Re-check BLE telemetry in `/status`.
2. Re-check the camera power state before drawing conclusions from BLE discovery failures.
3. Keep firmware as a clue source only; do not patch camera firmware unless the bridge approach is exhausted.
