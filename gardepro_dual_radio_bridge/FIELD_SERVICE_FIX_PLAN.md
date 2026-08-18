# Trail ESP32 field-service verification

Date: 2026-08-18

Board: `trail_esp32`

Expected HaLow HTTP address: `http://192.168.1.160:18080`

## Firmware fixes now flashed

- `/status` now returns a compact board-health payload so it is usable over the marginal HaLow link.
- Detailed data formerly packed into `/status` is now split across smaller alternate status endpoints.
- `/upload/status` no longer scans storage while building JSON; queued batch counts are cached.
- SD is the primary persistent store when mounted.
  - Onboard photos use SD.
  - Failed Wi-Fi/BLE observation upload batches use SD.
  - LittleFS remains the fallback if SD is unavailable.
- Onboard captures still run every 30 minutes.
  - With valid time, normal local-time window logic is used.
  - Without valid time/HaLow, uptime fallback still captures every 30 minutes.
- Captures made before network time is available store boot uptime and are backfilled after time sync.
- BLE upload payloads have been verified to include advertisement fields, including `manufacturer_data`, `adv_services`, `adv_service_data`, `tx_power`, `local_name`, and `signal_dbm`.

## Verified after flash over USB

- HaLow connected:
  - IP: `192.168.1.160`
  - HTTP port: `18080`
- SD mounted:
  - `storage_type: "sd"`
  - card size: about `63.8 GB`
- Onboard photos were deleted after verification:
  - `deleted=1`
  - `failed=0`
  - `stored_photo_count=0`
- Wi-Fi observations uploaded successfully.
- BLE observations uploaded successfully.
- Last BLE scan included advertisement metadata:
  - manufacturer count: `3`
  - services count: `1`
  - service data count: `5`
  - tx power count: `1`
  - name count: `2`
- Observation retry queue was empty after successful upload:
  - queued batches: `0`
  - queued bytes: `0`

## Field checks before leaving device outside

Run these from the house/server side:

```bash
ping -c 50 192.168.1.160
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/system/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/halow/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/wifi/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/camera/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/timing/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/stream/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/ble/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/control/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/battery/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/upload/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/onboard/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/sd/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/upload/ble/last
```

Avoid firing several curls at the board at exactly the same time. The ESP32 HTTP server is small and the HaLow link has shown jitter; sequential requests are reliable.

## Old `/status` field coverage

- `/status`: compact summary for field checks.
- `/system/status`: uptime, hostname, boot counters, PSRAM, chip temperature.
- `/halow/status`: HaLow connected/status/SSID/BSSID/MAC/IP/gateway/RSSI/event counters.
- `/wifi/status`: trail-camera Wi-Fi state plus Wi-Fi scanner counters.
- `/camera/status`: camera IP, camera Wi-Fi ever-connected flag, standby flag, session lease summary.
- `/timing/status`: bringup/BLE wake/hotspot wait/Wi-Fi join/camera HTTP timing.
- `/stream/status`: RTSP stream status, tunnel state, recovery counters, UDP/media packet counters.
- `/ble/status`: BLE wake/discovery/GATT state, recent devices, notify info, BLE scanner/upload metadata counters.
- `/control/status`: queued/active/last control action state.
- `/battery/status`: battery ADC and charge/done GPIO state.
- `/onboard/status`: onboard camera, schedule, storage, latest media, and timelapse state.
- `/upload/status`: telemetry/event/observation upload state and retry queue state.
- `/session/status`: camera session lease details.
- `/sd/status`: SD mount/card/storage details.

## Useful USB serial commands

```text
status
halow_status
halow_up
onboard_status
onboard_capture
onboard_delete_all
sd_mount
sd_status
upload_status
```
