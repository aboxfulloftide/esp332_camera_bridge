# Raspberry Pi Bridge: Port Plan

Date: 2026-10-31, updated 2026-11-02
Status: radio test and a first full download/delete/upload job done on a
Raspberry Pi 4 (see "Test results 2026-11-01/02"). Next: try the same tools
on a desktop with an ASUS X570-Plus onboard Wi-Fi/BT card.

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

There is no Ethernet connection. The Pi uses two Wi-Fi radios, one per job:

| Interface | Hardware | Role |
|---|---|---|
| `wlan0` | Onboard (`brcmfmac`), shares its chip with BLE | Uplink: joins the `hoth` network and holds the default route to the server. |
| `wlan1` | USB MediaTek MT7921U (`mt7921u`) | Camera link only: joins the camera hotspot. Never holds the default route. |
| `hci1` | USB Realtek BT 5.4 (`0bda:a728`, RTL8761BU) on a USB extension cable | Camera wake. The onboard BLE (`hci0`) could not hear the E6+. |

When the Pi joins the camera hotspot, only `wlan1` is busy, so the server
stays reachable over `wlan0` throughout a job.

### Uplink (`wlan0` on `hoth`)

- NetworkManager profile for `hoth` bound to `wlan0`
  (`connection.interface-name wlan0`), autoconnect on, so the Pi is
  reachable after boot without any other link.
- This is also the only way into the Pi for management, so any network
  change during testing must leave `wlan0`/`hoth` alone. A mistake that
  takes `wlan0` down needs local console access to fix.
- BLE and `wlan0` share the onboard chip and its antenna. Uplink traffic can
  interfere with the BLE wake; if wakes are unreliable, try pausing uplink
  transfers during the wake step.
- Field: `hoth` must reach the bridge site. If it does not, a HaLow bridge
  box (or another AP) providing `hoth` at the site replaces it without
  changing the Pi side.

### Camera link (`wlan1`)

The camera link is the project's bottleneck. During the 2026-10-30/31 run the
E6+ hotspot was at -90 to -97 dBm, which gave about 47 KB per connection
before the link dropped (roughly 6 items per hour overall).

The USB adapter is dedicated to the camera link, with an external antenna if
possible, and leaves the onboard chip to BLE and the uplink.

- `wlan1` is currently unmanaged by NetworkManager; it must be set managed
  (`nmcli dev set wlan1 managed yes`, plus persisting it if a config file
  marks it unmanaged).
- The camera profile is bound to `wlan1` (`connection.interface-name wlan1`)
  with `ipv4.never-default yes`, so the default route stays on `wlan0`.
- The camera hotspot uses `192.168.8.0/24`; check that `hoth` does not use
  the same subnet, or routes to the camera will be ambiguous.

The ESP32 firmware (0.2.77) joins the camera as an 802.11b-only station so the
camera transmits with the more robust DSSS/CCK rates. The radio test should
check whether `mt7921u` accepts forced legacy rates
(`iw dev wlan1 set bitrates legacy-2.4 1 2 5.5 11`). The onboard `brcmfmac`
generally ignores them, which no longer matters since it is not used for the
camera.

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
| Camera Wi-Fi | NetworkManager (`nmcli`): join `CAM8Z8_A4C138988148` (the camera's factory hotspot password, as used by the firmware's `CAMERA_WIFI_PASS`) on `wlan1` only (`connection.interface-name wlan1`), with `ipv4.never-default yes`, so `wlan0` on `hoth` keeps the default route. Route only `192.168.8.0/24` over `wlan1`. Camera HTTP is `192.168.8.1:8080`. |
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
   Run it on the Pi at the bridge's real field location, joining the camera
   on `wlan1` (USB) while staying connected to `hoth` on `wlan0`. Confirm the
   uplink stays up for the whole run, check whether forced 802.11b rates work
   on `wlan1`, and compare the camera RSSI and throughput with the ESP32's (-90 to -97 dBm, about 47 KB per connection). This
   decides whether a port is worth doing.
   Script: `tools/pi_radio_test.py` (uses the NetworkManager profile
   `gardepro-cam` on `wlan1`; never deletes camera media). Each run writes
   `report.json`, `samples.json`, `gallery.json`, `run.log` and the
   downloaded file to `../radio_test_runs/<timestamp>/`.
