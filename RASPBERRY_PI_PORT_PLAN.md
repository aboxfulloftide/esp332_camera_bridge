# Raspberry Pi 3 Bridge: Port Plan

Date: 2026-10-31
Status: proposal, nothing implemented yet

## Purpose

Evaluate running the trail-camera bridge on a Raspberry Pi 3 instead of the
HT-HC33 ESP32 board. The goals are the same as the ESP32 bridge (see
`RELIABILITY_WORKLOG_2026-10-29.md`): wake the GardePro E6+ over BLE, join its
Wi-Fi hotspot, download every media item to local storage, delete each camera
copy only after the staged copy is verified, and upload staged media to the
server. Live view and the onboard noon capture are secondary.

## What exists today

All of the working logic is in the ESP32 firmware
(`gardepro_dual_radio_bridge/gardepro_dual_radio_bridge.ino`): BLE wake,
durable download-and-delete jobs, verify-before-delete, Range resume,
low-battery hold, link-loss handling and the board HTTP API.

`gardepro_bridge.py` is only a skeleton: an HTTP proxy to the camera plus UDP
forwarding for live view. It has no BLE wake, no job handling and no staging.
A Pi bridge is therefore mostly a port of the firmware logic to Python.

## 1. Radios and networking

This is the main decision.

The Pi 3 has the same radio layout as the ESP32: one 2.4 GHz Wi-Fi radio and
BLE 4.1 on one chip (BCM43438), with a small PCB antenna. When the Pi joins
the camera hotspot, that Wi-Fi radio is busy, so the Pi needs a separate
uplink to the server.

| Use | Uplink option |
|---|---|
| Bench test | Ethernet. Simplest; enough to prove the software. |
| Field | HaLow. Either a Linux-supported HaLow adapter on the Pi, or a standalone HaLow bridge box on the Pi's Ethernet port. The bridge box avoids driver work. |

### Camera link

The camera link is the project's bottleneck. During the 2026-10-30/31 run the
E6+ hotspot was at -90 to -97 dBm, which gave about 47 KB per connection
before the link dropped (roughly 6 items per hour overall).

The Pi 3's onboard antenna is probably worse than the ESP32 setup. A USB Wi-Fi
adapter with an external antenna, dedicated to the camera link, is the
cheapest way to address this. It also leaves the onboard radio free for BLE
or the uplink.

The ESP32 firmware (0.2.77) joins the camera as an 802.11b-only station so the
camera transmits with the more robust DSSS/CCK rates. The Pi's onboard
`brcmfmac` driver generally ignores forced bitrates, so this trick is not
available there; many USB adapters do support it
(`iw dev <if> set bitrates legacy-2.4 1 2 5.5 11`).

## 2. Power and storage

### Power

A Pi 3 draws about 1.5-2.5 W at idle, many times the ESP32 bridge's draw. The
current battery already falls below the 3.8 V durable-job threshold overnight
with the ESP32, so a Pi needs a much larger battery and solar setup, or mains
power.

### Storage

A Pi booting from an SD card that loses power suddenly risks the same
filesystem corruption seen on the ESP32's card. Recommended:

- A UPS HAT so the Pi can shut down cleanly when power fails.
- A read-only root filesystem (overlay).
- Staged media on a USB SSD rather than the boot card.

## 3. Software port

A single Python service, run under systemd.

| Piece | Pi implementation |
|---|---|
| BLE wake | `bleak`: connect to `a4:c1:38:98:81:48`, subscribe to notifications on `6e400003` and `6e400004`, write `AT+WAKEPULSE=10` to `6e400004`, wait for `OK`, then disconnect before joining Wi-Fi. |
| Camera Wi-Fi | NetworkManager (`nmcli`): join `CAM8Z8_A4C138988148` (the camera's factory hotspot password, as used by the firmware's `CAMERA_WIFI_PASS`) with `ipv4.never-default yes`, so the uplink keeps the default route. Route only `192.168.8.0/24` over the camera interface. Camera HTTP is `192.168.8.1:8080`. |
| Camera HTTP | `requests` with `Range` resume. The Linux TCP stack and HTTP library handle the chunked-transfer quirks the firmware works around. Endpoints: `/list/detail/backward/{cursor}/60` (gallery pages), `/file/{id}/JPG` and `/file/{id}/mp4`, `/cmd/delete/{id}/{ext}`, `/cmd/info/3` (storage counts), `/cmd/standby/reset`. |
| Durable job | SQLite for job and manifest state. Port the firmware rules (below). |
| Board API | Reimplement the subset of `gardepro_dual_radio_bridge/BOARD_HTTP_API.md` the server uses (`/status`, `/jobs`, `/trail-media/status`, `/trail-media/files`, `/control/summary`) on port 18080, so the server works unchanged. |
| Live view | Reuse the UDP 49152/49153 forwarder from `gardepro_bridge.py`. |
| Supervision | systemd service with `WatchdogSec=` and restart on failure. |

### Durable-job rules to port

These are the behaviors that took the ESP32 firmware the longest to get
right; keep them.

1. Only one durable job at a time. A second submission returns the existing
   job (`409 durable_job_busy`).
2. Build the full manifest by paginating the gallery until an empty page.
   Treat an empty gallery as authoritative only if `/cmd/info/3` also reports
   zero photos and videos.
3. Camera media IDs are reused after deletion. Record each staged file's
   source identity (`id|type|size|date|uid`) and trust staged or partial bytes
   only when it matches the manifest record.
4. Delete a camera item only after the staged file's size on disk matches the
   expected size and it passes a structural check (JPEG SOI/EOI markers; MP4
   `ftyp`/`mdat`/`moov` atoms).
5. Resume partial downloads with `Range`. A response that grew the partial
   file is progress, not a failure.
6. Storage faults never count against a media item.
7. Camera link loss re-runs BLE wake and Wi-Fi join; it does not count against
   the item (bounded, so a dead item still eventually fails).
8. Skip an item only after three failed attempts with no progress. Skipped
   items stay on the camera.
9. Back off when the camera is unreachable instead of failing the job
   quickly.
10. Pause below a minimum battery voltage and resume above a recovery
    voltage, if the Pi's power setup can report one.

## Suggested order

1. **Radio test (about half a day).** One script that performs the BLE wake,
   joins the camera hotspot, lists the gallery and downloads one video.
   Run it on the Pi at the bridge's real field location, with the onboard
   radio and with a USB adapter, and compare the camera RSSI and throughput
   with the ESP32's (-90 to -97 dBm, about 47 KB per connection). This
   decides whether a port is worth doing.
2. **Port the durable job and the board API** if the Pi's link is clearly
   better. Point the server at the Pi and run a full download-and-delete job.
3. **HaLow uplink and power** last, sized from what steps 1 and 2 show.

Do not run the Pi and the ESP32 bridge against the camera at the same time;
both would try to wake it and own its single Wi-Fi client slot.

## Requirements for the radio test

- Raspberry Pi OS (Bookworm) with NetworkManager.
- Python 3 with `bleak` and `requests`.
- Ethernet to the LAN for the uplink.
- Optional: a USB Wi-Fi adapter with an external antenna.
