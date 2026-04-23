# Server-Side Plan

This document defines the next implementation phase on top of the validated bridge and camera-control layer.

## Goal

Build a local web service that hosts a page where a user can:

- connect to the camera on demand
- view a live feed
- verify camera status
- view current settings
- update safe settings
- browse and download photos/videos

The intended runtime model remains short-lived sessions, not permanent camera connectivity.

## Product Shape

The service should expose:

- a browser UI for human operators
- a small server API used by that UI
- a server-side session/job layer that wraps the existing bridge API

The browser should never talk directly to the ESP32 bridge. It should talk only to the local server.

## Architecture

Recommended layers:

1. `gardepro_server_api.py`
   Existing low-level client for the ESP32 bridge.

2. `gardepro_server_jobs.py`
   Existing short-session job layer.

3. New server app
   Responsibility:
   - HTTP routes for the browser
   - session lifecycle policy
   - streaming orchestration
   - download handling
   - response normalization

4. New frontend page
   Responsibility:
   - show status
   - trigger connect/disconnect
   - show live preview
   - render settings
   - render media gallery
   - request downloads

## Session Model

Default rule:

- every non-streaming action should run inside a bounded short-lived session
- the server opens the session
- performs one task
- closes the session

Exceptions:

- live view can keep a session open while the page is actively watching the stream
- server should stop live view and close the session when the page disconnects, times out, or explicitly stops streaming

## Server API Surface

Recommended first-pass routes:

### Health / Status

- `GET /api/status`
  Returns current bridge status from `/status`.

- `POST /api/session/open`
  Explicit wake/connect action for the UI.

- `POST /api/session/close`
  Explicit standby/teardown action for the UI.

### Live View

- `POST /api/live/start`
  Starts streaming on the bridge and returns the local player target or page-specific stream metadata.

- `POST /api/live/stop`
  Stops live view and tears down stream state.

- `GET /api/live/status`
  Returns whether live view is active and whether the tunnel is up.

### Settings

- `GET /api/settings`
  Returns the full current settings object.

- `POST /api/settings`
  Applies a safe patch through `setting-update-json`.

- `GET /api/settings/schema`
  Returns a server-defined description of editable fields, labels, and allowed values.

### Media

- `GET /api/media`
  Returns normalized gallery items.

- `GET /api/media/latest`
  Optional convenience endpoint for newest item lookup.

- `GET /api/media/:id/download`
  Streams or serves a photo/video file.

- `GET /api/media/:id/thumb`
  Returns thumbnail content.

- `DELETE /api/media/:id`
  Optional admin-only delete route.

### Actions

- `POST /api/actions/take-picture`
  Runs verified photo capture.

- `POST /api/actions/video/start`
  Starts recording.

- `POST /api/actions/video/stop`
  Stops recording with the verified wrapper.

### Admin

- `POST /api/admin/set-clock`
  Optional explicit clock sync action.

- `POST /api/admin/format-sd`
  Explicit destructive admin-only route.

## Response Normalization

The browser should not need to understand camera quirks.

The server should normalize:

- transport failures into stable server errors
- `uid: "00000000"` placeholder videos into a clear `recording_pending` or equivalent server concept if needed
- raw bridge booleans into user-facing state such as:
  - `camera_reachable`
  - `session_open`
  - `live_view_active`
  - `standby_requested`

Recommended normalized media shape:

```json
{
  "id": 170,
  "type": "video",
  "timestamp": "2026-04-23 14:44:21",
  "size_bytes": 137076353,
  "status": "ready",
  "download_path": "/api/media/170/download",
  "thumbnail_path": "/api/media/170/thumb"
}
```

For video status:

- `ready` for normal non-placeholder entries
- `pending` only if the server deliberately wants to expose placeholder `uid: "00000000"` semantics

## Frontend Page

The initial page should include:

### Connection Panel

- current bridge/camera state
- connect button
- disconnect button
- last error / last control message

### Live View Panel

- start live view
- stop live view
- player element for the local stream
- stream state text

### Status Panel

- WiFi connected
- HaLow connected
- camera IP
- BLE stage
- tunnel connected
- standby requested

### Settings Panel

- read current settings
- render safe editable fields first:
  - `standby_timeout`
  - `wifi`
  - `date_format`
  - `time_format`
  - `temperature_format`
  - `photo_or_video`
  - `photo_quality`
  - `video_quality`
  - `video_length`
- hide or protect risky/poorly understood settings until needed

### Media Panel

- gallery table/grid
- type, timestamp, size
- thumbnail for photos and videos when useful
- download button
- optional delete button only behind stronger confirmation

## Live Feed Handling

The live feed path needs an explicit server decision.

Recommended first implementation:

- server calls existing stream start/stop controls
- browser receives enough metadata to connect to a locally served player target
- avoid exposing raw bridge internals directly to the browser

Open implementation choice:

- simplest path: server returns the local SDP/player endpoint already produced by the tunnel receiver
- cleaner product path later: server proxies a browser-friendly stream format

The first pass should favor the simplest working path.

## Safety / Access Rules

Safe by default:

- status reads
- settings reads
- media listing
- downloads
- photo capture
- video start/stop

Protected:

- delete media
- format SD
- reboot camera
- factory reset

These should require:

- explicit confirmation in the UI
- server-side allowlist handling
- obvious audit logging

## Suggested Implementation Order

1. Create the new server app skeleton.
2. Add `/api/status`, `/api/session/open`, and `/api/session/close`.
3. Add `/api/settings` read path and the settings page.
4. Add `/api/media` and download routes.
5. Add `take-picture`.
6. Add live view start/stop and the live page panel.
7. Add video start/stop.
8. Add protected destructive admin routes.

## File Plan

Recommended additions:

- `SERVER_SIDE_PLAN.md`
  This plan.

- `gardepro_web_server.py`
  First local HTTP server app.

- `templates/` or static frontend files if using server-rendered HTML.

- `static/`
  CSS and browser JS if using a minimal local web app.

- `tests/test_gardepro_web_server.py`
  Server route and policy tests.

## Testing Plan

Server-side tests should cover:

- short-session open/work/close behavior
- action route success and failure mapping
- settings read/write route behavior
- media list normalization
- download route selection
- destructive route protection
- live-view lifecycle cleanup

Manual live tests should cover:

- open page, connect, view status
- start live view, stop live view
- take picture, confirm gallery update
- start/stop video, confirm gallery update
- download photo
- download video
- edit a safe setting and re-read it
- close session and confirm standby

## Open Decisions

Still to choose before implementation:

- server framework:
  - minimal built-in HTTP
  - Flask
  - FastAPI
- frontend style:
  - server-rendered HTML
  - minimal JS app
- live video delivery method to browsers
- auth model, if any, beyond local-network trust

## Recommendation

Start with a small Python web server using the existing job layer and a simple browser page.

Do not over-design the first pass.

The first server milestone should prove:

- the user can open the page
- connect to the camera
- see status
- view/update safe settings
- browse/download media
- start/stop live view

Once that is stable, expand admin flows and polish.
