# Trail ESP32 Board HTTP API

Base URL: `http://<board-ip>:18080`

Current field unit address: `http://192.168.1.160:18080`

The board HTTP server is intentionally small. Prefer sequential requests with conservative timeouts on the HaLow link, especially during BLE wake, camera Wi-Fi join, media download, and live view. Current firmware services the HTTP listener from a dedicated task so long control-worker waits, BLE wake, and camera hotspot polling do not starve basic status endpoints.

## Status endpoints

### `GET /status`

Compact board-health summary for field checks. This endpoint is intentionally smaller than the older monolithic status payload.

Representative fields:

- `uptime_ms`
- `hostname`
- `boot_count`
- `boot_session_id`
- `halow_connected`
- `halow_ip`
- `halow_rssi`
- `wifi_connected`
- `camera_target_wifi_ssid`
- `camera_target_ble_mac`
- `clock_valid`
- `storage_type`
- `storage_ready`
- `sd_ready`
- `scanner_schedule_enabled`
- `stored_photo_count`
- `latest_media_id`
- `onboard_captures`
- `schedule_mode`
- `http_service`
- `observation_queue`
- `wifi_scanner`
- `ble_scanner`

Detailed data formerly returned by `/status` is split across the endpoints below.

### `GET /healthz`

Minimal board HTTP health check. Use this when diagnosing whether the ESP32 application HTTP loop is still responsive while the HaLow AP still shows the board associated.

Representative fields:

- `ok`
- `uptime_ms`
- `hostname`
- `halow_connected`
- `halow_ip`
- `http_service`

`http_service` reports:

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

Current firmware defaults both automatic camera HTTP keepalive and automatic camera recovery to disabled so an unreliable trail-camera connection cannot run blocking recovery work from the board main loop. Explicit `POST /control/bringup` and `POST /control/stream_start` still run through the control worker while status endpoints continue to be serviced by the HTTP task.

### `GET /system/status`

System runtime: uptime, hostname, boot counters, PSRAM, and chip temperature.

### `POST /system/http_restart`

Restarts the board HTTP listener without rebooting the ESP32. This is a defensive recovery endpoint for the observed failure mode where serial and HaLow remain alive but the synchronous WebServer/TCP listener stops serving new HTTP clients.

The firmware also periodically re-begins the HTTP listener while idle. It skips the periodic restart during OTA, pending restart, active control actions, and active stream sessions.

### `GET /halow/status`

HaLow uplink state: connected/status, SSID/BSSID/MAC/IP/gateway, RSSI, event counters, and automatic recovery diagnostics. Firmware emits a gratuitous ARP every five seconds so the HaLow AP and LAN clients retain a usable mapping for the board even when inbound ARP replies become unreliable; counters are exposed as `gratuitous_arp_count` and `gratuitous_arp_failures`. It also probes the configured upstream HTTP host every 15 seconds. Two consecutive outbound data-plane failures force a clean Morse station disable/reconnect. Relevant fields include `liveness_probe_successes`, `liveness_probe_failures`, `consecutive_liveness_failures`, `last_liveness_error`, `last_liveness_probe_age_ms`, and `forced_reconnects`. Idle camera-Wi-Fi recovery is suppressed while a durable media job is active so two cores cannot concurrently reconfigure the camera radio.

Current Heltec HaLow wrapper does not expose SNR, noise, or rate-control details; those fields may be `null` with explanatory note fields.

### `GET /wifi/status`

Trail-camera 2.4 GHz Wi-Fi and Wi-Fi scanner state.

Important fields:

- `wifi_connected`
- `wifi_ip`
- `scheduled_scans_enabled`
- `wifi_scan_busy`
- `wifi_scan_last_count`
- `wifi_scan_last_age_ms`
- `scanner`

The firmware currently does not expose trail-camera Wi-Fi RSSI here. BLE RSSI for camera wake/discovery is available from `/ble/status`.

### `GET /camera/status`

Trail-camera bridge state:

- `camera_ip`
- `camera_target_ble_mac`
- `camera_target_ble_name`
- `camera_target_wifi_ssid`
- `camera_wifi_ever_connected`
- `standby_requested`
- `session`

