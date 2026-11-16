# Desktop bridge (replaces the ESP32 firmware)

`desktop_bridge/` is a Python service that does the ESP32 bridge's camera work on
this desktop (ASUS X570-Plus, Intel Wireless-AC 9260). It speaks the ESP32 board
HTTP API on port 18080 and the same GPRT tunnel protocol, so the server at
`192.168.1.42` can drive it without changes.

| Radio | Interface | Use |
|---|---|---|
| Camera Wi-Fi | `wlp5s0` | Camera hotspot only (`ipv4.never-default`) |
| Bluetooth | `hci0` | Camera wake (`AT+WAKEPULSE=10`) |
| Uplink | `enp4s0` (192.168.1.41) | Server, REGISTER, live-view tunnel |

## What it does

- **Camera control:** `POST /control/bringup|stream_start|stream_stop`,
  `POST /jobs?action=bringup|live_view_start|live_view_stop|trigger_photo`,
  progress in `GET /control/summary` (firmware field names).
- **Durable jobs:** `POST /jobs?action=download_all_media|download_and_delete_media`.
  One job at a time (409 `durable_job_busy`). Builds the full manifest, then for
  each item: download, verify (size + JPEG/MP4 structure), re-check identity,
  delete, confirm. Uploads to `/api/board/media` afterwards. Retries 3× a minute
  apart, then every 15 min, and fails after 3 h if the camera stays unreachable.
  A job interrupted by a restart resumes. Status: `GET /trail-media/status`.
- **Background upload:** every 30 s, any staged file not yet uploaded is
  uploaded (camera jobs take priority).
- **Live view:** RTSP `DESCRIBE/SETUP/PLAY` with the camera, RTP/RTCP received on
  UDP 25748/25749 and forwarded as GPRT frames to `192.168.1.42:6000`
  (`START`/`STOP` control frames, SDP in `START`). RTSP keepalive every 5 s,
  stall after 12 s without RTP → `control_error: "stream_rtp_stalled"`.
- **Camera proxy:** `/camera/request`, `/camera/raw`, `/camera/gallery`,
  `/camera/latest`, `/camera/info/1..6`, `/camera/getParaSetting`,
  `/camera/standby/reset`.
- **Target:** `GET/POST /camera/target?id=e6_plus|e6_original` (one active camera,
  persisted in `trail_staging/bridge_state.json`; refused while busy).
- **Sessions:** `POST /session/lease?ttl_ms=…`, `POST /session/release`.
- **Status:** `/status`, `/healthz`, `/wifi/status`, `/ble/status`,
  `/timing/status`, `/stream/status`, `/camera/status`, `/upload/status`.
- **Registration:** with `"register": true`, sends the firmware's `REGISTER`
  frame (uplink IP under the `halow_ip` name) at start, every 5 min, and when
  the uplink IP changes, so the server's `/tmp/gardepro_board_registration.json`
  points at this desktop.

Not ported (ESP32 hardware only): HaLow, battery ADC/low-battery hold, onboard
ESP camera, OTA, SD mount/recovery, DHT11, RF scanner, diagnostics journal.

## Running

Installed as a systemd service (runs as `matheau`; nmcli via `sudo -n`):

```bash
sudo systemctl status gardepro-desktop-bridge
journalctl -u gardepro-desktop-bridge -f
sudo systemctl restart gardepro-desktop-bridge
```

Settings: `/etc/gardepro-bridge.json` (template in
`deploy/gardepro-bridge.example.json`; unit file in
`deploy/gardepro-desktop-bridge.service`). Staged media and job state live in
`../trail_staging/`.

The HTTP API binds to the uplink address (`http_host: "uplink"` →
192.168.1.41:18080) because `llama-server` already listens on
`127.0.0.1:18080` on this machine.

Foreground run for testing (no registration):

```bash
.venv/bin/python -m desktop_bridge --config <file.json> --no-register
```

## Taking over from the ESP32

1. Power off the ESP32 bridge (it re-registers on every HaLow connect, and two
   bridges would fight over the camera's single Wi-Fi client slot).
2. Set `"register": true` in `/etc/gardepro-bridge.json` and restart the service.
3. Check `GET /upload/status` → `register.sent` > 0, and on the server that
   `/tmp/gardepro_board_registration.json` shows `halow_ip: 192.168.1.41`.

## Tests

```bash
.venv/bin/python -m unittest tests.test_desktop_bridge -v
```

End-to-end against a fake camera + upload server and the real
`gardepro_tunnel_server.py` (REGISTER, live-view framing, photo trigger, full
download/delete/upload job). No radios needed.

## Differences from the firmware worth knowing

- **Photo trigger:** the firmware stops at the first `/media/pic/result` reply,
  but the E6 first answers `{"code":1,"desc":"on-going"}`. The bridge polls until
  the code changes, then reports the new id (`fileIdx`, or a gallery diff) in
  `last_photo` on `/control/summary`.
- **Bring-up:** a wake can be acknowledged with the hotspot never appearing.
  The bridge wakes again after 75 s instead of trying joins that cannot succeed.
- **Standby after stop:** `stream_stop` and `trigger_photo` put the camera in
  standby and drop its Wi-Fi themselves. The server's `close_session` then sends
  `/cmd/standby/now` through `/camera/request`; with the link already down the
  bridge answers `200 {"code":0,"desc":"already_standby"}` instead of the
  firmware's 503 (deferred while a lease is active, as in the firmware).
- **Keyframe gate (live view):** with `"rtp_keyframe_gate": true` (default) RTP is
  held per frame and only complete frames are forwarded; after any packet loss
  frames are dropped until the next IDR (the E6 sends one every 4.0 s), and
  forwarded packets are renumbered gap-free. The viewer sees a freeze instead of
  smeared video. Real loss stays in `/stream/status` (`rtp_packets_missing`,
  `frames_dropped_damaged`, `frames_dropped_waiting_keyframe`). The loss itself is
  on the camera's 2.4 GHz link (0 kernel `UdpRcvbufErrors`); UDP buffers are 8 MB
  (`/etc/sysctl.d/90-gardepro-udp.conf`).
- **Job JSON:** adds `camera`, `uploaded`, `next_retry_in_s`; battery fields are `null`.
