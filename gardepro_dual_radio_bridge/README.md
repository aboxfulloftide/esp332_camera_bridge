# GardePro Dual-Radio Bridge

## Intended Hardware Topology

- ESP32 onboard `WiFi` STA -> GardePro camera hotspot
- ESP32 `HaLow` STA -> upstream server / long-range network

This matches your stated board usage:

- normal WiFi to the camera
- HaLow to the server

## What The Sketch Does

- joins the confirmed camera hotspot:
  - SSID `CAM8Z8_A46DD49E4732`
  - password `1234567890`
- joins the configured HaLow upstream network:
  - SSID from local config
  - password from local config
  - IP address assigned by the HaLow network
- exposes a small local HTTP bridge on port `18080`
- proxies confirmed camera HTTP routes to `192.168.8.1:8080`
- listens for UDP media on local ports `25748/25749`
- reads a DHT11 temperature/humidity sensor on `GPIO13`
- can open a single outbound TCP tunnel to the upstream receiver:
  - `UPSTREAM_TUNNEL_HOST:UPSTREAM_TUNNEL_PORT`
  - defaults to `UPSTREAM_API_HOST:6000`
  - protocol magic: `GPRT`
  - media packets forwarded as framed raw RTP / RTCP payloads
  - control frames include:
    - `REGISTER`
    - `START`
    - `STOP`

## HaLow Addressing

Current observed behavior:

- the Heltec HaLow stack accepts the `HaLow.config(...)` call
- but the live associated address is still DHCP-assigned by the AP
- in current field tests the board comes up as `192.168.1.160`
- the live HaLow MAC used for reservation/identity is:
  - `78:72:64:E4:57:00`

Because of that, the current architecture does not assume a fixed ESP32 HaLow IP.
Instead, on every HaLow connect the board sends a `REGISTER` control frame to the
server with the current:

- HaLow IP
- HaLow MAC
- HaLow BSSID
- HaLow RSSI
- HaLow SSID
- HaLow gateway

## Current HTTP Endpoints

- `/status`
- `GET /healthz`
- `POST /system/http_restart`
- `GET /system/status`
- `GET /halow/status`
- `GET /wifi/status`
- `GET /camera/status`
- `GET /timing/status`
- `GET /stream/status`
- `GET /ble/status`
- `GET /control/status`
- `/battery/status`
- `/onboard/status`
- `/onboard/latest.jpg`
- `/onboard/capture`
- `GET /onboard/media`
- `GET /onboard/media/{id}`
- `GET /onboard/media/{id}/thumb`
- `DELETE /onboard/media/{id}`
- `POST /onboard/media/delete_all`
- `/onboard/config`
- `/onboard/timelapse/start`
- `/onboard/timelapse/stop`
- `/scan/wifi`
- `GET /scanner/config`
- `POST /scanner/config`
- `POST /scanner/enable`
- `POST /scanner/disable`
- `GET /jobs`
- `POST /jobs`
- `/upload/status`
- `/upload/telemetry`
- `/upload/observations`
- `/upload/events`
- `/upload/all`
- `GET /upload/ble/last`
- `GET /sd/status`
- `POST /sd/mount`
- `GET /firmware/status`
- `POST /firmware/update`
- `/control/bringup`
- `/control/stream_start`
- `/control/stream_stop`
- `/session/lease`
- `/session/release`
- `/session/status`
- `/camera/request`
- `/camera/raw`
- `/camera/latest`
- `/camera/info/1`
- `/camera/info/2`
- `/camera/info/3`
- `/camera/info/4`
- `/camera/info/5`
- `/camera/info/6`
- `/camera/getParaSetting`
- `/camera/gallery`
- `/camera/standby/reset`

See [BOARD_HTTP_API.md](BOARD_HTTP_API.md) for the current route-by-route board HTTP API reference.

Unified board features:

- onboard ESP camera initializes at boot and captures a JPEG every 30 minutes by default
- every successful capture is persisted to SD when mounted, with LittleFS fallback, using a monotonic media ID and metadata sidecar
- saved media survives reboot; deleting media does not renumber IDs or cause ID reuse
- latest saved onboard JPEG is available at `/onboard/latest.jpg`, including after reboot
- force a fresh onboard capture with `POST /onboard/capture`
- list saved captures with `GET /onboard/media?offset=0&limit=50&sort=newest`
  - `sort=oldest` reverses the order
  - optional `from` and `to` filters accept Unix epoch seconds