### `GET /camera/target`

Returns the active trail-camera target and the built-in known camera profiles.

Representative response shape:

```json
{
  "ok": true,
  "target": {
    "active": {
      "ble_mac": "a4:c1:38:98:81:48",
      "ble_name": "CAM8Z8_NoName_G_E6+",
      "wifi_ssid": "CAM8Z8_A4C138988148"
    },
    "profiles": [
      {
        "id": "e6_original",
        "label": "GardePro E6 original",
        "ble_mac": "a4:6d:d4:9e:47:32",
        "ble_name": "CAM8Z8_NoName_G_E6",
        "wifi_ssid": "CAM8Z8_A46DD49E4732",
        "selected": false
      }
    ],
    "recent_ble_devices": []
  }
}
```

### `POST /camera/target`

Selects which trail camera the bridge should wake/connect to.

Accepted inputs:

- query/form fields, or JSON body
- `id` matching one of the built-in profiles, for example `e6_original` or `e6_plus`
- or explicit `ble_mac` plus optional `wifi_ssid` and `ble_name`

Examples:

```bash
curl -sS -X POST "http://192.168.1.160:18080/camera/target?id=e6_plus"

curl -sS -X POST "http://192.168.1.160:18080/camera/target" \
  -H 'Content-Type: application/json' \
  -d '{"ble_mac":"a4:c1:38:98:81:48"}'
```

If `wifi_ssid` is omitted, the firmware derives it from the BLE MAC as
`CAM8Z8_<BLE_MAC_WITHOUT_COLONS>`.

Changing target closes any cached BLE wake session and disconnects from the
current camera Wi-Fi. After selecting a target, run `POST /control/bringup` or
`POST /jobs?action=bringup`.

### `GET /timing/status`

Last bringup timing:

- `last_bringup_elapsed_ms`
- `last_ble_wake_elapsed_ms`
- `last_hotspot_wait_elapsed_ms`
- `last_wifi_join_elapsed_ms`
- `last_camera_http_elapsed_ms`

### `GET /stream/status`

Live-view / RTSP / tunnel state.

Important fields:

- `stream_status`
- `stream_active`
- `tunnel_connected`
- `stream_status.rtp_flowing`
- `stream_status.rtp_stalled`
- `stream_status.stall_fail_ms`
- `stream_status.stall_count`
- `stream_status.rtp_packets_received`
- `stream_status.rtp_packets_forwarded`
- `stream_status.rtp_sequence_gaps`
- `stream_status.rtp_packets_missing`
- `stream_status.rtp_out_of_order`
- `stream_status.rtp_forward_failures`
- `stream_status.udp_receive_overruns`
- `stream_status.last_rtp_sequence`
- `stream_status.rtp_loss_pct`
- `stream_status.rtp_keyframe_seen`
- `stream_status.rtp_keyframes_observed`
- `recoveries`
- `idle_recoveries`
- `http_keepalive_failures`
- `media_primary_packets`
- `media_primary_bytes`
- `media_secondary_packets`
- `media_secondary_bytes`

Firmware 0.2.7 adds these fields inside `stream_status`:

- `stream_session_id`
- `rtp_receiving` (recent RTP arrival, independent of loss quality)
- `rtp_quality_ok` and `rtp_quality_limit_pct`
- boot-lifetime `*_total` RTP/overrun counters that survive stream recovery
- `forward_queue` capacity, depth, high-water, drop, and tunnel-write metrics
- `recovery` reason, result, age, and duration
- actual `receive_buffer_bytes` under `udp_primary` and `udp_secondary`

See [STREAM_SERVER_UPDATE.md](../STREAM_SERVER_UPDATE.md) for the server state
model and compatibility rules. `rtp_flowing` remains for compatibility and is
true only when RTP is arriving, being forwarded, and within the loss limit.

During active live view, treat RTP packet movement as the functional signal. The webserver should compare `media_primary_packets`, `stream_status.tunnel_packets_sent`, and `stream_status.udp_primary.last_packet_age_ms` across polls. If `stream_active=true` but `rtp_stalled=true`, the camera-side RTP stream has stopped and the UI should fail or restart live view instead of continuing to show "waiting for relay".

