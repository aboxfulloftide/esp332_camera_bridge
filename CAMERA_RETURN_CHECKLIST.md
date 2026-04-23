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
   - current live result:
     - present: `standby_timeout`, `wifi`
     - not present on this firmware: `power_source`, `screen_timeout`, `cellular_transfer`, `instant_upload`
3. Validate verified action wrappers:
   - `take-picture`
   - `video-stop`
   - `format-start`
   - current live result:
     - `take-picture` confirmed
     - `video-stop` confirmed
     - `video-stop` now also handles the camera's early placeholder-video behavior (`uid: "00000000"`) and no longer requires a post-stop ID change
     - `format-start` confirmed
     - raw gallery check after format returned an empty `data` array
4. Validate media path and delete behavior:
   - `media-paths`
   - `media-delete`
   - confirm extension-form vs legacy delete paths
   - current live result:
     - `media-paths` confirmed for photo and video items
     - `media-delete` confirmed for a disposable photo and a disposable video item
5. If media timestamps look wrong, set the clock:
   - `python3 /home/matheau/esp32_camera/gardepro_server_control.py set-clock --timestamp "$(date -u '+%Y-%m-%d %H:%M:%S')"`
   - note: `/cmd/setGmtClock` expects UTC input; sending local Eastern time produced a 4-hour offset during validation
   - direct readback:
     - `python3 /home/matheau/esp32_camera/gardepro_server_control.py info --index 4`

## Teardown Validation

1. Stop live view if it was started:
   - `python3 /home/matheau/esp32_camera/gardepro_server_control.py stream-stop --timeout 45 --poll-interval 1`
2. Close the short-lived session:
   - `python3 /home/matheau/esp32_camera/gardepro_server_control.py session-close --timeout 45 --poll-interval 2`
   - expected verified result:
     - `standby.code == 0`
     - `after.wifi_connected == false`
     - `session_closed == true`
3. If intentionally keeping the camera awake briefly:
   - `python3 /home/matheau/esp32_camera/gardepro_server_control.py session-close --no-standby`

## If Problems Persist

1. Re-check BLE telemetry in `/status`.
2. Re-check the camera power state before drawing conclusions from BLE discovery failures.
3. Check whether `/status` shows `standby_requested:true`; if it does and WiFi still stays up, re-verify the board is running the patched bridge firmware.
4. Keep firmware as a clue source only; do not patch camera firmware unless the bridge approach is exhausted.