- download or delete an item with `GET` or `DELETE /onboard/media/{id}`
- delete all items with `POST /onboard/media/delete_all`; the response reports `deleted` and `failed`
- thumbnail generation is not currently supported; `thumb_path` is `null` and the thumbnail route returns `thumbnail_not_available`
- SD is the intended persistent store for onboard captures and failed observation retry batches; LittleFS is retained only as fallback
- periodic onboard capture now defaults to one JPEG every 30 minutes
- with `PSRAM=opi` enabled, the onboard camera supports UXGA `1600x1200`
- scheduled onboard captures run only inside the configured daylight window
- change the scheduled/default capture profile with `POST /onboard/config`
  - `enabled=true|false`
  - `interval_ms=1800000`
  - `start=06:00` or `start_minute=360`
  - `end=18:00` or `end_minute=1080`
  - `tz_offset_min=-240`
  - `epoch=1718123456` to set the board clock for schedule testing
  - `framesize=SVGA|XGA|SXGA|UXGA`
  - `jpeg_quality=4..63`
  - sensor tuning test fields: `brightness`, `contrast`, `saturation`, `sharpness`, `vflip`, `hmirror`, `awb`, `awb_gain`, `wb_mode`, `aec`, `aec2`, `ae_level`, `aec_value`, `agc`, `agc_gain`, `special_effect`
- trigger a one-shot web UI capture with temporary settings using `POST /onboard/capture` and the same sensor tuning fields
  - example: `POST /onboard/capture?framesize=UXGA&jpeg_quality=8&aec=0&aec_value=2300`
  - one-shot settings are applied for that capture only, then the scheduled/default profile is restored
- timelapse mode temporarily overrides the daylight window and captures every 5 minutes by default
  - start with `POST /onboard/timelapse/start?hours=2`
  - optional duration fields: `hours`, `duration_hours`, `duration_minutes`, `minutes`, or `duration_ms`
  - optional interval override: `interval_ms=300000`
  - stop early with `POST /onboard/timelapse/stop`
  - when the duration expires, the firmware automatically returns to normal onboard camera mode
- battery telemetry reads the HT-HC32/33 battery divider (`GPIO1` / `ADC_IN`, controlled by `GPIO20` / `ADC_Ctrl`)
- idle WiFi scanning is available at `/scan/wifi`
- WiFi/BLE RF scanning is disabled in camera-priority firmware so it cannot compete with trail-camera BLE, trail-camera Wi-Fi, onboard captures, or board HTTP responsiveness
  - scanner routes return disabled compatibility/status responses
  - RF observation upload execution is disabled
  - previous scanner code remains in the source for reference but is not started at boot
- camera jobs are available through `POST /jobs?action=...`
  - supported actions: `bringup`, `live_view_start`, `live_view_stop`, `trigger_photo`
  - jobs currently queue into the in-memory camera control worker; SD-backed durable persistence is planned next
- live view now keeps the ESP32 camera-side RTSP/RTP session active even if HaLow/tunnel forwarding is unavailable
- upstream API upload plumbing is available over HaLow:
  - `GET /upload/status`
  - `POST /upload/telemetry` -> `/api/board/telemetry`
  - `POST /upload/events` -> `/api/board/events`
  - `POST /upload/all` uploads telemetry, then queued events

Upload configuration is supplied by `local_config.h`:

- `BOARD_HOSTNAME` defaults to `trail_esp32`
- `SCANNER_HOST`
- `UPSTREAM_API_HOST` defaults to `192.168.1.42`
- `UPSTREAM_API_PORT` defaults to `80`
- `UPSTREAM_API_PREFIX` defaults to `/trail_cam`, making firmware uploads target `http://192.168.1.42/trail_cam/api/...`
- `UPSTREAM_API_TOKEN`
- `AIR_SCAN_API_HOST` defaults to `192.168.1.42`
- `AIR_SCAN_API_PORT` defaults to `80`; WiFi/BLE observations are posted to
  `http://192.168.1.42/api/observations/upload`