2. **Port the durable job and the board API** if the Pi's link is clearly
   better. Point the server at the Pi and run a full download-and-delete job.
3. **Field uplink (`hoth` coverage at the site, or a HaLow bridge providing
   it) and power** last, sized from what steps 1 and 2 show.

Do not run the Pi and the ESP32 bridge against the camera at the same time;
both would try to wake it and own its single Wi-Fi client slot.

## Requirements for the radio test

- Raspberry Pi OS (Bookworm) with NetworkManager.
- Python 3 with `bleak` and `requests`.
- Onboard Wi-Fi (`wlan0`) connected to `hoth` for the uplink; no Ethernet.
- USB Wi-Fi adapter (`wlan1`, MT7921U) managed by NetworkManager and
  reserved for the camera hotspot.
- `hoth` not using `192.168.8.0/24`.

## Test results 2026-11-01/02

Hardware: Raspberry Pi 4, Debian 13 (trixie), NetworkManager, BlueZ 5.82,
bleak 2.1.1. Wi-Fi regulatory country is unset (`country 00`); set it
(`sudo raspi-config` or `iw reg set`) before relying on the numbers.

### Wi-Fi camera link (`wlan1`, MT7921U)

Far better than the ESP32 at the same spot (-90 to -97 dBm, about 47 KB per
connection):

| Pi location | Camera signal | Throughput |
|---|---|---|
| Next to the ESP32 / first field spot | -51 to -65 dBm | 1.2-2.6 MB/s, full videos verified |
| Second spot (2026-11-02) | -72 to -75 dBm | about 660 KB/s |
| Third spot, much further (power-friendly) | not measured: BLE wake failed (below) | - |

Forced 802.11b rates were not needed.

### Full job (`tools/pi_trail_job.py`)

Run 2026-11-01 21:50-22:07, job directory
`../trail_staging/job_20261101-215006/`:

- Manifest matched the camera counters (500 = 237 photos + 263 videos).
- 165 items downloaded, verified, deleted from the camera and uploaded to the
  server (`http://192.168.1.42/trail_cam`, `scanner_host` `trail_pi`, 971 MB,
  0 failures). About 2.6 s per item.
- At 22:07 the camera hotspot disappeared (camera battery) and never came
  back. Nothing was lost; id 378 is a partial download still on the camera.
- **Job bug:** when the hotspot is gone, the job waits about 5.5 minutes per
  item and re-queues it, so it loops forever. It needs to give up after a few
  minutes of no hotspot, save state, and run the upload phase.

Camera gallery quirks found (both now handled in the Pi tools):

- `/list/detail/backward/{cursor}/60` is **exclusive**: it returns ids
  strictly below `cursor`. The next cursor is the page's minimum id, not
  minimum id - 1 (that skipped one item per page: 489 listed vs 500 counted).
- The same off-by-one is in the ESP32 firmware at
  `gardepro_dual_radio_bridge.ino:11330` (manifest cursor) and around `:10281`
  (delete-confirm lookup). **Not fixed yet.**
- JPEG check: the EOI marker can be anywhere in the last 4 KB, not only the
  last two bytes (matches the firmware).

### BLE wake: what was wrong, and what fixed it

The Pi could not wake the E6+ for most of the session, while the phone (at
twice the distance) and the ESP32 could. Causes, in order of impact:

1. **Software bug:** `tools/pi_radio_test.py` passed the adapter to bleak as
   `bluetooth_adapter="hci1"`. bleak only accepts `adapter=` and silently
   ignores unknown keywords, so every bleak scan and connect actually ran on
   the onboard radio (`hci0`), which has a weak antenna shared with `wlan0`
   and no LE Coded PHY. Fixed (`tools/pi_radio_test.py:174`).
2. **Antenna placement:** with the USB BT dongle plugged directly into the Pi,
   even raw `hcitool`/`btmon` captures on `hci1` never received the E6+. After
   moving the dongle onto a USB extension cable, the Pi heard it at about
   -82 dBm, similar to the ESP32 (-78 to -80 dBm). USB 3 noise from the
   MT7921U and USB drive also costs a little (about 15% of adverts lost while
   reading the USB 3 drive).
