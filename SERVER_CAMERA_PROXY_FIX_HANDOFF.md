# Server Fix Handoff: ESP32 Camera Proxy Route

Date: 2026-06-21

## Problem

The server is generating an incompatible ESP32 bridge camera proxy URL for read-only camera calls.

The failing form is:

```text
GET /camera/request?method=GET&path=/cmd/getSetting
```

During live testing, that form failed against the bridge.

The working form is:

```text
GET /camera/request?path=/cmd/getSetting
```

POST-style camera calls still need method metadata and JSON metadata:

```text
POST /camera/request?method=POST&path=/cmd/setSetting&content_type=application/json
```

with the raw JSON request body forwarded unchanged.

## Required Server Change

Find the server-side camera proxy/helper code that builds requests to the ESP32 bridge route:

```text
http://<board-ip>:18080/camera/request
```

Change the query construction rule to:

1. If forwarding a camera GET/read request:

```text
GET /camera/request?path=<camera-path>
```

Do not include:

```text
method=GET
```

2. If forwarding a camera POST/write request with a body:

```text
POST /camera/request?method=POST&path=<camera-path>&content_type=application/json
```

Include the raw JSON body.

## Example Pseudocode

```php
if ($body === null) {
    // Camera read
    $bridgeMethod = 'GET';
    $query = [
        'path' => $cameraPath,
    ];
} else {
    // Camera write
    $bridgeMethod = 'POST';
    $query = [
        'path' => $cameraPath,
        'method' => 'POST',
        'content_type' => 'application/json',
    ];
}
```

Equivalent behavior in any language is fine. The important part is that GET camera calls omit `method=GET`.

## Server Routes Likely Affected

Revalidate any server API route that reads or writes camera data through the ESP32 bridge, especially:

```text
GET  /trail_cam/api/settings
GET  /trail_cam/api/media/latest
GET  /trail_cam/api/admin/clock
POST /trail_cam/api/settings
POST /trail_cam/api/admin/clock
```

The exact route names may differ on the deployed server, but any route using the bridge endpoint `/camera/request` should be checked.

## Firmware Contract

The ESP32 bridge accepts:

```text
GET /camera/request?path=/cmd/...
POST /camera/request?method=POST&path=/cmd/...&content_type=application/json
```

The bridge firmware defaults a missing `method` argument to `GET`, so `method=GET` is unnecessary for reads.

## Validation Commands

Run these from the server after the ESP32 bridge is reachable.

Use real curl on Windows PowerShell as `curl.exe`, not `curl`, because PowerShell aliases `curl` to `Invoke-WebRequest`.

```bash
curl -i --max-time 10 http://192.168.1.160:18080/status
curl -i --max-time 10 "http://192.168.1.160:18080/camera/request?path=/cmd/getSetting"
```

Expected result:

- `/status` returns HTTP 200 with bridge status JSON.
- `/camera/request?path=/cmd/getSetting` returns HTTP 200 with camera settings JSON.

Then test through the server API:

```bash
curl -i --max-time 10 http://127.0.0.1/trail_cam/api/status
curl -i --max-time 10 http://127.0.0.1/trail_cam/api/settings
curl -i --max-time 10 http://127.0.0.1/trail_cam/api/media/latest
curl -i --max-time 10 http://127.0.0.1/trail_cam/api/admin/clock
```

Expected result:

- The server should not generate `GET /camera/request?method=GET&path=...`.
- The API should no longer return a camera-proxy error caused by the bad GET query shape.

## Known Board State Caveat

If the board is unhealthy or offline, the server may return an error such as:

```json
{"error":"bridge_unreachable"}
```

That is a connectivity/board availability issue, not this route-construction bug. Confirm the board first with:

```bash
curl -i --max-time 10 http://192.168.1.160:18080/status
```