The firmware sets `BOARD_HOSTNAME` on the ESP32 2.4 GHz WiFi station before
joining the trail-camera hotspot. The current Heltec HaLow wrapper does not
expose a DHCP hostname setter, so the same hostname is also included in board
registration, `/status`, telemetry, and event uploads for server-side identity.

Current upload limitation: `recorded_at` is sent as `null` until the unified
firmware has a trusted clock source from NTP, RTC, or the server.

Current onboard schedule behavior: if the clock is valid, scheduled captures use
the configured local-time window. If the clock is not valid, the board still
captures on the configured interval using uptime fallback, then backfills
timestamps after network time syncs.

OTA behavior: after a firmware containing OTA support has been flashed once over
USB, later firmware updates can be uploaded with multipart form field
`firmware` to `POST /firmware/update`. Use `GET /firmware/status` to check OTA
state.

The ESP32-S3 has one 2.4 GHz WiFi radio. Because of that, `/scan/wifi` only runs
when the trail-camera WiFi session is idle; it returns `camera_wifi_active` while
the bridge is connected to the trail-camera hotspot or streaming.

Control behavior is now server-oriented rather than console-oriented:

- the `POST /control/*` routes return quickly with a queued/accepted response
- `POST /control/stream_start` can be queued behind an active `bringup`; the
  accepted response uses `message: "queued_after:bringup"` instead of rejecting
  with `busy:bringup`
- a background worker task performs the actual BLE / WiFi / RTSP work
- `POST /session/lease?ttl_ms=120000` keeps the camera session warm on the board and refreshes on camera proxy activity
- `POST /session/release` clears the lease and can request camera standby
- `GET /camera/latest?type=1` returns one item from a small gallery page instead of relaying the full gallery
- `POST /camera/request` can now forward raw JSON bodies to the camera, for example `POST /cmd/setSetting` and `POST /cmd/setGmtClock`
- `/camera/raw` now dechunks camera file responses before relaying them to the server
- in local serial mode the sketch now boots HaLow and the HTTP control plane immediately instead of waiting for camera wake first
- `/status` is now a compact board-health summary for field checks.
- Detailed runtime state is split across the alternate status endpoints:
  - `/control/status` for queued/active/last control action state
  - `/camera/status` and `/session/status` for camera session and lease state
  - `/timing/status` for bringup phase timings
  - `/stream/status` for RTSP status codes, tunnel target/connect errors, UDP receive counters, and tunnel-forwarding counters
  - `/ble/status` for BLE wake/discovery telemetry and BLE scanner counters
  - `/wifi/status` for trail-camera Wi-Fi state and Wi-Fi scanner counters

## Current Live-View Result

Local serial mode now proves the camera-side live-view path directly:

- BLE wake uses NimBLE-Arduino and brings the hotspot up
- HTTP control on `192.168.8.1:8080` works
- RTSP `DESCRIBE` works on:
  - `rtsp://192.168.8.1/live.sdp`
- RTSP `SETUP` works on:
  - `rtsp://192.168.8.1/live.sdp/track1`
- RTSP `PLAY` works when `DESCRIBE -> SETUP -> PLAY` are kept on one persistent TCP connection
- live RTP arrives on the board-selected UDP ports:
  - `25748` from camera port `49152`
  - `25749` from camera port `49153`

So the remaining gap is not camera protocol discovery anymore. It is turning the proven local RTSP/RTP path into the final forwarding/product behavior.

The product target is not a permanently awake camera session.

Desired runtime model:

- receive a server request
- wake and join the camera on demand
- perform the requested work:
  - live view
  - settings changes
  - media download
  - status queries
- stop and disconnect cleanly
- let the camera return to low power

So the bridge should optimize for reliable wake and short-lived sessions, not permanent hotspot uptime.

The server-side control layer now reflects that model directly:

- `session-open` / `open_session(...)` for the wake/connect step
- perform the requested work while the session is active
- `session-close` / `close_session(...)` for teardown
- `session-close` stops active streaming first and then requests camera standby by default
- pass `--no-standby` if you need to keep the camera awake briefly after stopping stream
- for server code, `run_in_session(...)` and the small one-shot wrappers handle common short-session reads without manual open/close sequencing

The main remaining runtime issue is reliable on-demand session startup:

- the tunnel and receiver path are working
- the camera still needs more long-run validation
- the sketch now auto-recovers by:
  - re-waking the camera over BLE
  - waiting for hotspot return
  - rejoining camera WiFi
  - re-running RTSP
  - re-establishing the tunnel
- in the current sketch, idle camera WiFi loss in local serial mode now also triggers a throttled recovery path instead of only passive rescans
- `/stream/status` now reports:
  - `idle_recoveries`
  - `http_keepalive_failures`
  - `idle_recovery_last_ms`

## Current Test Mode

Right now the sketch is set to local serial mode first, not HaLow mode.

In this mode it:

- performs the exact NimBLE wake replay
- waits for the hotspot to appear
- joins the camera WiFi
- runs the HTTP self-test
- opens local UDP listeners on:
  - `25748`
  - `25749`
- exposes a serial console with:
  - `help`
- `status`
- `dht`
- `selftest`
  - `http <path>`
  - `httpm <METHOD> <path>`
  - `rtsp_probe`
  - `rtsp_live`
  - `stream_start`
  - `stream_stop`
  - `upload_status`
  - `upload_telemetry`
  - `upload_events`
  - `upload_all`
  - `rtsp <METHOD> <url>`
  - `bleclose`
  - `wake`

In local serial mode, `stream_start` now:

- connects HaLow on demand
- runs the proven RTSP live-view sequence
- opens the TCP tunnel to the Pi receiver
- forwards packets seen on local UDP listeners into that tunnel
- reports precise stream failure stage through `/stream/status`

Latest live-board validation:

- `POST /control/bringup` succeeds and joins the camera as `192.168.8.30`
- RTSP `DESCRIBE`, `SETUP`, and `PLAY` return `200`
- camera RTP is received from `192.168.8.1:49152` on local UDP port `25748`
- camera RTCP is received from `192.168.8.1:49153` on local UDP port `25749`
- when the upstream receiver is available at `UPSTREAM_TUNNEL_HOST:UPSTREAM_TUNNEL_PORT`,
  `stream_status.tunnel_packets_sent` and `stream_status.tunnel_bytes_sent`
  increase
- if the upstream receiver is missing, `/stream/status` reports
  `last_stage: "tunnel_connect_failed"` and the socket error code

This is the current preferred mode while validating end-to-end live forwarding.

## Local Receiver

Run the upstream receiver on the server configured as `UPSTREAM_TUNNEL_HOST`
or, by default, `UPSTREAM_API_HOST`:

```bash
python3 /home/matheau/esp32_camera/gardepro_tunnel_server.py --verbose
```

Default local fanout on the Pi:

- primary media -> `127.0.0.1:5004`
- secondary media -> `127.0.0.1:5005`

On `REGISTER`, the receiver writes:

- `/tmp/gardepro_board_registration.json`

That file is the current source of truth for the board's actual HaLow address.

## Server Control

The repo now includes a helper that resolves the board's current HaLow IP from the
registration file and calls the board HTTP bridge automatically:

```bash
python3 /home/matheau/esp32_camera/gardepro_server_control.py status
python3 /home/matheau/esp32_camera/gardepro_server_control.py bringup --timeout 60 --poll-interval 1
python3 /home/matheau/esp32_camera/gardepro_server_control.py session-open --timeout 60 --poll-interval 1
python3 /home/matheau/esp32_camera/gardepro_server_control.py stream-start --timeout 90 --poll-interval 1
python3 /home/matheau/esp32_camera/gardepro_server_control.py stream-stop --timeout 45 --poll-interval 1
python3 /home/matheau/esp32_camera/gardepro_server_control.py session-close --timeout 30 --poll-interval 1
python3 /home/matheau/esp32_camera/gardepro_server_control.py settings
python3 /home/matheau/esp32_camera/gardepro_server_control.py setting-values
python3 /home/matheau/esp32_camera/gardepro_server_control.py setting-keys
python3 /home/matheau/esp32_camera/gardepro_server_control.py setting-get --key date_format
python3 /home/matheau/esp32_camera/gardepro_server_control.py setting-set --key date_format --value-json '1'
python3 /home/matheau/esp32_camera/gardepro_server_control.py setting-update-json --json '{"date_format":1,"time_format":0}'
python3 /home/matheau/esp32_camera/gardepro_server_control.py take-picture --timeout 45 --poll-interval 1.5
python3 /home/matheau/esp32_camera/gardepro_server_control.py video-stop --timeout 45 --poll-interval 1.5
python3 /home/matheau/esp32_camera/gardepro_server_control.py format-start --timeout 120 --poll-interval 2
python3 /home/matheau/esp32_camera/gardepro_server_control.py camera-get --camera-path /cmd/getSetting --no-auto-bringup
python3 /home/matheau/esp32_camera/gardepro_server_control.py media-list
python3 /home/matheau/esp32_camera/gardepro_server_control.py media-paths --media-type 1
python3 /home/matheau/esp32_camera/gardepro_server_control.py gallery
python3 /home/matheau/esp32_camera/gardepro_server_control.py info --index 5
python3 /home/matheau/esp32_camera/gardepro_server_control.py camera-request --camera-path /media/getIrStatus
```

