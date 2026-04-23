# Server API Spec

This document turns the high-level server plan into an implementation-oriented API contract for the future web service.

It is written so the server work can be done on another machine without re-deriving the camera/bridge behavior from scratch.

## Scope

This spec covers:

- browser-facing HTTP routes
- request and response shapes
- which existing Python bridge/job calls each route should use
- expected session behavior
- normalized server semantics

It does not lock in a specific Python web framework.

## Core Rules

1. The browser talks only to the server, never directly to the ESP32 bridge.
2. Non-streaming operations should use short-lived sessions by default.
3. Live view may keep a session open while the stream is active.
4. Destructive actions must be explicitly protected.
5. Server responses should hide camera quirks where practical.

## Shared Response Shape

Recommended top-level envelope:

```json
{
  "ok": true,
  "data": {},
  "error": null
}
```

On failure:

```json
{
  "ok": false,
  "data": null,
  "error": {
    "code": "camera_timeout",
    "message": "Timed out waiting for bringup"
  }
}
```

The implementation may omit the envelope for download/file routes.

## Error Codes

Recommended stable server error codes:

- `bridge_unreachable`
- `camera_wifi_down`
- `camera_timeout`
- `camera_action_failed`
- `camera_busy`
- `not_found`
- `invalid_request`
- `unsafe_action_blocked`
- `internal_error`

## Session Semantics

### Default

For normal actions:

- open session
- perform action
- close session

### Explicit Session Routes

The UI may also explicitly call:

- `POST /api/session/open`
- `POST /api/session/close`

These are useful for a manual operator workflow.

### Live View Exception

Live view is allowed to keep the camera session open while active.

The server should stop stream and close session when:

- the client clicks stop
- the live session times out
- the browser tab disconnects and there are no remaining viewers

## Route Spec

## `GET /api/status`

### Purpose

Return normalized current bridge and camera status.

### Backing call

- `GardeProSessionJobs.fetch_status()`

### Response

```json
{
  "ok": true,
  "data": {
    "camera_reachable": true,
    "session_open": true,
    "live_view_active": false,
    "halow_connected": true,
    "wifi_connected": true,
    "camera_ip": "192.168.8.1",
    "bridge_ip": "192.168.1.157",
    "ble_stage": "wake_ok",
    "standby_requested": false,
    "control_last_message": "bringup_complete",
    "raw": {}
  },
  "error": null
}
```

### Notes

- include enough raw status fields for debugging
- `session_open` should normally map to `wifi_connected`

## `POST /api/session/open`

### Purpose

Open a short-lived working session explicitly.

### Backing call

- `GardeProServerAPI.open_session(...)`

### Request

```json
{
  "timeout_sec": 60,
  "poll_interval_sec": 1
}
```

### Response

```json
{
  "ok": true,
  "data": {
    "session_ready": true,
    "status": {}
  },
  "error": null
}
```

### Notes

- return `session_ready`
- include `before`, `bringup`, `after` only if useful for the UI/debug route

## `POST /api/session/close`

### Purpose

Close the current session and request standby.

### Backing call

- `GardeProServerAPI.close_session(...)`

### Request

```json
{
  "timeout_sec": 45,
  "poll_interval_sec": 2,
  "standby": true
}
```

### Response

```json
{
  "ok": true,
  "data": {
    "session_closed": true,
    "standby_requested": true,
    "status": {}
  },
  "error": null
}
```

### Notes

- `session_closed` should be surfaced as the primary result
- the server should not claim success unless the wrapper confirms teardown

## `GET /api/settings`

### Purpose

Return current settings.

### Backing call

- `GardeProSessionJobs.fetch_setting_values(...)`

### Response

```json
{
  "ok": true,
  "data": {
    "settings": {
      "standby_timeout": 300,
      "wifi": 1,
      "time_zone": "US/Eastern"
    }
  },
  "error": null
}
```

### Notes

- use a short-lived session
- return the `data` object from camera settings, not the whole wrapper unless needed

## `GET /api/settings/schema`

### Purpose

Return a server-defined safe-edit schema for the UI.

### Backing source

- server-owned static mapping

### Response

```json
{
  "ok": true,
  "data": {
    "fields": [
      {
        "key": "standby_timeout",
        "label": "Standby Timeout",
        "type": "integer",
        "editable": true
      }
    ]
  },
  "error": null
}
```

### Notes

- do not generate this dynamically from camera firmware alone
- keep a curated allowlist

## `POST /api/settings`

### Purpose

Apply a safe settings patch.

### Backing call

- `GardeProServerAPI.update_settings(...)` inside a short-lived session

### Request

```json
{
  "patch": {
    "standby_timeout": 300,
    "wifi": 1
  }
}
```

### Response

```json
{
  "ok": true,
  "data": {
    "changed": {},
    "unchanged": {},
    "after": {}
  },
  "error": null
}
```

### Notes

- reject unknown or unsafe keys at the server layer
- return the bridge wrapper result directly or lightly normalized

## `GET /api/media`

### Purpose

Return normalized media gallery items.

### Backing call

- `GardeProSessionJobs.list_media(...)`

### Response

```json
{
  "ok": true,
  "data": {
    "items": [
      {
        "id": 171,
        "type": "photo",
        "timestamp": "2026-04-23 14:52:43",
        "size_bytes": 362440,
        "status": "ready",
        "download_path": "/api/media/171/download",
        "thumbnail_path": "/api/media/171/thumb"
      }
    ]
  },
  "error": null
}
```

### Normalization Rules