3. **The camera advertises only in bursts** (about 80 s seen while it was
   asleep). A short scan can miss it; scan for minutes, not seconds.

Ruled out with raw captures: rotating/private addresses, LE Coded PHY
(`hci1` supports it and it is selected; `tools/ble_phy.py` shows this),
directed advertising, and the phone acting as the advertiser. The E6+ uses a
normal connectable `ADV_IND` from its public address
`A4:C1:38:98:81:48` on LE 1M, carrying service data `a4c138988148` and the
`6e400001` service UUID.

On Linux, a "direct connect" to the public address (`tools/ble_direct.py`)
is an accept-list scan that connects on the first advert; it cannot connect
to a camera that is not advertising, and neither can the ESP32.

Result after the fixes (2026-11-02 23:13, second spot): found in 6 s at
-84 dBm, connected in 3.2 s, three `OK` replies, hotspot up 29 s later,
joined in 3.1 s, gallery listed (424 items), 5.5 MB video verified.

At the third, much further spot the camera was found at -82 to -86 dBm on
four tries, but every connect timed out after 20 s. The BLE link is about
3-7 dB short there.

### Tools

| File | Purpose |
|---|---|
| `tools/pi_radio_test.py` | Wake, join, list, download one item, report signal and speed. `--skip-wake` measures Wi-Fi only when the camera is already awake. |
| `tools/pi_trail_job.py` | Full durable job: manifest, download, verify, delete, upload. State in `<job_dir>/job_state.json`; `--resume`, `--no-delete`, `--skip-camera`, `--limit`. |
| `tools/pi_wake_loop.sh` | Retry the radio test unattended until it succeeds (default 4 h). |
| `tools/ble_direct.py` | Kernel L2CAP/ATT connect and minimal ATT client for the wake. |
| `tools/ble_phy.py` | Read or select the adapter's LE PHYs (`btmgmt phy` hangs on this Pi). |

All Linux-only (NetworkManager, BlueZ, raw Bluetooth sockets).

## Desktop bridge service (2026-11-02)

The ESP32 firmware's camera work is now ported to `desktop_bridge/` and runs on
the desktop as the systemd service `gardepro-desktop-bridge` (see
`DESKTOP_BRIDGE.md`). Verified on real hardware against the E6: bring-up from
sleep (13.7 s), camera proxy routes, photo trigger, live view through the GPRT
tunnel to a local tunnel server (ffmpeg decoded the H.264; about 1-2% RTP loss
on the camera Wi-Fi leg, none in the kernel), and a full download/delete/upload
job via `POST /jobs`. Registration with the server is off until the ESP32 is
powered down.

## Next steps

### Desktop test (ASUS X570-Plus onboard Wi-Fi/BT)

The point is antenna, not chip: the board's card has an external antenna on a
cable that can be raised and aimed, against the small antennas inside the
USB dongles. Expect a few dB to about 10 dB better, which is the BLE margin
missing at the third spot. Power draw (about 50-100 W against about 5 W)
rules it out as the field bridge; it is a test of how much a better antenna
buys.

1. Use Linux on the desktop (a live USB is enough). The tools need
   NetworkManager, BlueZ and Python 3 with `bleak` and `requests`. On Windows
   they will not run as written.
2. Identify the card (`lspci -nn | grep -i net`, `hciconfig -a`) and use its
   adapter names in place of `wlan1`/`hci1`. Keep the desktop's own network on
   Ethernet or another adapter so the camera link has its own radio.
3. Create the camera profile on the card's Wi-Fi interface: SSID
   `CAM8Z8_A4C138988148`, `ipv4.never-default yes`, `ipv6.method disabled`,
   autoconnect off, named `gardepro-cam`.
4. Put the antenna as high as possible with line of sight to the camera, then
   run, from the same spot as the Pi's failed test:
   `python3 tools/pi_radio_test.py --ble-adapter <hciN> --cam-iface <wlanN> --ble-scan-seconds 180 --pick largest`
5. Compare BLE RSSI and connect success, Wi-Fi signal and throughput with the
   table above.
