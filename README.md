# ESP32 Camera Bridge

This repository contains the current GardePro ESP32 bridge work:

- `gardepro_dual_radio_bridge/`
  - ESP32 dual-radio bridge sketch
- `gardepro_server_api.py`
  - Python client for the board HTTP bridge
- `gardepro_server_jobs.py`
  - small server-facing short-session job layer
- `gardepro_server_control.py`
  - CLI wrapper for live control and validation
- `gardepro_tunnel_server.py`
  - receiver for the framed tunnel stream
- `gardepro_web_server.py`
  - Flask browser/API server for the operator UI
- `templates/`
  - Flask HTML templates for the server UI
- `static/`
  - CSS and browser JS for the server UI
- `BRIDGE_USAGE.md`
  - current operator notes and validation workflow
- `gardepro_dual_radio_bridge/BOARD_HTTP_API.md`
  - current board HTTP API reference for status, onboard media, scanner/upload, OTA, and trail-camera proxy routes
- `CAMERA_RETURN_CHECKLIST.md`
  - resume checklist for when the camera has power again
- `SERVER_SIDE_PLAN.md`
  - next-phase plan for the local web service and UI
- `SERVER_API_SPEC.md`
  - route-by-route server API contract and bridge/job mapping
- `UNIFIED_FIRMWARE_API_REQUIREMENTS.md`
  - server API requirements for the unified scanner/camera/battery firmware uploads
- `SERVER_DEPLOYMENT_HANDOFF.md`
  - runtime/deployment notes for doing the server work on another machine

## Local Config

The published tree does not include local network credentials.

Create these local files from the provided examples:

- `gardepro_dual_radio_bridge/local_config.h`
- `stream_test/local_config.h`

## Notes

- The trail-camera hotspot password remains in the repo because it is the vendor default for the device under test.
- Large local reverse-engineering artifacts and machine-specific files are ignored by git.
- The current server-side direction is short-lived on-demand sessions, not permanent camera connectivity.
- The current Flask web server is intentionally small and serves the browser UI plus the API used by that UI; it does not talk to the camera directly from the browser.