- `type == 1` -> `photo`
- `type == 2` -> `video`
- `uid == "00000000"` on video may be surfaced as:
  - `status: "pending"` if the UI wants that distinction
  - otherwise still allow listing it as `video`

## `GET /api/media/latest`

### Purpose

Convenience endpoint for newest item lookup.

### Backing call

- `GardeProServerAPI.latest_media(...)` inside a short-lived session

### Query params

- `type=photo`
- `type=video`

## `GET /api/media/:id/download`

### Purpose

Return photo or video content.

### Backing calls

- `GardeProServerAPI.download_media_result(...)`
- or lower-level download helpers after resolving type/id

### Route requirement

Because the camera uses different file extensions by type, the server must resolve media ID to media type before download.

### Behavior

- server may stream directly to client
- or fetch to temporary server storage then respond

### Notes

- prefer resolving the media item from the gallery first
- set correct content type based on media type

## `GET /api/media/:id/thumb`

### Purpose

Return thumbnail content.

### Backing call

- `GardeProServerAPI.download_media_result(..., thumbnail=True)`

## `DELETE /api/media/:id`

### Purpose

Delete a media item.

### Backing call

- `GardeProServerAPI.delete_media_verified(...)`

### Protection

- admin-only
- explicit confirmation required

### Notes

- server must resolve the media item type first
- canonical delete paths are already validated for photo/video items

## `POST /api/actions/take-picture`

### Purpose

Capture a photo and verify a new image exists.

### Backing call

- `GardeProServerAPI.take_picture_verified(...)`

### Response

```json
{
  "ok": true,
  "data": {
    "captured": true,
    "media": {}
  },
  "error": null
}
```

### Notes

- short-lived session by default

## `POST /api/actions/video/start`

### Purpose

Start recording video.

### Backing call

- `GardeProServerAPI.start_video_recording()`

### Notes

- if not already in a manually opened session, the server should open one first
- usually paired with explicit `video/stop`

## `POST /api/actions/video/stop`

### Purpose

Stop recording and verify the result.

### Backing call

- `GardeProServerAPI.stop_video_recording_verified(...)`

### Notes

- wrapper is now validated against the camera’s early placeholder-video behavior
- a placeholder `uid: "00000000"` item can legitimately represent the new recording before stop completes

## `POST /api/live/start`

### Purpose

Start the live stream path.

### Backing calls

- `GardeProServerAPI.stream_start(...)`
- plus server-side local stream metadata management

### Response

```json
{
  "ok": true,
  "data": {
    "stream_active": true,
    "player_url": "/live/gardepro.sdp"
  },
  "error": null
}
```

### Notes

- exact browser playback mechanism is still an implementation choice
- first pass can reuse the local SDP/tunnel receiver model

## `POST /api/live/stop`

### Purpose

Stop the live stream and release live-view state.

### Backing call

- `GardeProServerAPI.stream_stop(...)`

### Notes

- should stop stream before full session close

## `GET /api/live/status`

### Purpose

Return stream state for the page.

### Backing source

- bridge `/status`
- optional server-owned viewer/session bookkeeping

## `GET /api/admin/clock`

### Purpose

Return direct camera clock readback.

### Backing call

- `GardeProServerAPI.get_info(4)` inside a short-lived session

### Response

```json
{
  "ok": true,
  "data": {
    "clock": "2026-04-23 10:55:33",
    "tz": "US/Eastern"
  },
  "error": null
}
```

## `POST /api/admin/set-clock`

### Purpose

Set the camera clock explicitly.

### Backing call

- `GardeProServerAPI.set_clock(...)`

### Important Rule

- `/cmd/setGmtClock` expects UTC input

### Request

```json
{
  "timestamp_utc": "2026-04-23 14:42:35"
}
```

### Notes

- do not send local Eastern time to this route unless the server converts it to UTC first

## `GET /api/admin/format-status`

### Purpose

Return raw format status/result.

### Backing call

- `GardeProServerAPI.format_sd_result()`

## `POST /api/admin/format-sd`

### Purpose

Format the SD card.

### Backing call

- `GardeProServerAPI.format_sd_start_verified(...)`

### Protection

- admin-only
- explicit confirmation required
- strong UI warning required

### Notes

- live validation confirmed the real post-condition by checking that the gallery became empty afterward

## Route-to-Code Mapping

Recommended mapping:

- low-level transport: `gardepro_server_api.py`
- route-safe short-session jobs: extend `gardepro_server_jobs.py`
- browser-facing routes: new web server file

Recommended new job additions:

- `open_session_job(...)`
- `close_session_job(...)`
- `get_clock_job(...)`
- `set_clock_job(...)`
- `take_picture_job(...)`
- `start_video_job(...)`
- `stop_video_job(...)`
- `download_media_job(...)`
- `delete_media_job(...)`
- `format_sd_job(...)`

## Recommended Next Code Files

- `gardepro_web_server.py`
- `gardepro_server_models.py`
  Optional response normalization helpers
- `tests/test_gardepro_web_server.py`
- `tests/test_gardepro_server_models.py`

## Minimal First Milestone

The first server milestone should implement:

- `GET /api/status`
- `POST /api/session/open`
- `POST /api/session/close`
- `GET /api/settings`
- `POST /api/settings`
- `GET /api/media`
- `GET /api/media/:id/download`
- `POST /api/actions/take-picture`
- `POST /api/actions/video/start`
- `POST /api/actions/video/stop`
- `GET /api/admin/clock`

That is enough to support the first useful browser page.
