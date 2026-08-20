# Current work handoff: ESP32 trail-camera bridge

Date: 2026-08-20

Board:

```text
trail_esp32
HT-HC33 / ESP32-S3
HaLow HTTP IP: 192.168.1.160
HTTP port: 18080
Trail camera BLE MAC: a4:6d:d4:9e:47:32
Trail camera Wi-Fi SSID: CAM8Z8_A46DD49E4732
Trail camera Wi-Fi password: 1234567890
```

Browser repo:

```text
https://github.com/aboxfulloftide/esp332_camera_bridge
```

## Current flashed firmware

Flashed over USB on 2026-08-20:

```text
e7b41fa Add board HTTP listener recovery
```

This includes:

- `63cc58f Prioritize camera workflows over RF scanning`
- `02208c8 Keep board HTTP responsive during camera failures`

## Problem being fixed

The board can remain associated to the HaLow AP while the board HTTP API stops responding.

The important finding is that AP association is not enough. During the failure:

- HaLow/AP still showed the ESP32 connected.
- Ping could work.
- The ESP32 application could remain alive.
- Serial console could remain responsive.
- But HTTP on port `18080` stopped serving clients or reset/timed out.

This means the failure is in firmware/TCP/WebServer responsiveness, not primarily HaLow RF.

## USB diagnostic finding

On 2026-08-20 the board was brought back and connected over USB before flashing.

Observed:

- Serial port `/dev/ttyUSB0` opened at `115200`.
- ESP32 responded to bootloader query as ESP32-S3.
- After hard reset, firmware booted normally.
- HaLow connected and got `192.168.1.160`.
- Onboard camera captured a scheduled image.
- Serial `status` and `halow_status` worked.
- HTTP was fragile:
  - one raw socket `/status` request returned full HTTP 200 JSON.
  - subsequent curl/parallel HTTP requests timed out or reset.
  - serial still worked afterward.

Conclusion: synchronous `WebServer` / TCP listener can get wedged independently of the rest of the firmware.

## Fixes currently flashed

### HTTP/control-plane hardening

- Added `GET /healthz`.
- Added `POST /system/http_restart`.
- Added serial command `http_restart`.
- Added idle periodic HTTP listener restart every 120 seconds.
- Periodic listener restart is skipped during:
  - OTA
  - pending reboot
  - active control action
  - active stream
- Added `http_service` telemetry:
  - `service_count`
  - `last_service_age_ms`
  - `last_gap_ms`
  - `max_gap_ms`
  - `server_restart_count`
  - `server_last_restart_age_ms`
  - `server_last_restart_reason`
  - `server_idle_restart_ms`
  - `camera_http_keepalive_enabled`
  - `camera_auto_recovery_enabled`

### Camera-priority firmware

Wi-Fi/BLE RF scanning is disabled so it cannot fight trail-camera work.

Disabled execution paths:

- `GET /scan/wifi` returns `410 scanner_disabled`.
- `POST /upload/observations` returns `410 scanner_disabled`.
- `POST /scanner/enable` does not enable scanning.
- RF scanner task is not started at boot.

Retained status/compatibility route:

- `GET /scanner/config`

### Camera jobs

Added first in-memory set-and-forget job API:

```text
GET /jobs
POST /jobs?action=bringup
POST /jobs?action=live_view_start
POST /jobs?action=live_view_stop
POST /jobs?action=trigger_photo
```

Current limitation:

- Jobs queue into the in-memory control worker.
- SD-backed durable job persistence is still planned, not implemented.

### Live view decoupling

Live view is now treated as two layers:

1. Camera-side RTSP/RTP session on ESP32.
2. Optional HaLow tunnel forwarding to `.42`.

If camera RTSP starts but HaLow/tunnel is unavailable, live view can still be considered locally active on the ESP32 instead of immediately failing and closing RTSP.

## Post-flash validation

After flashing `e7b41fa`:

- Boot successful.
- HaLow connected at `192.168.1.160`.
- Serial responsive.
- HTTP server started on `18080`.
- RF scanner task logged disabled.
- Sequential HTTP checks passed:
  - `GET /healthz`
  - `GET /status`
  - `GET /system/status`
  - `GET /jobs`
  - `GET /scanner/config`
- `POST /system/http_restart` worked.
- Follow-up `/healthz` reported:
  - `server_restart_count: 1`
  - `server_last_restart_reason: "api_request"`

Current post-flash state from serial:

- onboard camera ready
- SD mounted
- stored photos: `43`
- latest onboard media: `00000059`
- battery estimate around `3.928V`
- trail-camera Wi-Fi idle/down
- scanner disabled

## What to test next

Run these sequentially, not in parallel:

```bash
curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/healthz
curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/status
curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/jobs
```

Then test camera jobs:

```bash
curl -sS --connect-timeout 5 --max-time 10 \
  -X POST "http://192.168.1.160:18080/jobs?action=bringup"

curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/control/status
curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/ble/status
curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/wifi/status
```

Trigger photo job:

```bash
curl -sS --connect-timeout 5 --max-time 10 \
  -X POST "http://192.168.1.160:18080/jobs?action=trigger_photo"
```

Live view local/tunnel behavior:

```bash
curl -sS --connect-timeout 5 --max-time 10 \
  -X POST "http://192.168.1.160:18080/jobs?action=live_view_start"

curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/stream/status
```

Stop live view:

```bash
curl -sS --connect-timeout 5 --max-time 10 \
  -X POST "http://192.168.1.160:18080/jobs?action=live_view_stop"
```

## Important testing rules

- Do not run scanner tests.
- Do not use `/scan/wifi`.
- Do not use `/upload/observations`.
- Do not bulk-delete onboard media.
- Avoid parallel curl calls against the ESP32.
- Use `/healthz` first when checking board HTTP health.
- If HTTP wedges but serial is available, run serial command:

```text
http_restart
```

## Next implementation work

Highest-value next changes:

1. SD-backed durable job queue.
   - Jobs must survive reboot.
   - Jobs should persist state: `queued`, `running`, `waiting_retry`, `succeeded`, `failed`, `cancelled`, `expired`.
   - First durable jobs should be:
     - `trigger_photo`
     - `download_new_media`
     - `download_all_media`
     - `download_media_id`
     - `live_view_start` with TTL
     - `live_view_stop`

2. Local trail-camera media mirror on ESP32 SD.
   - Download requested trail-camera photo/video files to SD.
   - Upload later when HaLow/backhaul is stable.

3. Camera-side state machine.
   - Replace long blocking bringup steps with small bounded states.
   - Avoid large blocking BLE/Wi-Fi/camera operations from affecting board HTTP.

4. Camera Wi-Fi telemetry.
   - Add camera Wi-Fi RSSI.
   - Add disconnect count/reason if available.

5. More robust HTTP server implementation.
   - Current Arduino `WebServer` is synchronous and fragile under bad TCP/client behavior.
   - Consider moving board API to a dedicated task or replacing with a lower-level non-blocking server if listener restart is not sufficient.

## Key docs

- Laptop setup: `LAPTOP_FIELD_SETUP.md`
- Board HTTP API: `gardepro_dual_radio_bridge/BOARD_HTTP_API.md`
- Onboard media API: `gardepro_dual_radio_bridge/ONBOARD_MEDIA_API.md`
- Field-service plan/checks: `gardepro_dual_radio_bridge/FIELD_SERVICE_FIX_PLAN.md`