### `GET /control/summary`

Lightweight connectivity, control-worker, durable-job, and live-view state. Live-view
consumers should use the explicit fields:

- `live_view_active` — authoritative trail-camera live-view session state
- `tunnel_connected` — whether the HaLow stream tunnel is currently connected

Do not infer live-view state from `control_last_action`. A later unrelated control action
can replace that bookkeeping value while live view remains active. Servers supporting
older firmware can fall back to `GET /stream/status`; if neither endpoint supplies an
explicit state, report it as unknown.

### `GET /ble/status`

Trail-camera BLE wake/discovery plus BLE scanner counters.

Important fields:

- `camera_target_ble_mac`
- `camera_target_ble_name`
- `camera_target_wifi_ssid`
- `ble_wake_attempted`
- `ble_wake_confirmed`
- `ble_stage`
- `ble_scan_results`
- `ble_target_seen_count`
- `ble_connect_attempts`
- `ble_last_connect_error`
- `ble_last_seen_mac`
- `ble_last_seen_name`
- `ble_last_seen_rssi`
- `ble_best_seen_mac`
- `ble_best_seen_name`
- `ble_best_seen_rssi`
- `ble_recent_devices`
- `ble_notify_count`
- `ble_last_notify`
- `scanner`

Camera BLE signal strength is currently exposed as `ble_last_seen_rssi` / `ble_best_seen_rssi`.

Camera target behavior:

- Default target remains the original E6 profile unless overridden in `local_config.h`.
- The bridge can discover compatible GardePro cameras by BLE name prefix `CAM8Z8_` or advertised service `6e000100-b5a3-f393-e0a9-e50e24dcca9e`.
- When a compatible camera is selected from BLE advertising, the firmware derives the Wi-Fi SSID from the BLE MAC. Example: `a4:c1:38:98:81:48` -> `CAM8Z8_A4C138988148`.

### `GET /control/status`

Background action queue state: busy/pending/current/last action, last result, active time, finished age, and message.

Absent action/error/message fields are JSON `null`, not sentinel strings such
as `"none"`.

Important fields:

- `control_busy`
- `control_state`
- `control_pending`
- `control_action`
- `control_last_action`
- `control_last_ok`
- `control_error`
- `control_message`
- `control_progress`
- `control_progress_text`
- `control_active_ms`
- `control_last_finished_ms`

### `GET /control/summary`

Lightweight polling endpoint for the webserver/PHP/Python wait loops. Prefer
this endpoint over `/status` while waiting for `bringup`, `stream_start`,
`stream_stop`, or `trigger_photo`.

Representative response:

```json
{
  "ok": true,
  "control_busy": false,
  "control_state": "connected",
  "control_action": null,
  "control_pending": null,
  "control_last_action": "bringup",
  "control_last_ok": true,
  "control_error": null,
  "control_message": "bringup_complete",
  "control_progress": "camera_wifi_connected",
  "control_progress_text": "Camera Wi-Fi connected",
  "eta_sec": 0,
  "wifi_connected": true,
  "stream_active": false,
  "tunnel_connected": false,
  "ble_stage": "wake_ok"
}
```

Use `control_error` for fail-fast behavior. It is `null` when there is no
current/last failure.

### `GET /battery/status`

Battery and charger pins:

- `adc_pin`
- `adc_ctrl_pin`
- `adc_ctrl_active_level`
- `adc_ctrl_idle_level`
- `adc_ctrl_idle_after_read`
- `adc_mv`
- `battery_est_v`
- `charging_gpio15`
- `done_gpio16`

The battery ADC control pin is asserted only while sampling and returned to
`adc_ctrl_idle_level` after each read. This avoids leaving the battery-divider
or ADC enable circuit active between status polls.

Low-voltage boot recovery:

- If boot voltage is below `3400 mV`, the firmware enters a low-battery boot hold before starting HaLow, BLE, trail-camera Wi-Fi, SD-heavy work, or the onboard camera.
- If the previous boot failed early with battery below `3400 mV`, the next boot waits for a stronger recovery threshold of `3600 mV`.
- The voltage must be at/above the recovery threshold for 3 consecutive checks before the board restarts for a clean normal boot.
- During the hold the board intentionally does not expose the normal HTTP API because HaLow is not started. Progress is visible over serial.
- If it remains in hold for 30 minutes, it performs a timed restart and checks again.

After a successful boot, `/diagnostics/status` includes `low_battery_boot_hold`
with the thresholds and the previous-boot low-voltage-wedge flag.

### `GET /diagnostics/log`

Streams the SD-backed diagnostic journal at `/diagnostics/health.jsonl` as
newline-delimited JSON. Use this after a recovery to inspect boot, health, and
operational checkpoints.

Query parameters:

- `previous=1`: stream `/diagnostics/health.previous.jsonl` when the active log
  has rotated

### `POST /system/restart`

Requests a full ESP32 software restart. Use this only when a subsystem is
wedged and narrower recovery endpoints are insufficient.

### `GET /onboard/status`

Onboard ESP camera configuration, schedule, power state, storage, and timelapse state. Scheduled capture defaults to once per day in the configured noon window (`12:00`–`12:30`) using the selected sunny-day settings. Outside that window the firmware powers the onboard camera down and leaves stored media available from SD/LittleFS. See [ONBOARD_MEDIA_API.md](ONBOARD_MEDIA_API.md) for media-specific details.

Power/schedule fields include:

- `ready`
- `power_state`
- `powered_down_by_schedule`
- `power_up_count`
- `power_down_count`
- `enabled`
- `interval_ms`
- `window_start`
- `window_end`
- `clock_valid`
- `schedule_mode`
- `window_active`

### `GET /upload/status`

Board telemetry/event upload and RF observation retry state. Firmware currently targets board telemetry/events under `http://192.168.1.42/trail_cam/api/...` and RF observations at `http://192.168.1.42/api/observations/upload`.

### `GET /session/status`

Trail-camera session lease state. Use this when coordinating short-lived camera operations from the server.

### `GET /sd/status`

SD card mount/storage details: ready state, durable media-job load state, mount counters, `last_mount_frequency_hz`, background recovery counters, pins, card type/size, and cached storage totals. Capacity values are cached so status polling cannot contend with an active media transfer. `durable_media_job_loaded` must be `true` before a durable trail-media job can be accepted. Boot and remote recovery use multiple settle-and-retry attempts, falling back from 4 MHz to 400 kHz when necessary. If every boot attempt fails, firmware continues trying one frequency every 15 seconds after networking starts; a successful background mount automatically loads and resumes the durable job.

### `GET /scanner/config`

Scheduled Wi-Fi/BLE scan configuration. In camera-priority firmware this feature is disabled so it cannot compete with trail-camera BLE, trail-camera Wi-Fi, onboard capture, or board HTTP responsiveness.

### `GET /firmware/status`

OTA status: firmware name/version, update endpoint, in-progress flag, last result, bytes written, errors, and free sketch space.

## Control endpoints

### `POST /control/bringup`

Queues camera bringup: BLE wake, camera hotspot wait, trail-camera Wi-Fi join, and camera HTTP verification.

The accepted response includes:

- `eta_sec`
- `poll_path`, currently `/control/summary`

Poll `/control/summary` for wait-loop completion and fail-fast errors. Use
`/wifi/status`, `/ble/status`, and `/timing/status` for deeper diagnostics.

### `POST /control/stream_start`

Queues live-view start. This can queue behind active bringup.

The accepted response includes `eta_sec` and `poll_path`.

Live view is split into two independent states:

- camera-side RTSP/RTP session on the ESP32
- optional HaLow tunnel forwarding to the upstream receiver

If the trail-camera RTSP session starts but HaLow or the tunnel is unavailable, the job still succeeds as a local camera stream. `/stream/status` then reports camera packet counters while `tunnel_connected` remains `false`.