This is intended as the current server-side control path until or unless DHCP
reservation is added for the board's HaLow MAC.

For real server integration, use:

- [gardepro_server_api.py](/home/matheau/esp32_camera/gardepro_server_api.py)
- [gardepro_server_jobs.py](/home/matheau/esp32_camera/gardepro_server_jobs.py)
- [GARDEPRO_CAMERA_HTTP_CANDIDATES.md](/home/matheau/esp32_camera/GARDEPRO_CAMERA_HTTP_CANDIDATES.md)
- [CAMERA_RETURN_CHECKLIST.md](/home/matheau/esp32_camera/CAMERA_RETURN_CHECKLIST.md)

That module now:

- resolves the board IP from the registration file
- exposes `open_session(...)`, `close_session(...)`, `session(...)`, and `run_in_session(...)` for short-lived session handling
- waits for queued control actions to finish by polling the board status APIs
- retries raw camera fetches once after `bringup` when the board reports a transport-level camera failure
- returns structured raw camera fetch results with decoded body text and parsed JSON when available
- lets `bringup`, `stream-start`, and `stream-stop` use CLI `--timeout` and `--poll-interval` tuning while they wait on board status APIs
- uses verified settings updates for `setting-set` and `setting-update-json` by re-reading `/cmd/getSetting` after the write
- verifies picture capture by polling `picture-result` and checking for a new latest photo
- verifies video stop by checking whether a new latest video appears after the stop request
- verifies SD format start by polling `format-result` after the start request
- exposes CLI `--timeout` and `--poll-interval` tuning for the verified poll-based actions
- includes poll timing metadata in verified action results so live tests show attempts, elapsed time, and timeout state
- exposes CLI `--no-auto-bringup` for raw camera requests and downloads when first-failure diagnostics matter
- verifies media deletes against a refreshed gallery view after the camera responds
- returns structured metadata for raw and typed downloads, including resolved camera paths and byte counts
- keeps the older tuple-returning download helpers as compatibility wrappers over the same structured path
- exposes read-only helpers for:
  - status
  - bringup
  - stream start/stop
  - settings
  - setting values
  - gallery
  - info slots
  - IR status
  - picture trigger / result
  - video start / stop
  - format start / result
  - reboot / factory reset / standby
  - generic camera GET requests
  - generic camera POST JSON requests
  - raw file download
  - typed media listing / latest-item lookup
  - canonical media file / thumbnail / delete path resolution
  - typed media download / thumbnail download
  - typed media delete with extension-form primary and legacy fallback

## Current State

- bridge sketch with tunnel mode was compiled and flashed successfully to the attached `HT-HC33`
- local WiFi + BLE + RTSP live-view path was already proven before adding tunnel mode
- Pi-side receiver script now records both registration and stream metadata
- server-side control helper now resolves the live board IP from registration and can issue:
  - `status`
  - `bringup`
  - `stream-start`
  - `stream-stop`
- current next work is not control discovery anymore
- live confirmation now covers:
  - settings writes via `POST /cmd/setSetting`
  - clock set via `POST /cmd/setGmtClock`
  - photo download via `/file/<id>/JPG`
  - video download via `/file/<id>/mp4`
  - thumbnail download via `/thumb/<id>/JPG`
  - delete via `/cmd/delete/1/<id>` and `/cmd/delete/<id>/1`
