# Unified ESP32 Camera/Scanner API Requirements

This document defines the server API the unified HT-HC33 firmware should use once
the HaLow network and production API server are available.

## Runtime Model

- The ESP32 scans WiFi/BLE locally.
- At each upload window, it connects upstream over HaLow and uploads:
  - WiFi observations
  - BLE observations
  - scanner health
  - battery status
  - local temperature, once the probe is attached
  - any queued onboard-camera captures
  - any queued trail-camera media or metadata
- Trail-camera bridge work has priority over local scanning.
  - Pause WiFi/BLE scanning while waking, connecting to, streaming from, or downloading from the trail camera.
  - Resume scanning after `session-close`.
- HaLow is the only intended upstream transport for production uploads.

## Existing Compatible Endpoint

### `POST /api/observations/upload`

This exists in `air_scan` and should remain the primary upload route for WiFi and
BLE observations.

Current ESP32 WiFi payload shape:

```json
{
  "scanner_host": "ht-hc33-yard-1",
  "health": {
    "mac": "ac:a7:04:e3:74:a8",
    "free_heap": 251008,
    "min_free_heap": 210000,
    "uptime_ms": 123456,
    "temperature_c": 43.2
  },
  "observations": [
    {
      "mac": "aa:bb:cc:dd:ee:ff",
      "device_type": "AP",
      "interface": "esp32-wifi",
      "signal_dbm": -72,
      "channel": 6,
      "freq_mhz": 2437,
      "ssid": "example",
      "ht": true,
      "vht": false,
      "he": false,
      "probe_count": 4,
      "recorded_at": "2026-06-09T23:05:10"
    }
  ]
}
```

Required server update for BLE:

- Accept `device_type: "BLE"`.
- Preserve these optional BLE fields on each observation:
  - `manufacturer_data`
  - `adv_services`
  - `adv_service_data`
  - `tx_power`
  - `tracker_type`
- Store BLE device names using `ssid` as the current Pi scanner does, or add a
  neutral alias/name table if preferred.

BLE observation payload:

```json
{
  "mac": "11:22:33:44:55:66",
  "device_type": "BLE",
  "interface": "esp32-ble",
  "signal_dbm": -81,
  "channel": 37,
  "freq_mhz": 2402,
  "ssid": "optional-local-name",
  "ht": false,
  "vht": false,
  "he": false,
  "probe_count": 1,
  "manufacturer_data": "004C:0215...",
  "adv_services": "0000feaa-0000-1000-8000-00805f9b34fb",
  "adv_service_data": "0000feaa-0000-1000-8000-00805f9b34fb:...",
  "tx_power": -8,
  "tracker_type": "AirTag",
  "recorded_at": "2026-06-09T23:05:10"
}
```

Recommended response:

```json
{
  "inserted": 42,
  "devices": 18
}
```

## New Required Endpoints

### `POST /api/board/telemetry`

Uploads low-rate board telemetry that is not an RF observation.

Request:

```json
{
  "scanner_host": "ht-hc33-yard-1",
  "recorded_at": "2026-06-09T23:05:10",
  "firmware": {
    "name": "gardepro_unified",
    "version": "0.1.0",
    "build": "local"
  },
  "board": {
    "mac": "ac:a7:04:e3:74:a8",
    "uptime_ms": 123456,
    "free_heap": 251008,
    "min_free_heap": 210000,
    "psram_free": 6000000,
    "chip_temperature_c": 43.2
  },
  "battery": {
    "adc_mv": 2023,
    "battery_est_v": 4.046,
    "charging_gpio15": 1,
    "done_gpio16": 1
  },
  "temperature": {
    "probe_attached": false,
    "sensor": null,
    "temperature_c": null
  },
  "radio": {
    "halow_connected": true,
    "halow_ip": "192.168.1.157",
    "halow_rssi": -55,
    "trail_wifi_connected": false
  }
}
```