If the camera stops sending RTP after live view starts, the firmware now marks the stream stalled, stops the stale local/tunnel session, and exposes `control_error: "stream_rtp_stalled"` through `/control/status` so callers can fail fast.

### `POST /control/stream_stop`

Stops active live view and closes the tunnel path.

The accepted response includes `eta_sec` and `poll_path`.

### `POST /session/lease`

Keeps the camera session warm on the board.

Useful query parameters:

- `ttl_ms`
- `standby_on_expire`

### `POST /session/release`

Releases the camera session lease.

## Trail-camera proxy endpoints

These routes require the camera Wi-Fi session to be up.

### `GET /camera/request`

Generic camera request proxy. Common query parameters:

- `path`: camera-side path, for example `/cmd/getSetting`
- `method`: optional method override; defaults to `GET`
- `content_type`: optional content type for proxied POST bodies

### `POST /camera/request`

Forwards a raw POST body to the camera. Used for camera settings and clock updates.

### `GET /camera/raw?path=...`

Relays binary files from the camera and dechunks camera responses before returning them.

Known camera paths:

- photo: `/file/<id>/JPG`
- video: `/file/<id>/mp4`
- thumbnail: `/thumb/<id>/JPG`

### Convenience camera routes

- `GET /camera/latest`
- `GET /camera/latest?type=1`
- `GET /camera/info/1`
- `GET /camera/info/2`
- `GET /camera/info/3`
- `GET /camera/info/4`
- `GET /camera/info/5`
- `GET /camera/info/6`
- `GET /camera/getParaSetting`
- `GET /camera/gallery`
- `GET /camera/standby/reset`

Trail-camera still capture uses:

- trigger: `/camera/request?method=GET&path=/media/pic/take`
- poll: `/camera/request?method=GET&path=/media/pic/result`

Some camera firmware returns `fileIdx:0` even when the capture succeeded. In that case, compare gallery before/after and use the newest new type-`1` item.

## Camera jobs

### `GET /jobs`

Returns the current camera worker state plus the SD-backed durable media job.

Current supported actions:

- `bringup`
- `live_view_start`
- `live_view_stop`
- `trigger_photo`
- `download_all_media`
- `download_and_delete_media`

### `POST /jobs`

Queues camera work and returns immediately. Media download jobs persist their state and
camera manifest on SD, resume after resets, and retry camera bring-up without requiring
HaLow. Download-and-delete verifies the staged SD file before deleting each camera item.
Firmware 0.2.8 paginates until the trail camera returns an empty gallery page; it does
not assume the camera honors the requested page size. Therefore both durable download
actions cover the complete camera gallery rather than only its newest 40-item page.

Power behavior:

- `trigger_photo` brings the trail camera up only for the photo when no explicit session lease is active, then requests standby and turns the ESP32 2.4 GHz Wi-Fi radio off.
- Active session leases keep the trail-camera Wi-Fi path available until the lease expires.
- Expired leases are processed in normal field mode and request standby/power-down.
- Durable media jobs keep the trail-camera Wi-Fi path active while transferring, but request standby and power the ESP32 2.4 GHz Wi-Fi radio off between retries and after completion.
- Live view keeps the trail-camera Wi-Fi path active until `live_view_stop` / `stream_stop`.

Examples:

```bash
curl -sS -X POST "http://192.168.1.160:18080/jobs?action=trigger_photo"
curl -sS -X POST "http://192.168.1.160:18080/jobs?action=live_view_start"
curl -sS -X POST "http://192.168.1.160:18080/jobs?action=live_view_stop"
curl -sS -X POST "http://192.168.1.160:18080/jobs?action=download_all_media"
curl -sS -X POST "http://192.168.1.160:18080/jobs?action=download_and_delete_media"
```

## Staged trail-camera media

- `GET /trail-media/status` — durable job progress and cached SD free space. The job object also reports `item_bytes_received`, `item_expected_bytes`, `item_elapsed_ms`, and `item_progress_age_ms` while an individual file is transferring, allowing the server to distinguish a slow large file from a stalled transfer.
- `GET /trail-media/files` — files safely staged on the ESP32 SD card
- `GET /trail-media/file/<filename>` — download a staged file over HaLow when available

