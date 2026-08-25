# ESP32 Onboard Media API

Base URL: `http://<board-ip>:18080`

Current field unit address: `http://192.168.1.160:18080`

## Endpoints

### `GET /onboard/status`

Returns camera configuration, schedule/power state, and storage state.

The scheduled onboard camera is configured for one sunny-day capture at noon by default:

- interval: `86400000` ms / 24 hours
- active window: `12:00` through `12:30`
- outside the active window, the firmware deinitializes the ESP32 camera and leaves persisted media available from storage
- at the next active window, the scheduler initializes the camera again and captures once
- if network time is not valid yet, the scheduler uses an uptime-based 24-hour fallback window instead of capturing unrestricted 24 hours/day
- default sunny-day image settings: `ae_level=-2`, `brightness=-1`, `contrast=1`, `sharpness=3`, `saturation=0`, `jpeg_quality=5`

Power/schedule fields:

```json
{
  "ready": false,
  "power_state": "scheduled_window_inactive",
  "powered_down_by_schedule": true,
  "power_up_count": 1,
  "power_down_count": 1,
  "enabled": true,
  "interval_ms": 86400000,
  "window_start": "12:00",
  "window_end": "12:30",
  "clock_valid": true,
  "schedule_mode": "clock_window",
  "local_minute": 1265,
  "actual_local_minute": 1265,
  "window_active": false
}
```

Storage fields:

```json
{
  "storage_ready": true,
  "storage_type": "sd",
  "storage_total_bytes": 63856705536,
  "storage_used_bytes": 6291456,
  "storage_free_bytes": 63850414080,
  "stored_photo_count": 1,
  "latest_media_id": "00000004",
  "latest_bytes": 138736
}
```

`latest_media_id` is `null` when no image exists.

### `POST /onboard/capture`

Captures and persists a JPEG.

If frame capture fails, firmware attempts one onboard-camera deinit/reinit and
then retries the frame capture once. `/onboard/status` exposes
`camera_reinit_count`, `capture_recovery_count`, `last_capture_error`, and
`last_recovery_reason` for diagnosis.

```json
{
  "ok": true,
  "media": {
    "id": "00000004",
    "filename": "00000004.jpg",
    "recorded_at": null,
    "bytes": 138736,
    "content_type": "image/jpeg",
    "width": 1600,
    "height": 1200
  },
  "latest_updated": true
}
```

`recorded_at` is an ISO 8601 UTC string when the board clock is valid; otherwise it is `null`.

### `GET /onboard/latest.jpg`

Returns the newest persisted JPEG, including after reboot.

### `GET /onboard/media`

Query parameters:

- `offset`: default `0`
- `limit`: default `50`, maximum `100`
- `sort`: `newest` (default) or `oldest`
- `from`, `to`: optional Unix epoch-second filters

```json
{
  "count": 1,
  "offset": 0,
  "limit": 50,
  "sort": "newest",
  "media": [
    {
      "id": "00000004",
      "filename": "00000004.jpg",
      "recorded_at": null,
      "bytes": 138736,
      "content_type": "image/jpeg",
      "width": 1600,
      "height": 1200,
      "capture_kind": "manual",
      "path": "/onboard/media/00000004",
      "thumb_path": null
    }
  ]
}
```

`count` is the total matching count before pagination.

### `GET /onboard/media/{id}`

Returns the JPEG with correct `Content-Type` and `Content-Length` headers.

### `GET /onboard/media/{id}/thumb`

Currently returns HTTP `404` with `{"error":"thumbnail_not_available"}`. Thumbnail paths are therefore `null`.

### `DELETE /onboard/media/{id}`

```json
{"ok":true,"id":"00000004"}
```

### `POST /onboard/media/delete_all`

```json
{"ok":true,"deleted":3,"failed":0}
```

### `POST /onboard/reinit`

Forces an ESP32 onboard-camera driver deinit/reinit without rebooting the whole
board. Use this when `/onboard/status` says the camera is ready but capture
returns `capture_failed` or settings return `sensor_unavailable`.

## Errors

JSON error responses use stable codes:

```json
{"error":"media_not_found"}
```

Possible codes include:

- `onboard_camera_not_ready`
- `capture_failed`
- `storage_unavailable`
- `storage_full`
- `media_not_found`
- `delete_failed`
- `invalid_request`
- `thumbnail_not_available`

## Integration Notes

- Treat media IDs as opaque strings. IDs remain stable and are never renumbered or reused after deletion.
- Resolve relative `path` values against the board base URL.
- SD is the primary persistent store when mounted. LittleFS is retained only as fallback.
- Handle HTTP `507` / `storage_full`.
- File endpoints return binary JPEG data; other endpoints return JSON.
