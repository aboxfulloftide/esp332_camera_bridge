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
  - requested static IP `192.168.1.30/24`
  - gateway `192.168.1.1`
- exposes a small local HTTP bridge on port `18080`
- proxies confirmed camera HTTP routes to `192.168.8.1:8080`
- listens for UDP media on local ports `25748/25749`
- can open a single outbound TCP tunnel to the upstream receiver:
  - `192.168.1.39:6000`
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
- in current tests the board comes up as `192.168.1.157`
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
- `/control/bringup`
- `/control/stream_start`
- `/control/stream_stop`
- `/camera/request`
- `/camera/raw`
- `/camera/info/1`
- `/camera/info/2`
- `/camera/info/3`
- `/camera/info/4`
- `/camera/info/5`
- `/camera/info/6`
- `/camera/getParaSetting`
- `/camera/gallery`
- `/camera/standby/reset`

Control behavior is now server-oriented rather than console-oriented:

- the `POST /control/*` routes return quickly with a queued/accepted response
- a background worker task performs the actual BLE / WiFi / RTSP work
- `POST /camera/request` can now forward raw JSON bodies to the camera, for example `POST /cmd/setSetting` and `POST /cmd/setGmtClock`
- `/camera/raw` now dechunks camera file responses before relaying them to the server
- in local serial mode the sketch now boots HaLow and the HTTP control plane immediately instead of waiting for camera wake first
- `/status` exposes control state:
  - `control_busy`
  - `control_pending`
  - `control_action`
  - `control_last_action`
  - `control_last_ok`
  - `control_last_message`

## Current Live-View Result

Local serial mode now proves the camera-side live-view path directly:

- BLE wake brings the hotspot up
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

The main remaining runtime issue is camera hotspot lifetime:

- the tunnel and receiver path are working
- the camera still needs more long-run validation, but the sketch now sends periodic `/cmd/standby/reset` even when no stream is active
- the sketch now auto-recovers by:
  - re-waking the camera over BLE
  - waiting for hotspot return
  - rejoining camera WiFi
  - re-running RTSP
  - re-establishing the tunnel
- in the current sketch, idle camera WiFi loss in local serial mode now also triggers a throttled recovery path instead of only passive rescans
- `/status` now reports:
  - `idle_recoveries`
  - `http_keepalive_failures`
  - `idle_recovery_last_ms`

## Current Test Mode

Right now the sketch is set to local serial mode first, not HaLow mode.

In this mode it:

- performs the exact BLE wake replay
- waits for the hotspot to appear
- joins the camera WiFi
- runs the HTTP self-test
- opens local UDP listeners on:
  - `25748`
  - `25749`
- exposes a serial console with:
  - `help`
  - `status`
  - `selftest`
  - `http <path>`
  - `httpm <METHOD> <path>`
  - `rtsp_probe`
  - `rtsp_live`
  - `stream_start`
  - `stream_stop`
  - `rtsp <METHOD> <url>`
  - `bleclose`
  - `wake`

In local serial mode, `stream_start` now:

- connects HaLow on demand
- runs the proven RTSP live-view sequence
- opens the TCP tunnel to the Pi receiver
- forwards packets seen on local UDP listeners into that tunnel

This is the current preferred mode while validating end-to-end live forwarding.

## Local Receiver

Run the upstream receiver on the Pi at `192.168.1.39`:

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
python3 /home/matheau/esp32_camera/gardepro_server_control.py stream-start --timeout 90 --poll-interval 1
python3 /home/matheau/esp32_camera/gardepro_server_control.py stream-stop --timeout 45 --poll-interval 1
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
- [GARDEPRO_CAMERA_HTTP_CANDIDATES.md](/home/matheau/esp32_camera/GARDEPRO_CAMERA_HTTP_CANDIDATES.md)

That module now:

- resolves the board IP from the registration file
- waits for queued control actions to finish by polling `/status`
- retries raw camera fetches once after `bringup` when the board reports a transport-level camera failure
- returns structured raw camera fetch results with decoded body text and parsed JSON when available
- lets `bringup`, `stream-start`, and `stream-stop` use CLI `--timeout` and `--poll-interval` tuning while they wait on `/status`
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
  - keep the HTTP control plane responsive during BLE scan / wake
  - finish long-run hotspot-lifetime stabilization so recovery is the exception instead of the normal path
  - enumerate more settings keys safely from the live camera
  - confirm the canonical delete syntax with extension/type variants

Latest live validation findings after the current stability patches:

- the local-serial-mode board now returns to HaLow immediately after boot and `/status` is reachable before `bringup`
- a live `bringup` attempt still timed out while the board was in BLE scan
- during that BLE scan window the HTTP control plane became intermittently unavailable
- because the camera never reached WiFi-up in that validation window, live settings-key mapping and live delete-path confirmation remain incomplete in this round

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
