# ESP32 Onboard Media API

Base URL: `http://<board-ip>:18080`

## Endpoints

### `GET /onboard/status`

Returns camera configuration and storage state. Storage fields:

```json
{
  "storage_ready": true,
  "storage_type": "littlefs",
  "storage_total_bytes": 655360,
  "storage_used_bytes": 155648,
  "storage_free_bytes": 499712,
  "stored_photo_count": 1,
  "latest_media_id": "00000004",
  "latest_bytes": 138736
}
```

`latest_media_id` is `null` when no image exists.

### `POST /onboard/capture`

Captures and persists a JPEG.

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
- The current board partition provides 640 KiB of media storage. Handle HTTP `507` / `storage_full`.
- File endpoints return binary JPEG data; other endpoints return JSON.