Staged files remain on the ESP32 until a separate retention/upload policy removes them.

The firmware also uploads staged trail-camera files outbound so operation does not depend on inbound HaLow TCP. When no durable camera job is active, it selects an unmarked staged file, calculates SHA-256, posts metadata to `POST <UPSTREAM_API_PREFIX>/api/board/media`, then streams the file with `PUT` to the returned `upload_url`. A successful PUT creates a `<media-file>.uploaded` marker on SD; the original remains staged. Failures retain the file and retry after 30 seconds. The server must return an `upload_url`; metadata acceptance alone is not treated as a completed transfer. Progress and retry state are exposed under `trail_media` in `GET /upload/status`.

Durable camera jobs have priority over staged uploads. Accepting a durable job preempts any active upload during transfer-Wi-Fi association, hashing, or blob transmission, and the uploader remains blocked with `blocked_reason: "camera_job_active"` until the camera job is terminal. This prevents the background uploader from starving camera-to-SD writes or holding stale active state across a job submission.

Durable media jobs enter `waiting_retry` with stage `low_battery_wait` below 3.8 V. Camera Wi-Fi/BLE activity is shut down, the manifest and current position remain durable, and the low-voltage hold does not increment retry counters. Firmware rechecks every five minutes and resumes only after battery voltage reaches 3.9 V. The job JSON exposes `battery_mv`, `minimum_battery_mv`, and `recovery_battery_mv`.

Firmware 0.2.25 separates camera-job safe mode from storage/upload recovery. Repeated HTTP failures may suspend new camera work, but SD remount attempts continue with exponential backoff from 15 seconds to 10 minutes. After the SD and durable job state recover, already-staged media resumes uploading automatically. SD recovery waits below 3.5 V and media uploads wait below 3.6 V to avoid high-current storage/network work on a collapsing battery.

The durable job summary is mirrored periodically to NVS (on state changes, every ten completed entries, or every five minutes). If SD is temporarily unavailable, job JSON reports `state_source: "nvs_mirror"` instead of incorrectly appearing as a new idle job.

When transfer Wi-Fi is configured, staged uploads prefer that 2.4 GHz connection and bind their sockets specifically to it. If association fails, the same upload falls back to HaLow. `GET /upload/status` reports `preferred_transport`, `fallback_transport`, battery threshold, and a machine-readable `blocked_reason` such as `sd_unavailable`, `battery_below_upload_threshold`, or `camera_job_active`.

### `POST /wifi/transfer_test`

Performs a bounded, non-media transfer-network test. It refuses to run while the camera radio, live view, a control action, or an upload is active. The board associates with the configured 2.4 GHz SSID, binds a test socket to that Wi-Fi interface, connects to the configured API host/port, reports IP/gateway/RSSI/channel and the data-plane result, then disconnects from 2.4 GHz. No media or camera state is changed.

If `TRANSFER_WIFI_SSID` and `TRANSFER_WIFI_PASS` are configured, staged-media uploads first try that normal 2.4 GHz network for higher throughput and fall back to HaLow if association fails. The camera and transfer networks share the ESP32 Wi-Fi radio, so transfer Wi-Fi is only used when no camera job, control action, or live stream is active. Transfer-network state is exposed under `transfer_wifi` in `GET /upload/status`.

Directory scanning, SHA-256 hashing, and blob streaming yield to the CPU idle task between SD chunks. This prevents the task watchdog from resetting the board when a large staged directory is processed.

Because the HaLow and normal Wi-Fi networks may use the same IPv4 subnet, HaLow remains the management/default interface while staged-upload sockets are explicitly bound to the normal Wi-Fi interface. This keeps both radios up without sending HaLow replies through the wrong interface.

Outbound connect and send operations have bounded socket timeouts. Transfer Wi-Fi remains connected for an efficient batch of up to 25 successful files or 10 minutes. It then rotates back to HaLow before the next batch; failures close the transfer session immediately so retries begin with clean routing. An independent supervisor restarts the board if HTTP servicing stops for 60 seconds or an active staged upload makes no progress for three minutes. The restart reason and cumulative count persist in NVS and are reported under `failsafe` by `GET /diagnostics/status`.

