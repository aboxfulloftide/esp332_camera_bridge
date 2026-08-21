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
- `bringup`, `stream-start`, and `stream-stop` wait on the board status APIs by default and now accept `--timeout` and `--poll-interval` to tune that wait
- `session-close` now polls status after standby instead of taking one immediate snapshot, so delayed WiFi drop is reported correctly
- in the current local-serial-mode sketch, HaLow and the HTTP control plane now boot immediately instead of waiting for the camera wake path to finish first
- poll `/control/status` to watch:
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
- `/stream/status` now exposes:
  - `idle_recoveries`
  - `http_keepalive_failures`
  - `idle_recovery_last_ms`
- `/camera/status` now exposes:
  - `standby_requested`
- the control worker now runs on a separate core from the main loop to reduce starvation during long control actions
- local serial mode now brings up HaLow and the HTTP bridge immediately on boot so remote status/control does not wait for the camera wake path
- after a successful `/cmd/standby/now`, the bridge now suppresses idle `/cmd/standby/reset` keepalives and idle auto-recovery until the next explicit bringup or standby reset

Camera target selection:

- The normal operator path is now website-driven:
  1. open the local GardePro web UI
  2. choose the camera in the `Camera Target` panel
  3. click `Use Selected Camera`
  4. click `Connect`
- The board exposes the same control directly:

```bash
curl -sS http://192.168.1.160:18080/camera/target
curl -sS -X POST "http://192.168.1.160:18080/camera/target?id=e6_original"
curl -sS -X POST "http://192.168.1.160:18080/camera/target?id=e6_plus"
```

- Selecting a different target intentionally closes the cached BLE session and
  disconnects the current trail-camera Wi-Fi session. This avoids sending
  commands to the wrong camera after a target switch.
- Built-in camera profiles:
  - `e6_original`: BLE `a4:6d:d4:9e:47:32`, Wi-Fi `CAM8Z8_A46DD49E4732`
  - `e6_plus`: BLE `a4:c1:38:98:81:48`, Wi-Fi `CAM8Z8_A4C138988148`

Settings write behavior:

- `setting-set` and `setting-update-json` send the patch, re-read `/cmd/getSetting`, and report which requested keys actually changed
- `set-setting-json` sends the raw POST body and prints the direct camera response
- `set-clock` writes through `/cmd/setGmtClock`
- live validation showed `/cmd/setGmtClock` expects a UTC timestamp; sending local Eastern time produced a 4-hour offset in saved media timestamps
- `info --index 4` now provides a direct clock readback path and returned:
  - `clock: 2026-04-23 10:55:33`
  - `tz: US/Eastern`

Media delete behavior:

- `media-delete` now reports the chosen media item's canonical paths, the direct camera delete response, and whether that item still appears in the refreshed gallery afterward
- live validation confirmed delete success for both a disposable photo and a disposable video item

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
- live validation showed the camera can expose an in-progress video early with placeholder `uid: "00000000"`
- the wrapper now treats that placeholder item as the new recording already being observed, so `video-stop` no longer waits for a post-stop ID change that may never happen

Format behavior:

- `format-start` now records `format-result` before the request, polls `format-result` afterward, and reports whether the result changed or looked complete
- use `--timeout` and `--poll-interval` to tune how long the result polling runs
- the verified result now also includes `poll_attempts`, `elapsed_sec`, and `timed_out`
- `format-result` remains available as the raw direct camera query
- live validation confirmed the real post-condition by reading the gallery afterward and observing an empty `data` array

Current board-side camera routes:

- `GET /camera/request?path=/cmd/...`
- `POST /camera/request?method=POST&path=/cmd/...&content_type=application/json` with raw request body
- `GET /camera/raw?path=/...`
- `GET /camera/info/1..6`
- `GET /camera/getParaSetting`
- `GET /camera/gallery`
- `GET /camera/standby/reset`

Current Python integration surface:

- reusable module:
  - `gardepro_server_api.py`
- server-facing short-session job layer:
  - `gardepro_server_jobs.py`
- CLI wrapper:
  - `gardepro_server_control.py`

Short-lived session helpers now available for server integration:

- `open_session(...)`
- `close_session(...)`
- `session(...)` as a context manager for wake -> work -> teardown
- `run_in_session(...)` for one-shot bounded work inside a short session
- convenience one-shot wrappers:
  - `get_settings_in_session(...)`
  - `get_setting_values_in_session(...)`
  - `list_media_in_session(...)`
  - `get_ir_status_in_session(...)`

Candidate camera path inventory:

- `GARDEPRO_CAMERA_HTTP_CANDIDATES.md`
- camera resume checklist:
  - `CAMERA_RETURN_CHECKLIST.md`

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
- `format-start`
- `ir-status`
- `take-picture`
- `picture-result`
- `session-close` with verified WiFi drop
- `video-start`
- `video-stop`
- `info --index 4` direct clock readback
- `camera-request --camera-path /cmd/standby/now`
- `camera-request --camera-path /cmd/delete/1/<id>`
- `download --camera-path /thumb/<id>/JPG`
- `download --camera-path /file/<photo_id>/JPG`
- `download --camera-path /file/<video_id>/mp4`

Idle session note:

- this was useful for diagnosis, but it is not the intended product behavior
- the intended session model is:
  - server requests `session-open` or `bringup`
  - ESP32 wakes and joins the camera
  - ESP32 performs the requested work quickly
  - ESP32 requests standby and now allows camera WiFi to fall away cleanly
  - server requests `session-close`
  - `session-close` stops active live view first, then sends camera standby by default
  - ESP32 disconnects and lets the camera return to low power

Deferred follow-up work:

- no additional work is currently needed on deeper Python-side standby-state regression coverage beyond the added delayed-standby unit test
- no additional work is currently needed on extra board-side serial/log observability for standby transitions
- if that changes later, resume from:
  - `gardepro_server_api.py` delayed-standby polling
  - `gardepro_dual_radio_bridge/gardepro_dual_radio_bridge.ino` `standby_requested` handling
- do not optimize for permanent WiFi/hotspot uptime; optimize for reliable wake, fast actions, and clean teardown

Session helper behavior:

- `session-open` wraps the “wake and prepare the short-lived work window” step
- `session-close` wraps the normal teardown step
- `session-close` returns `before` / `after` status snapshots plus any stream-stop / standby results it invoked
- pass `--no-standby` to `session-close` when you want to stop streaming but intentionally keep the camera session up for a little longer

Local playback test:

```bash
ffplay -protocol_whitelist file,udp,rtp /tmp/gardepro_live.sdp
```

## Current Remaining Work

Prioritized next work from the current live state:

1. validate on-demand session behavior end-to-end:
   - reliable `bringup`
   - requested work only during the active window
   - clean disconnect/teardown after work
2. validate the newer verified commands against a successfully awake camera:
   - `take-picture`
   - `video-stop`
   - `format-start`
   - `setting-set`
   - `setting-update-json`
   - `media-delete`
3. map the live `/cmd/getSetting` keys and safe values from the real camera
4. confirm the canonical delete syntax with live camera responses for extension-form and legacy type/id paths

Firmware-derived candidate settings to test through `POST /cmd/setSetting` once the camera is powered again:

- `standby_timeout`
- `wifi`
- `power_source`
- `screen_timeout`
- `cellular_transfer`
- `instant_upload`

Current firmware clue summary:

- `standby_timeout` appears directly in the newer firmware image
- `wifi`, `power_source`, and `screen_timeout` also appear in config-like string regions
- bind / activation / sleep control is explicit in firmware strings:
  - `bind_success`
  - `WIFI MCU_SUBRD_INT_EN()`
  - `WIFI/AH MCU_SUBRD_INT_CLOSE()`
  - `tc_sleep_ctl(...)`
- this supports using firmware as a clue source for settings/session behavior, not as a patch target

Current live validation state:

- the board boots onto HaLow and `/status` is reachable before camera bringup
- `POST /control/bringup` wakes the camera, joins camera WiFi, and reports
  `control_last_message: "bringup_complete"` on success
- `POST /control/stream_start` can now queue behind active `bringup`; the
  board returns `message: "queued_after:bringup"` instead of dropping the live
  start request as `busy:bringup`
- RTSP `DESCRIBE`, `SETUP`, and `PLAY` are confirmed at status `200`
- camera RTP/RTCP packets are confirmed on the board UDP listeners:
  - primary RTP: local `25748`, source `192.168.8.1:49152`
  - secondary RTCP: local `25749`, source `192.168.8.1:49153`
- `/stream/status` reports:
  - RTSP status codes
  - selected PLAY URL
  - tunnel target and tunnel connect error
  - UDP receive counters
  - tunnel forwarding counters
- the live tunnel target is no longer hard-coded to `192.168.1.39`; it defaults
  to `UPSTREAM_API_HOST:6000` through `UPSTREAM_TUNNEL_HOST` /
  `UPSTREAM_TUNNEL_PORT`
- if there is no upstream receiver listening at the configured tunnel target,
  the board reports `stream_tunnel_connect_failed`; if the receiver is present,
  `stream_status.tunnel_packets_sent` and `stream_status.tunnel_bytes_sent`
  should increase
