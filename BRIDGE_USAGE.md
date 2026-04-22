# Bridge Usage

## Purpose

`gardepro_bridge.py` is a minimal relay skeleton for the confirmed WiFi-side protocol:

- HTTP control proxy to `192.168.8.1:8080`
- UDP listeners on `49152` and `49153`

It does **not** yet implement BLE activation or the dynamic UDP port negotiation step.

## Run

```bash
python3 /home/matheau/esp32_camera/gardepro_bridge.py
```

This starts:

- HTTP proxy on `http://0.0.0.0:18080/camera/`
- UDP listeners on `0.0.0.0:49152` and `0.0.0.0:49153`

Example:

```bash
curl http://127.0.0.1:18080/camera/cmd/info/1
```

## UDP Forwarding

If you already know where you want media packets forwarded:

```bash
python3 /home/matheau/esp32_camera/gardepro_bridge.py \
  --upstream-media-host 192.168.1.50 \
  --upstream-media-port-primary 6000 \
  --upstream-media-port-secondary 6001
```

## Current Limits

- BLE bootstrap is still external.
- The camera chooses client UDP destination ports dynamically during live-view setup.
- This script reflects confirmed transport facts, but it is not yet a complete end-to-end camera replacement for the Android app.

## GPRT Tunnel Receiver

For the current ESP32 tunnel prototype, run the local receiver on this machine:

```bash
python3 /home/matheau/esp32_camera/gardepro_tunnel_server.py
```

Default behavior:

- listens for the ESP32 TCP tunnel on `0.0.0.0:6000`
- forwards received primary media packets to local UDP `127.0.0.1:5004`
- forwards received secondary media packets to local UDP `127.0.0.1:5005`
- logs `REGISTER`, `START`, and `STOP` metadata frames from the board

Useful options:

```bash
python3 /home/matheau/esp32_camera/gardepro_tunnel_server.py \
  --bind-port 6000 \
  --udp-port-primary 5004 \
  --udp-port-secondary 5005 \
  --verbose
```

For short packet-level diagnostics, add `--verbose-packets`. Avoid leaving that on during normal playback tests because console spam can add local jitter while `ffplay` is decoding.

On `START`, the receiver now writes a local SDP file:

- `/tmp/gardepro_live.sdp`

On `REGISTER`, the receiver now writes the latest board identity/address record:

- `/tmp/gardepro_board_registration.json`

## Server Control

The board can now be controlled through its HTTP bridge using the latest registered HaLow IP.

