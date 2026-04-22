# GardePro WiFi Bridge Notes

## Status

This file started as early APK-only hypothesis notes.

Current confirmed values from packet capture and on-device testing are:

- hotspot IP: `192.168.8.1`
- HTTP control port: `8080`
- live media: UDP from camera ports `49152` and `49153`
- BLE bootstrap runs first and appears to wake/authenticate the camera before WiFi control is fully useful

Use [GARDEPRO_PROTOCOL_FINDINGS.md](/home/matheau/esp32_camera/GARDEPRO_PROTOCOL_FINDINGS.md) as the current source of truth. The remaining speculative items in this file are kept only as reverse-engineering context.

## Goal

Use the ESP32-based board as an intermediary:

1. Join the trail camera's local WiFi hotspot as a client.
2. Reach the camera's local control and live-view interfaces.
3. Forward that traffic over a second long-range WiFi link to a server.
4. View/control the camera remotely from farther away.

## Current Repo Status

- `take_photo/` and `stream_test/` are local ESP32 camera examples only.
- Nothing in this repo yet proxies a third-party trail camera.
- The APK analysis gives enough structure to guide packet capture.

## APK Findings

The app `com.zpszjs.gardepro.mobile_2.2.28.apk` is a Flutter app. Most WiFi-camera logic is compiled into `lib/arm64-v8a/libapp.so`, but plaintext strings still reveal the local interface shape.

### Early APK-Only Camera Base Hypothesis

- `http://192.168.5.20:9081`

This was an early APK-only guess and is now superseded by the confirmed endpoint `http://192.168.8.1:8080`.

### Discovered HTTP API Paths

- `/wifi/status`
- `/wifi/scan`
- `/wifi/live`
- `/wifi/gallery/`
- `/cmd/getParaSetting`
- `/cmd/getSetting`
- `/cmd/setSetting`
- `/cmd/setGmtClock`
- `/cmd/standby/reset`
- `/cmd/standby/now`
- `/cmd/format/start`
- `/cmd/format/result`
- `/cmd/delete/`
- `/cmd/reboot`
- `/cmd/resetFact`
- `/cmd/info/`
- `/cmd/upgrade/start`
- `/cmd/upgrade/result`
- `/live.sdp`

### Authentication Hints

The app contains these strings:

- `Password consists of 4-digit. If not assigned, password protection is disabled.`
- `step_5_checkPWD ==== 1 - no password required`
- `step_5_checkPWD ==== 2 - password required`
- `Enter camera password`
- `pwdStr converted password`
- `authHeaders`
- `www-authenticate`
- `authorization`
- `authChallenge`

This strongly suggests:

- The camera may support no password or a 4-digit local password.
- The app performs an explicit password check before normal API usage.
- HTTP auth headers may be involved, or the app may convert the 4-digit password into another request field/header.

### Streaming Hints

The app contains these strings:

- `rtsp://127.0.0.1:`
- `wifiCameraRTSPProxyPort`
- `wifiCameraAPIProxyPort`
- `/live.sdp`
- `package:p2p_rtsp/p2p_rtsp.dart`

Most likely interpretation:

- The app talks to the camera over local hotspot HTTP and/or RTSP.
- The app may run a local proxy on the Android device and then point its player to `rtsp://127.0.0.1:<port>/live.sdp`.
- The camera itself may expose RTSP directly, or the app may translate another local stream into RTSP for its player.

## Practical Reverse-Engineering Plan

### 1. Capture the Initial Connect Sequence

On the Android tablet, capture while doing only this:

1. Join the camera hotspot.
2. Open the app.
3. Connect to the WiFi camera.
4. Enter the camera password if prompted.
5. Stop before opening gallery or live view.

What to look for:

- First HTTP request to `192.168.8.1:8080`
- Request method, path, headers, and body
- Whether the app probes `/wifi/status` first
- Whether auth is Basic, Digest, custom header, or JSON/body based

### 2. Capture Settings Fetch

After initial connection succeeds, capture:

1. Open the settings page.
2. Refresh device status.

What to look for:

- Requests to `/cmd/getParaSetting` and `/cmd/getSetting`
- Response format: JSON, plaintext, XML, or binary
- Parameter names that define camera state

### 3. Capture Live View Start

Capture only the transition into live view.

What to look for:

- Request to `/wifi/live`
- Request to `/live.sdp`
- Any RTSP `DESCRIBE`, `SETUP`, `PLAY`
- Any separate API call that returns a stream port or session token

### 4. Capture Gallery Listing and Download

Capture:

1. Open gallery.
2. Open one image.
3. Download one item if the app offers it.

What to look for:

- Listing endpoint under `/wifi/gallery/`
- Thumbnail vs original image fetch
- File naming scheme
- Delete operation under `/cmd/delete/`

## Recommended tcpdump Workflow

On Android, save raw PCAPs for separate actions instead of one huge capture:

- `connect-only.pcap`
- `settings-load.pcap`
- `live-start.pcap`
- `gallery-list.pcap`
- `gallery-download.pcap`

If you can filter to the camera IP:

```bash
tcpdump -i any -s 0 -w connect-only.pcap host 192.168.8.1
```

If DNS or discovery may matter, first take one broader capture, then narrow later.

## What the ESP32 Bridge Actually Needs To Do

The bridge does not need to emulate the Android app. It only needs to preserve camera-side protocol behavior.

### Minimum Viable Bridge

1. ESP32 joins the trail camera hotspot.
2. ESP32 opens TCP connections to the camera API/stream ports.
3. ESP32 exposes a simpler upstream interface to your server.
4. A server-side process relays or translates requests from your viewing client to the camera.

### Better Split

- ESP32: transport bridge only
- Server: protocol-aware proxy

Reason:

- The trail camera protocol is still unknown.
- HTTP auth/state handling and RTSP proxying are easier on the server than on the ESP32.
- Packet-for-packet forwarding is easier to debug when reverse engineering.

## Important Hardware Constraint

If this design depends on one ESP32 radio joining two unrelated WiFi networks at the same time, that usually does not work unless the hardware truly has two independent radios or both links can share channel constraints.

For this project to be practical, you likely need one of these:

- A board with separate hotspot-side and long-range radios
- A hotspot-side ESP32 plus a second upstream radio/module
- A bridge where the ESP32 talks to another transport device over UART/SPI/Ethernet

The presence of both `WiFi.h` and `HaLow.h` in this repo suggests your board may already be intended for a dual-link setup. That should be confirmed before writing bridge firmware.

## Immediate Next Step

The highest-value artifact is a clean `connect-only.pcap` showing:

- first request
- first response
- password exchange
- status/settings bootstrap sequence

Once you have that capture, the next implementation target should be a small server-side proxy that reproduces the same request sequence against the confirmed camera endpoint.