6. Do not run the Pi, the desktop and the ESP32 against the camera at the same
   time; each would try to wake it and take its one Wi-Fi client slot.

### Desktop test result 2026-11-02 18:51

Hardware: the board's card is an Intel Wireless-AC 9260 (`wlp5s0`, BT `hci0`,
`8087:0025`), not an AX200. It was disabled in the BIOS (Advanced → Onboard
Devices Configuration); the PCIe root port was hidden until it was enabled.
Uplink on Ethernet `enp4s0`; regulatory country set to US; NM profile
`gardepro-cam` bound to `wlp5s0`. Python deps in `esp332_camera_bridge/.venv`.

Command: `.venv/bin/python tools/pi_radio_test.py --ble-adapter hci0
--cam-iface wlp5s0 --uplink-iface enp4s0 --ble-scan-seconds 180 --pick largest`
(run `../radio_test_runs/20261102-185146/`).

- BLE: found in 0.7 s at **-92 dBm**, connected in 8.4 s on the first try,
  three `OK` replies. (The Pi's dongle failed to connect at -82 to -86 dBm.)
- Hotspot up 46 s after the wake; joined in 3.1 s.
- Wi-Fi: -64 to -68 dBm, 65 Mbit/s TX; 10.5 MB video (id 468) verified at
  about 1.34 MB/s. Uplink pings 40/40.
- Camera: 424 items (208 photos, 216 videos), battery 100%.

Not yet a like-for-like comparison: record where the desktop/antenna was
relative to the Pi's three spots. (Desktop is in the garage, stock ASUS
antenna wall-mounted indoors.)

Full job on the desktop, 2026-11-02 18:56-19:29 (`tools/pi_trail_job.py
--cam-iface wlp5s0 --uplink-iface enp4s0 --ble-adapter hci0`, job directory
`../trail_staging/job_20261102-185540/`): all 426 items (209 photos,
217 videos, 2.1 GB) downloaded, verified, deleted and uploaded; 0 skipped,
0 failures, no link drops. Camera phase 28.5 min (about 4.0 s per item) at
-63 to -66 dBm; upload 4 min. Camera left with 0 items.

Job fixes made for this run: wake with bleak first (then `ble_direct`), always
re-wake when a join fails (NetworkManager's scan cache can list the hotspot
for minutes after the camera sleeps), stop the camera phase after
`--lost-minutes` (default 5) of no camera and still upload. The last one is
not yet tested against a real hotspot loss.

Original E6 (`--camera e6_original`: BLE `A4:C1:38:98:26:35`, SSID
`CAM8Z8_A4C138982635`, NM profile `gardepro-cam-e6`, firmware V82.2.157 MCU
V91), 2026-11-02 19:42-19:48, job `../trail_staging/job_20261102-194216/`:
BLE -72 dBm, Wi-Fi -48 to -54 dBm, about 2.7 MB/s. All 46 photos (64 MB)
downloaded, verified, deleted and uploaded (labelled `GardePro E6`). The
hotspot vanished at 19:42:33 mid-download; the job retried joins for the
5-minute item budget, then re-woke over BLE and finished. Since then the
download loop re-wakes on the first failed join (`rewake_on_loss`). One photo
(id 47, 19:42:09) was taken as the job started and was not in the manifest, so
it stayed on the camera.

### Pi follow-ups

1. Fix `tools/pi_trail_job.py` to stop and upload when the hotspot is gone
   for more than a few minutes, then resume the job for the remaining items
   (334 from the original manifest plus new captures; 424 on the camera at
   2026-11-02 23:14).
2. If the desktop shows a big antenna gain: fit the Pi with a USB BT adapter
   that has an external (RP-SMA) antenna, and a MediaTek USB Wi-Fi adapter
   with external antennas, both on extension cables away from the USB 3
   ports. The desktop's antennas may fit them.
3. Retry the bleak connect at weak signal: allow 40 s instead of 20 s and
   fall back to `tools/ble_direct.py` after a bleak connect timeout (today
   the fallback runs only when the scan finds nothing).
4. Set the Wi-Fi regulatory country.
5. Fix the gallery cursor off-by-one in the ESP32 firmware.
6. Commit the plan and `tools/` changes (not committed yet).