- current next work is narrowing the remaining gaps:
  - finish reliable on-demand wake/connect behavior
  - keep active sessions short and teardown explicit
  - enumerate more settings keys safely from the live camera
  - confirm the canonical delete syntax with extension/type variants

Firmware-derived setting candidates worth testing through the existing HTTP settings path:

- `standby_timeout`
- `wifi`
- `power_source`
- `screen_timeout`
- `cellular_transfer`
- `instant_upload`

Firmware clue summary:

- the newer firmware image contains explicit strings for:
  - `standby_timeout`
  - `wifi`
  - `power_source`
  - `screen_timeout`
  - bind / activation success
  - WiFi-side sleep / standby control (`tc_sleep_ctl`, WiFi MCU control strings)
- this makes firmware useful as a clue source for session/power behavior without patching or reflashing the camera firmware itself

Latest live validation findings after the current stability patches:

- the trail camera was powered on for the latest NimBLE validation
- the local-serial-mode board now returns to HaLow immediately after boot and `/status` is reachable before `bringup`
- `/status` now remains reachable while `bringup` is actively running, and the Python control client now gets a board-side `bringup_failed` result instead of timing out waiting for completion
- the local serial loop now services HTTP much more frequently, and the long BLE / WiFi wait paths now yield cooperatively instead of sleeping in large blocking chunks
- idle WiFi recovery is now gated off during active control actions, so manual `bringup` no longer races the background recovery path
- `/ble/status` now exposes BLE scan telemetry including scan mode, total advertisements seen, target-hit count, and strongest/last seen advertisers
- targeted NimBLE validation saw the trail camera advertisement and successfully woke it:
  - target `a4:6d:d4:9e:47:32`
  - name `CAM8Z8_NoName_G_E6`
  - advertised service `6e000100-b5a3-f393-e0a9-e50e24dcca9e`
  - wake write `AT+WAKEPULSE=10\r\n` to `6e400004`
  - three `OK` notifications received on `6e400004`
- second-camera bench validation on 2026-08-20 confirmed a GardePro E6+ uses
  the same BLE wake and camera HTTP API:
  - BLE MAC `a4:c1:38:98:81:48`
  - BLE name `CAM8Z8_NoName_G_E6+`
  - Wi-Fi SSID `CAM8Z8_A4C138988148`
  - camera HTTP `192.168.8.1:8080`
  - `/cmd/info/1` reports brand `GardePro`, product/model `E6+`,
    version `V82.2.152 MCU V84`
  - `/media/pic/take` returned OK and `/media/pic/result` returned
    `fileIdx: 1`
- bridge firmware now tracks the active camera target from BLE discovery and
  derives the Wi-Fi SSID from the selected camera MAC, so a second E6/E6+
  camera can be used without changing the hardcoded Wi-Fi SSID
- serial `camera_target [ble_mac] [wifi_ssid]` can force a specific camera
  during USB debugging; if `wifi_ssid` is omitted the firmware derives
  `CAM8Z8_<BLE_MAC_WITHOUT_COLONS>`
- unified firmware validation after the NimBLE migration succeeded:
  - board hostname `trail_esp32`
  - HaLow IP `192.168.1.160`
  - NimBLE wake stage `wake_ok`
  - trail camera hotspot became visible after about `9.8s`
  - board joined trail-camera WiFi as `192.168.8.30`
  - camera HTTP keepalive `/cmd/standby/reset` returned `200`
  - `/status` was reachable over HaLow and reported both HaLow and trail-camera
    WiFi connected
  - `/status` reported ESP32 internal `chip_temperature_c`
  - `POST /upload/all` over board HTTP succeeded against
    `192.168.1.42/trail_cam/api`, returning `telemetry_id=3`, inserting `2`
    events, and clearing the board event queue
- the previous BLE target discovery / wake reliability blocker is cleared for
  the powered-camera test setup

## Build

```bash
/home/matheau/esp32_camera/build.sh /home/matheau/esp32_camera/gardepro_dual_radio_bridge /dev/ttyUSB0
```

Adjust `/dev/ttyUSB0` if your board enumerates differently.

## Local Config

The published sketch does not include the real HaLow credentials.

Create:

- `gardepro_dual_radio_bridge/local_config.h`

Starting from:

- `gardepro_dual_radio_bridge/local_config.example.h`
