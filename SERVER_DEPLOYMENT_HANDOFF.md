# Server Deployment Handoff

This document is for the machine that hosts the Flask browser/API server and the ESP32 bridge runtime.

It explains what that machine needs, what still runs on the bridge machine, and what assumptions the server code can safely make.

## High-Level Model

There are two logical sides:

1. Bridge side
   Runs the ESP32 bridge firmware and the local tunnel receiver.

2. Server side
   Runs the web service and browser UI.

If both happen to live on the same machine, that is fine. The current repo includes a Flask server (`gardepro_web_server.py`) that can be run directly on the bridge host or a separate server host.

If they are split across different machines, the server side must still be able to reach:

- the ESP32 bridge HTTP server on port `18080`
- the tunnel receiver outputs or equivalent live-stream metadata source
- the current board registration file or a replacement discovery mechanism

## What Is Already Validated

Validated runtime components:

- ESP32 bridge firmware in `gardepro_dual_radio_bridge/`
- local tunnel receiver in `gardepro_tunnel_server.py`
- validated bridge client in `gardepro_server_api.py`
- validated short-session helpers in `gardepro_server_jobs.py`
- Flask browser/API server in `gardepro_web_server.py`

Validated camera behaviors:

- wake / bringup
- short session close / standby
- settings read/write
- clock readback/set
- photo capture
- video start/stop
- media list/download/delete
- SD format

## Machine Roles

## Option A: Same Machine

Simplest first deployment.

Run on one box:

- `gardepro_tunnel_server.py`
- `gardepro_web_server.py`

Advantages:

- simplest networking
- `/tmp/gardepro_board_registration.json` already local
- local SDP/tunnel path easiest to reuse

## Option B: Separate Server Machine

If server work happens on a different machine, one of these must be true:

### B1. Server machine talks directly to bridge HTTP

The server machine can reach the ESP32 HaLow IP and port `18080`.

Requirements:

- the bridge IP must be routable from that machine
- the server must know the current bridge IP

### B2. Registration file is replicated

If using `gardepro_server_api.py` unchanged, the server machine needs the equivalent of:

- `/tmp/gardepro_board_registration.json`

That file currently comes from:

- `gardepro_tunnel_server.py`

If the server is on another machine, you need one of:

- shared filesystem
- copied registration file
- server-side config override for `--host`
- a replacement registration service

### B3. Live stream path is bridged too

If the server machine will host the browser UI and live playback, it also needs a plan for:

- SDP file access
- tunnel receiver reachability
- local UDP/player target behavior

## Current Runtime Files the Server Depends On

Today, `gardepro_server_api.py` defaults to:

- registration file:
  - `/tmp/gardepro_board_registration.json`

The tunnel receiver currently writes:

- registration:
  - `/tmp/gardepro_board_registration.json`
- SDP:
  - `/tmp/gardepro_live.sdp`

The Flask web server also uses temporary local files for browser-facing media downloads and live-view previews.

If the server runs on a different machine, decide whether to:

- keep those exact file paths available there
- or change server code to source this state elsewhere

## Recommended Handoff Strategy

For another machine, the cleanest first step is:

1. copy this repo to the server machine
2. run the Flask web server on that machine
3. configure the server to use explicit bridge host/IP rather than relying only on the local registration file
4. keep the tunnel receiver on the bridge-side machine unless the live-view path is also being moved

That avoids requiring the full bridge runtime to move immediately.

## Environment Inputs To Make Configurable

The web server should support configuration for:

- `GARDEPRO_BRIDGE_HOST`
- `GARDEPRO_BRIDGE_PORT`
- `GARDEPRO_REGISTRATION_PATH`
- `GARDEPRO_HTTP_TIMEOUT`
- `GARDEPRO_CONTROL_TIMEOUT`
- `GARDEPRO_PICTURE_TIMEOUT`
- `GARDEPRO_SDP_PATH`
- `GARDEPRO_DOWNLOAD_DIR`

Recommended behavior:

- if `GARDEPRO_BRIDGE_HOST` is set, prefer it
- otherwise fall back to the registration file

That makes remote deployment much easier.

## Ports and Network Expectations

Current validated ports:

- ESP32 bridge HTTP:
  - `18080/tcp`
- GPRT tunnel receiver:
  - `6000/tcp`
- local UDP fanout:
  - `5004/udp`
  - `5005/udp`

If the server and browser are separate from the bridge machine, confirm firewall/routing for:

- server -> `bridge:18080`
- browser -> server web port
- any chosen live-view delivery path

## Software Prerequisites For The Server Machine

Minimum:

- Python 3
- Flask
- this repo checked out
- access to the bridge HTTP endpoint

If using the current Python modules directly:

- standard library is sufficient for the existing client layer

Potential future additions:

- chosen web framework
- frontend static asset pipeline, if any

## Files To Copy To The Server Machine

Required:

- `gardepro_server_api.py`
- `gardepro_server_jobs.py`
- `gardepro_web_server.py`
- `templates/`
- `static/`
- this repo documentation

Helpful:

- `BRIDGE_USAGE.md`
- `GARDEPRO_CAMERA_HTTP_CANDIDATES.md`
- `SERVER_SIDE_PLAN.md`
- `SERVER_API_SPEC.md`

## Live Feed Handoff Notes

The hardest cross-machine question is live playback.

Before implementation, decide whether the server machine will:

1. directly serve the existing local SDP/player model
2. proxy/relay a browser-friendlier stream
3. remote-control a player on the bridge machine instead of serving media directly

Recommended first pass:

- keep the simplest route
- do not redesign the streaming layer until the basic web server is working

## Operational Assumptions The Server Can Rely On

The server can safely assume:

- `session-close` now really waits for WiFi to fall away
- `set-clock` must use UTC input
- `info --index 4` gives direct clock readback
- `video-stop` now handles early placeholder video items
- canonical media delete paths are validated
- format success can be verified via an empty gallery

## Things The Server Should Not Re-Debug

These are already validated enough and should be treated as settled until proven otherwise:

- BLE wake viability
- short-session standby behavior
- canonical media paths
- clock path semantics
- basic action wrappers

The server should consume these behaviors, not rediscover them.

## Suggested Bring-Up Checklist On The Other Machine

1. Confirm the server machine can reach the bridge host on `18080/tcp`.
2. Confirm the chosen bridge host resolution method:
   - explicit env host
   - or replicated registration file
3. Run a basic status probe using `gardepro_server_api.py`.
4. Verify `session-open` and `session-close`.
5. Verify settings read.
6. Verify media list and download/thumb routes.
7. Start `gardepro_web_server.py` and confirm the browser UI loads.

## Recommended Next Artifacts

After this handoff, the next useful files are:

- `gardepro_web_server.py`
- `tests/test_gardepro_web_server.py`
- `static/app.js`
- `static/app.css`
- optional template file for the first operator page
