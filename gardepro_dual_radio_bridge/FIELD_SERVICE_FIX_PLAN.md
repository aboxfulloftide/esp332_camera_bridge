# Trail ESP32 field-service verification

Date: 2026-08-19

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
- Current source changes observation uploads to the wireless database host at `192.168.1.42:80`.
- Camera-priority firmware disables Wi-Fi/BLE scanning and RF observation upload execution entirely. Scanner routes remain only as disabled compatibility/status responses.
- Camera jobs can now be queued with `POST /jobs?action=bringup|live_view_start|live_view_stop|trigger_photo`.
- Live view no longer fails only because HaLow tunnel connect fails. If RTSP starts, the ESP32 keeps the camera-side stream local and retries/uses the tunnel when available.
- Current source adds scanner schedule control endpoints:
  - `GET /scanner/config`
  - `POST /scanner/config?enabled=true|false`
  - `POST /scanner/enable`
  - `POST /scanner/disable`
- Current source adds OTA update endpoints:
  - `GET /firmware/status`
  - `POST /firmware/update`
- Current board HTTP API reference:
  - `gardepro_dual_radio_bridge/BOARD_HTTP_API.md`

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
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/healthz
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
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/firmware/status
curl -sS --connect-timeout 10 --max-time 30 http://192.168.1.160:18080/scanner/config
```

Avoid firing several curls at the board at exactly the same time. The ESP32 HTTP server is small and the HaLow link has shown jitter; sequential requests are the safest default.

Firmware build `Aug 25 2026 11:30:21` moves `server.handleClient()` ownership to a dedicated HTTP service task. The previous multi-context attempt could panic-reset the board; the corrected build avoids calling the WebServer from the main loop once the HTTP task is running. Field validation after OTA showed `/control/status` and `/healthz` stayed responsive through a full BLE wake and camera-hotspot wait, with `http_service.max_gap_ms` staying at 13 ms.

Current hardening disables automatic trail-camera HTTP keepalive and automatic camera recovery by default. This keeps an unreliable trail-camera connection from running BLE/Wi-Fi recovery work inside the board main loop. Use explicit control endpoints for camera work and use `/healthz` plus `http_service.max_gap_ms` to confirm whether the board HTTP task is staying responsive.

USB diagnostic finding on 2026-08-20: after the field HTTP failure, the ESP32 serial console and HaLow association were still alive, but the board WebServer/TCP listener stopped serving new HTTP clients. Firmware now exposes `POST /system/http_restart`, serial command `http_restart`, and idle periodic HTTP listener restart telemetry under `http_service`.

## Old `/status` field coverage

- `/status`: compact summary for field checks.
- `/system/status`: uptime, hostname, boot counters, PSRAM, chip temperature.
- `/halow/status`: HaLow connected/status/SSID/BSSID/MAC/IP/gateway/RSSI/event counters.
- `/wifi/status`: trail-camera Wi-Fi state plus retained disabled Wi-Fi scanner counters.
- `/camera/status`: camera IP, camera Wi-Fi ever-connected flag, standby flag, session lease summary.
- `/timing/status`: bringup/BLE wake/hotspot wait/Wi-Fi join/camera HTTP timing.
- `/stream/status`: RTSP stream status, tunnel state, recovery counters, UDP/media packet counters.
- `/ble/status`: BLE wake/discovery/GATT state, recent devices, notify info, and retained disabled BLE scanner counters.
- `/control/status`: queued/active/last control action state.
- `/battery/status`: battery ADC and charge/done GPIO state.
- `/onboard/status`: onboard camera, schedule, storage, latest media, and timelapse state.
- `/upload/status`: telemetry/event/observation upload state and retry queue state.
- `/session/status`: camera session lease details.
- `/sd/status`: SD mount/card/storage details.
- `/firmware/status`: firmware version and OTA status.
- `/scanner/config`: disabled scanner compatibility/status response.

## Scanner routes

Wi-Fi/BLE RF scanning and observation upload execution are disabled in camera-priority firmware. This reduces ESP32 load and avoids unnecessary 2.4 GHz/BLE activity that can interfere with trail-camera and onboard-camera reliability.

Do not use scanner routes as field tests. They are retained only as compatibility/status responses:

```bash
curl -sS --connect-timeout 10 --max-time 30 \
  http://192.168.1.160:18080/scanner/config
```

## OTA firmware update

OTA support has been flashed over USB. Future updates can be pushed over HaLow when the board HTTP API is reachable.

Build an OTA binary:

```bash
arduino-cli compile \
  --fqbn heltec:esp_halow:HT-HC33:PSRAM=opi \
  --export-binaries \
  /home/matheau/esp32_camera/gardepro_dual_radio_bridge
```

Upload it to the board:

```bash
curl -sS --connect-timeout 10 --max-time 300 \
  -F "firmware=@/home/matheau/esp32_camera/gardepro_dual_radio_bridge/build/heltec.esp_halow.HT-HC33/gardepro_dual_radio_bridge.ino.bin" \
  http://192.168.1.160:18080/firmware/update
```

If `UPSTREAM_API_TOKEN` is configured, include the token:

```bash
curl -sS --connect-timeout 10 --max-time 300 \
  -F "firmware=@/path/to/gardepro_dual_radio_bridge.ino.bin" \
  "http://192.168.1.160:18080/firmware/update?token=TOKEN"
```

The board returns JSON, waits briefly, then restarts into the new image.

## Useful USB serial commands

```text
status
halow_status
halow_up
onboard_status
onboard_capture
sd_mount
sd_status
upload_status
```
