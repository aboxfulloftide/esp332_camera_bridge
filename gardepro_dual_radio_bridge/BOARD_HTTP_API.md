# Trail ESP32 Board HTTP API

Base URL: `http://<board-ip>:18080`

Current field unit address: `http://192.168.1.160:18080`

The board HTTP server is intentionally small. Prefer sequential requests with conservative timeouts on the HaLow link, especially during BLE wake, camera Wi-Fi join, media download, and live view.

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

Current firmware defaults both automatic camera HTTP keepalive and automatic camera recovery to disabled so an unreliable trail-camera connection cannot run blocking recovery work from the board HTTP service loop. Explicit `POST /control/bringup` and `POST /control/stream_start` still run through the control worker.

### `GET /system/status`

System runtime: uptime, hostname, boot counters, PSRAM, and chip temperature.

### `POST /system/http_restart`

Restarts the board HTTP listener without rebooting the ESP32. This is a defensive recovery endpoint for the observed failure mode where serial and HaLow remain alive but the synchronous WebServer/TCP listener stops serving new HTTP clients.

The firmware also periodically re-begins the HTTP listener while idle. It skips the periodic restart during OTA, pending restart, active control actions, and active stream sessions.

### `GET /halow/status`

HaLow uplink state: connected/status, SSID/BSSID/MAC/IP/gateway, RSSI, and event counters.

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
- `recoveries`
- `idle_recoveries`
- `http_keepalive_failures`
- `media_primary_packets`
- `media_primary_bytes`
- `media_secondary_packets`
- `media_secondary_bytes`

During active live view the stream can still be functional even if occasional ESP32 control/status HTTP polls reset or time out. Treat packet counters as the functional signal.

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

### `GET /battery/status`

Battery and charger pins:

- `adc_pin`
- `adc_ctrl_pin`
- `adc_mv`
- `battery_est_v`
- `charging_gpio15`
- `done_gpio16`

### `GET /onboard/status`

Onboard ESP camera configuration, schedule, storage, and timelapse state. See [ONBOARD_MEDIA_API.md](ONBOARD_MEDIA_API.md) for media-specific details.

### `GET /upload/status`

Board telemetry/event upload and RF observation retry state. Firmware currently targets board telemetry/events under `http://192.168.1.42/trail_cam/api/...` and RF observations at `http://192.168.1.42/api/observations/upload`.

### `GET /session/status`

Trail-camera session lease state. Use this when coordinating short-lived camera operations from the server.

### `GET /sd/status`

SD card mount/storage details: ready state, mount counters, pins, card type/size, and storage totals.

### `GET /scanner/config`

Scheduled Wi-Fi/BLE scan configuration. In camera-priority firmware this feature is disabled so it cannot compete with trail-camera BLE, trail-camera Wi-Fi, onboard capture, or board HTTP responsiveness.

### `GET /firmware/status`

OTA status: firmware name/version, update endpoint, in-progress flag, last result, bytes written, errors, and free sketch space.

## Control endpoints

### `POST /control/bringup`

Queues camera bringup: BLE wake, camera hotspot wait, trail-camera Wi-Fi join, and camera HTTP verification.

Poll `/control/status`, `/wifi/status`, `/ble/status`, and `/timing/status` for completion details.

### `POST /control/stream_start`

Queues live-view start. This can queue behind active bringup.

Live view is split into two independent states:

- camera-side RTSP/RTP session on the ESP32
- optional HaLow tunnel forwarding to the upstream receiver

If the trail-camera RTSP session starts but HaLow or the tunnel is unavailable, the job still succeeds as a local camera stream. `/stream/status` then reports camera packet counters while `tunnel_connected` remains `false`.

### `POST /control/stream_stop`

Stops active live view and closes the tunnel path.

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

Returns the current in-memory camera worker queue/control state and supported actions.

Current supported actions:

- `bringup`
- `live_view_start`
- `live_view_stop`
- `trigger_photo`

### `POST /jobs`

Queues camera work and returns immediately. This is the first set-and-forget API layer. The current implementation queues into the in-memory control worker; SD-backed durable persistence is the next planned increment.

Examples:

```bash
curl -sS -X POST "http://192.168.1.160:18080/jobs?action=trigger_photo"
curl -sS -X POST "http://192.168.1.160:18080/jobs?action=live_view_start"
curl -sS -X POST "http://192.168.1.160:18080/jobs?action=live_view_stop"
```

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

Attempts to mount/remount the SD card.

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