Response:

```json
{
  "ok": true,
  "telemetry_id": 123
}
```

Storage requirements:

- Keep telemetry as time-series records keyed by `scanner_host` and `recorded_at`.
- Battery and temperature should not be forced into the RF `observations` table.

### `POST /api/board/media`

Uploads media metadata first. The server returns an upload URL or accepts the file
directly if it supports multipart.

Request:

```json
{
  "scanner_host": "ht-hc33-yard-1",
  "source": "onboard",
  "media_type": "image/jpeg",
  "filename": "onboard_20260609T230510Z.jpg",
  "recorded_at": "2026-06-09T23:05:10",
  "bytes": 22234,
  "sha256": "hex-encoded-sha256",
  "metadata": {
    "width": 1600,
    "height": 1200,
    "battery_est_v": 4.046
  }
}
```

`source` values:

- `onboard`
- `trail_camera`

Response option A, direct multipart:

```json
{
  "ok": true,
  "media_id": 456
}
```

Response option B, two-step upload:

```json
{
  "ok": true,
  "media_id": 456,
  "upload_url": "/api/board/media/456/blob"
}
```

### `PUT /api/board/media/{media_id}/blob`

Uploads raw bytes when the server chooses the two-step flow.

Headers:

- `Content-Type: image/jpeg` or `video/mp4`
- `Content-Length: <bytes>`
- `X-SHA256: <hex>`

Response:

```json
{
  "ok": true,
  "media_id": 456,
  "bytes": 22234
}
```

Trail-camera media requirements:

- Store original trail-camera `media_id`, `media_type`, and camera path when known.
- Support images and videos:
  - `image/jpeg`
  - `video/mp4`
- Store whether the item came from a thumbnail, full photo, full video, or live
  capture frame.

### `POST /api/board/events`

Uploads state transitions and operational events that are useful for debugging.

Request:

```json
{
  "scanner_host": "ht-hc33-yard-1",
  "recorded_at": "2026-06-09T23:05:10",
  "events": [
    {
      "type": "scanner_paused",
      "reason": "trail_bridge_session_open",
      "details": {}
    },
    {
      "type": "trail_session_result",
      "ok": true,
      "details": {
        "ble_stage": "wake_confirmed",
        "wifi_connected": true
      }
    }
  ]
}
```

Response:

```json
{
  "ok": true,
  "inserted": 2
}
```

## Upload Window Behavior

Recommended sequence:

1. Pause promiscuous WiFi scanning.
2. Pause BLE scanning unless the BLE stack can safely remain active with HaLow.
3. Connect HaLow.
4. POST `/api/observations/upload` for WiFi/BLE observations.
5. POST `/api/board/telemetry`.
6. POST media metadata and upload pending media blobs.
7. POST `/api/board/events` if any queued events exist.
8. Disconnect/idle HaLow if desired.
9. Resume WiFi/BLE scanning unless a trail-camera session is active.

## Firmware Queue Requirements

The firmware should maintain separate queues:

- RF observations queue
- telemetry latest-record queue
- media queue
- event queue

The server should make every upload idempotent enough for retry:

- RF observations can be duplicated unless the server adds a uniqueness key.
- Telemetry can accept duplicates or use `(scanner_host, recorded_at)` uniqueness.
- Media should use `sha256` plus `(scanner_host, source, recorded_at)` to avoid duplicate files.
- Events can use a client-generated `event_id` later if duplicates become noisy.

## Scanner Compatibility Notes

- WiFi observations should keep the existing 10-second UTC slot behavior.
- BLE observations should use the same slot boundary.
- The ESP32-S3 onboard WiFi radio cannot scan and stay connected to the
  trail-camera hotspot at the same time.
- HaLow is independent of the 2.4 GHz WiFi radio and is the correct upload path.
- BLE scanning may need to pause during BLE trail-camera wake if the BLE client
  and scanner conflict in practice.