Example:

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
python3 /home/matheau/esp32_camera/gardepro_server_control.py gallery
python3 /home/matheau/esp32_camera/gardepro_server_control.py info --index 3
python3 /home/matheau/esp32_camera/gardepro_server_control.py camera-request --camera-path /media/getIrStatus
python3 /home/matheau/esp32_camera/gardepro_server_control.py camera-get --camera-path /cmd/getSetting --no-auto-bringup
python3 /home/matheau/esp32_camera/gardepro_server_control.py set-setting-json --json '{"date_format":1}'
python3 /home/matheau/esp32_camera/gardepro_server_control.py set-clock --timestamp '2026-04-21 20:05:00'
python3 /home/matheau/esp32_camera/gardepro_server_control.py take-picture --timeout 45 --poll-interval 1.5
python3 /home/matheau/esp32_camera/gardepro_server_control.py video-stop --timeout 45 --poll-interval 1.5
python3 /home/matheau/esp32_camera/gardepro_server_control.py format-start --timeout 120 --poll-interval 2
python3 /home/matheau/esp32_camera/gardepro_server_control.py media-list
python3 /home/matheau/esp32_camera/gardepro_server_control.py media-latest --media-type 1
python3 /home/matheau/esp32_camera/gardepro_server_control.py media-latest --media-type 2
python3 /home/matheau/esp32_camera/gardepro_server_control.py media-paths --media-id 126 --media-type 1
python3 /home/matheau/esp32_camera/gardepro_server_control.py thumb-download --media-id 126 --media-type 1 --output /tmp/thumb.jpg
python3 /home/matheau/esp32_camera/gardepro_server_control.py media-download --media-id 126 --media-type 1 --output /tmp/photo.jpg
python3 /home/matheau/esp32_camera/gardepro_server_control.py media-download --media-id 112 --media-type 2 --output /tmp/video.mp4
python3 /home/matheau/esp32_camera/gardepro_server_control.py media-delete --media-id 126 --media-type 1
python3 /home/matheau/esp32_camera/gardepro_server_control.py download --camera-path /thumb/126/JPG --output /tmp/thumb.jpg
python3 /home/matheau/esp32_camera/gardepro_server_control.py download --camera-path /file/126/JPG --output /tmp/photo.jpg
python3 /home/matheau/esp32_camera/gardepro_server_control.py download --camera-path /file/112/mp4 --output /tmp/video.mp4
```

By default this resolves the board IP from:

- `/tmp/gardepro_board_registration.json`

Important control behavior:

- `POST /control/bringup`, `POST /control/stream_start`, and `POST /control/stream_stop` are now queued background actions
- the HTTP request returns immediately with `202 Accepted`
- the board executes the requested action in a worker task
- `bringup`, `stream-start`, and `stream-stop` wait on `/status` by default and now accept `--timeout` and `--poll-interval` to tune that wait
- in the current local-serial-mode sketch, HaLow and the HTTP control plane now boot immediately instead of waiting for the camera wake path to finish first
- poll `status` to watch:
  - `control_busy`
  - `control_pending`
  - `control_action`
  - `control_last_action`
  - `control_last_ok`
  - `control_last_message`

Raw camera fetch behavior:

- `camera-get`, `download`, `file-download`, `media-download`, and `thumb-download` now retry once after `bringup` when the board reports a transport-level camera failure such as WiFi-down or connect-timeout
- pass `--no-auto-bringup` to `camera-request`, `camera-get`, or the download commands when you want to see the first failure without that retry
- `camera-get` now prints both the decoded body text and parsed JSON when the camera response is JSON
- successful file payloads are still written only on `2xx`
- real camera/file errors are returned to the caller without being masked
- download commands now print the resolved camera path, output path, write status, and byte count
- `media-download` and `thumb-download` also print the resolved media item and its canonical file/delete paths

Stability work completed in the current sketch:

- idle camera WiFi loss is no longer just logged in local serial mode; the sketch now attempts a throttled idle recovery path
- repeated idle HTTP keepalive failures now increment a counter and can trigger that idle recovery path
- `/status` now exposes:
  - `idle_recoveries`
  - `http_keepalive_failures`
  - `idle_recovery_last_ms`
- the control worker now runs on a separate core from the main loop to reduce starvation during long control actions
- local serial mode now brings up HaLow and the HTTP bridge immediately on boot so remote status/control does not wait for the camera wake path

Settings write behavior:

- `setting-set` and `setting-update-json` send the patch, re-read `/cmd/getSetting`, and report which requested keys actually changed
- `set-setting-json` sends the raw POST body and prints the direct camera response

Media delete behavior:

- `media-delete` now reports the chosen media item's canonical paths, the direct camera delete response, and whether that item still appears in the refreshed gallery afterward

Picture capture behavior:

- `take-picture` now triggers capture, polls `picture-result`, compares the latest photo before and after, and reports whether a new photo was actually observed
- use `--timeout` and `--poll-interval` to tune how long that verification loop waits
- the verified result now also includes `poll_attempts`, `elapsed_sec`, and `timed_out`
- `picture-result` remains available as the raw direct camera query

Video stop behavior:

- `video-stop` now reports the direct camera response plus the latest video item before and after the stop request
- it also reports whether a new completed video was actually observed in the refreshed gallery
- use `--timeout` and `--poll-interval` to tune the post-stop gallery polling
- the verified result now also includes `poll_attempts`, `elapsed_sec`, and `timed_out`

Format behavior:

- `format-start` now records `format-result` before the request, polls `format-result` afterward, and reports whether the result changed or looked complete
- use `--timeout` and `--poll-interval` to tune how long the result polling runs
- the verified result now also includes `poll_attempts`, `elapsed_sec`, and `timed_out`
- `format-result` remains available as the raw direct camera query

Current board-side camera routes:

- `GET /camera/request?method=GET&path=/cmd/...`
- `POST /camera/request?method=POST&path=/cmd/...&content_type=application/json` with raw request body
- `GET /camera/raw?path=/...`
- `GET /camera/info/1..6`
- `GET /camera/getParaSetting`
- `GET /camera/gallery`
- `GET /camera/standby/reset`

Current Python integration surface:

- reusable module:
  - `gardepro_server_api.py`
- CLI wrapper:
  - `gardepro_server_control.py`

Candidate camera path inventory:

- `GARDEPRO_CAMERA_HTTP_CANDIDATES.md`

Recently confirmed through the live board/camera path:

- `setting-values`
- `setting-keys`
- `setting-get`
- `setting-set`
- `setting-update-json`
- `set-setting-json`
- `set-clock`
- `media-latest`
- `media-paths`
- `media-download`
- `thumb-download`
- `media-delete`
- `ir-status`
- `take-picture`
- `picture-result`
- `video-start`
- `video-stop`
- `camera-request --camera-path /cmd/standby/now`
- `camera-request --camera-path /cmd/delete/1/<id>`
- `download --camera-path /thumb/<id>/JPG`
- `download --camera-path /file/<photo_id>/JPG`
- `download --camera-path /file/<video_id>/mp4`

Idle session note:

- the ESP32 sketch now sends periodic `GET /cmd/standby/reset` while camera WiFi is up, even without an active stream
- in live serial observation this held `wifi=up` across repeated keepalive intervals, which is better than the earlier non-stream hotspot expiry behavior
- with `halow_up` issued on the current local-serial-mode build, remote `/status` polling also stayed healthy while the idle keepalives were running

Local playback test:

```bash
ffplay -protocol_whitelist file,udp,rtp /tmp/gardepro_live.sdp
```

## Current Remaining Work

Prioritized next work from the current live state:

1. keep the HTTP control plane responsive during BLE scan / wake so `bringup` can be observed and polled remotely while it runs
2. finish long-run hotspot and stream validation now that idle recovery and immediate HaLow boot are in place
3. validate the newer verified commands against a successfully awake camera:
   - `take-picture`
   - `video-stop`
   - `format-start`
   - `setting-set`
   - `setting-update-json`
   - `media-delete`
4. map the live `/cmd/getSetting` keys and safe values from the real camera
5. confirm the canonical delete syntax with live camera responses for extension-form and legacy type/id paths

Current blocker from the latest live validation:

- the board now comes up on HaLow immediately after boot and `/status` is reachable before `bringup`
- but the latest `bringup` attempt timed out while the board was still in BLE scan
- during that BLE scan window the HTTP control plane became intermittently unavailable, so settings-key mapping and delete-path confirmation could not be completed against an awake camera in this round