The supervisor starts before storage, camera, and network initialization complete. It additionally checks the HTTP listener over loopback once per minute, restarts a dead listener after two failures, reboots after four failures, detects stalled startup/main-loop/long HTTP operations, and reboots after HaLow has remained down for ten minutes. OTA and file streaming callbacks update explicit long-operation progress heartbeats.

HaLow liveness probes are bound to the HaLow interface and target the configured gateway rather than the application server. Association gets up to 75 seconds to finish, and failed recovery attempts back off from 30 seconds to a maximum of ten minutes.

SD-backed HTTP handlers wait at most two seconds for storage and otherwise return `503 storage_busy`. Trail-media streaming, staged-upload hashing/blob reads, and camera-to-SD writes hold the storage mutex only for individual chunks rather than for the entire network transfer.

Each manifest item has a persistent failure counter. After three failed download or delete attempts, the durable job records `skipped_after_3:<error>`, advances to the next manifest item, and continues instead of retrying one camera file forever.

Video staging is intentionally strict. MP4 downloads are not promoted from `.part` to
final `.mp4` unless the firmware can determine the complete byte count from the camera
gallery, `Content-Range`, or `Content-Length`, and the staged file passes a basic MP4
atom-structure validation. This prevents short camera-side transfers from being
reported as complete playable videos.
Loss of HaLow does not stop camera-to-SD staging or verified camera deletion.

## Onboard media endpoints

See [ONBOARD_MEDIA_API.md](ONBOARD_MEDIA_API.md).

Routes:

- `GET /onboard/status`
- `POST /onboard/capture`
- `GET /onboard/latest.jpg`
- `GET /onboard/media`
- `GET /onboard/media/{id}`
- `GET /onboard/media/{id}/thumb`
- `DELETE /onboard/media/{id}`
- `POST /onboard/media/delete_all`
- `POST /onboard/config`
- `POST /onboard/reinit`
- `POST /onboard/timelapse/start`
- `POST /onboard/timelapse/stop`

## Scanner and upload endpoints

### `GET /scan/wifi`

Disabled in camera-priority firmware. Returns `410` with `scanner_disabled`.

### `POST /scanner/config?enabled=true|false`

Disabled in camera-priority firmware. Returns scanner config with `feature_enabled:false`.

### `POST /scanner/enable`

Disabled in camera-priority firmware.

### `POST /scanner/disable`

Keeps scheduled Wi-Fi/BLE scans disabled.

### `POST /upload/observations`

Disabled in camera-priority firmware. RF observation scanning/upload has been removed from the active firmware path to protect trail-camera and onboard-camera reliability.

Observation upload target:

```text
http://192.168.1.42/api/observations/upload
```

### `GET /upload/ble/last`

Returns the last BLE upload/debug payload when available.

### `POST /upload/telemetry`

Uploads board telemetry to the configured upstream API.

### `POST /upload/events`

Uploads queued board events to the configured upstream API.

### `POST /upload/all`

Uploads telemetry, events, and queued observation batches.

## SD and OTA endpoints

### `POST /sd/mount`

Attempts to mount/remount the SD card and then loads the durable trail-media job state from it. A successful response means both the card mount and durable-state initialization succeeded. Failure to load or initialize the durable state returns HTTP 503 with `last_message` set to `durable_job_load_failed`.

Returns HTTP 409 with `error: "durable_job_active"` if the SD card is already mounted and a durable media job is active. This prevents a remote remount from invalidating an in-progress transfer.

### `POST /firmware/update`

Multipart OTA upload. The form field name must be `firmware`.

Example:

```bash
curl -sS --connect-timeout 10 --max-time 300 \
  -F "firmware=@/path/to/gardepro_dual_radio_bridge.ino.bin" \
  http://192.168.1.160:18080/firmware/update
```

If `UPSTREAM_API_TOKEN` is configured, include `?token=TOKEN`.

The board returns JSON, waits briefly, and restarts into the new image.
