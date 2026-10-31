# ESP32 Trail-Camera Bridge Reliability Worklog

Last updated: 2026-10-29  
Current deployed firmware: `0.2.76`  
Board address: `192.168.1.160:18080`

## Project goal

The bridge must reliably wake and connect to a GardePro trail camera, provide live view when requested, download all camera media to the ESP32 SD card, delete camera files only after a verified download, and upload staged files to the server. A submitted durable job must continue without a server connection or HaLow connection and must recover after a reset. The onboard ESP32 camera is only used for one scheduled image at noon.

## Main problems observed

### HaLow and HTTP availability

- The ESP32 sometimes remained pingable while port `18080` stopped accepting HTTP connections.
- Long synchronous HTTP or SD operations could prevent the single web-server task from servicing `/status` and `/healthz`.
- The old health check tested the upstream server instead of the HaLow gateway. An upstream outage could therefore cause the ESP32 to tear down a healthy HaLow link.
- Repeated `HaLow.begin()` calls could restart an association before the radio had enough time to finish connecting.
- The failsafe previously watched whether `handleClient()` returned, not whether the HTTP listener actually accepted a real request.
- Wi-Fi power saving and ARP behavior made `.160` intermittently unreachable from some LAN hosts even when other hosts could reach it.

### Trail-camera bring-up

- BLE wake and camera Wi-Fi association are unreliable at weak signal levels. During the latest test, the E6+ was seen around `-78` to `-80 dBm` and several BLE connections failed.
- A weak BLE connection has caused a panic reset. The durable job recovered after reboot, but wake attempts are slow.
- Bring-up used to retry indefinitely or appear stuck. It now uses bounded attempts and reports `camera_bringup_failed` instead of remaining permanently hung.
- BLE is now closed immediately after wake so it does not remain active while camera Wi-Fi is in use.
- The E6 and E6+ profiles have been confused in the past. The current selected E6+ is:
  - BLE MAC: `a4:c1:38:98:81:48`
  - Camera Wi-Fi SSID: `CAM8Z8_A4C138988148`

### Durable download-and-delete jobs

- Early server behavior treated HaLow loss as job failure and could submit duplicate jobs. The ESP32 is now the owner of an accepted job.
- Jobs and manifests are persisted to SD, with an NVS mirror used when SD is unavailable.
- A reboot during a job restores it as `waiting_retry`/`resume_after_reset` and continues automatically.
- Only one durable media job is allowed at a time. HTTP `409 durable_job_busy` means the server should adopt the returned existing job rather than submit another one.
- Earlier manifest pagination only processed a subset (often about 40 files). Pagination was changed to build a complete manifest.
- A file is deleted from the trail camera only after the staged SD copy has the expected size.
- A failed or incomplete download must never cause deletion of the camera copy.
- Three consecutive no-progress attempts may skip a bad media item. Failures that make real partial progress must not consume these attempts.
- Two entries in the current long-running job were skipped because their failure counters were accumulated by older firmware before the newer recovery behavior was installed. They remain on the camera and will require a fresh cleanup job.

### Camera HTTP transfer behavior

- The camera uses chunked HTTP and can pause between chunks.
- Arduino's default one-second stream timeout caused valid slow responses to be reported as `invalid_chunk`.
- Different files repeatedly stopped at exactly `47,104` bytes, showing that the camera can end or separate responses at a fixed block boundary.
- Range resume works: the partial file advanced from `47,104` to `94,208` bytes, and later from about `94 KB` to `1.72 MB`.
- Firmware `0.2.76` treats a closed response as useful progress when the `.part` file grew. It keeps the partial file and quickly issues another Range request without charging a media failure attempt.
- Empty chunk-header lines are ignored for up to 60 seconds; malformed non-empty chunk headers still fail.

### SD-card instability

- SD mounting has intermittently failed at boot even when the card was untouched.
- The card has returned zero-byte writes during downloads, including errors such as:
  - `sd_short_write_expected_4096_got_0`
  - `manifest_attempt_write_failed`
  - total/used capacity temporarily returning zero
- The card sometimes identifies electrically while the FAT volume becomes unavailable.
- The card was initially operated at 4 MHz. It now starts at 1 MHz and falls back to 400 kHz. During the latest manual recovery, it required 400 kHz before mounting successfully.
- SD writes now tolerate bounded busy/erase latency for roughly one second instead of failing after only about 60 ms.
- Periodic diagnostic-journal writes previously accessed SD without the shared mutex and could collide with a camera download. They now use the storage lock and are suppressed during an open camera-to-SD transfer.
- Trail-media list, file-download, and diagnostic-log handlers now return storage busy while a camera transfer owns the SD filesystem.
- Capacity refresh now uses the storage mutex and cached capacity is used by status endpoints.
- A storage fault during a durable job is no longer treated as a bad camera media item. The firmware remounts SD, reloads the manifest/job, and retries the same entry.
- A bug prevented background SD recovery when a durable job existed only in the NVS mirror. Firmware `0.2.76` removes that guard so recovery continues until SD mounts and the full job is loaded.
- Remaining concern: repeated zero-byte writes and the need to fall back to 400 kHz indicate the physical SD card, socket, wiring, power integrity, or FAT filesystem may still be marginal. Firmware recovery reduces data loss but cannot make failing hardware healthy.

### Battery and power

- Voltage has varied substantially with USB and camera/radio load. Recent readings included about `4.07 V` on USB and `3.79-3.83 V` after USB was unplugged.
- Durable media work pauses below `3.8 V` and resumes only after recovery to `3.9 V`.
- The low-battery recheck interval was reduced from five minutes to 30 seconds so a brief load sag no longer looks like a hung job.
- The safety threshold should not be lowered casually. Brownout or unstable voltage during FAT writes is a likely contributor to SD corruption.
- The latest validation stopped at `low_battery_wait` with the partial file safely preserved.

### Staged-media upload to the server

- Staged media was downloaded to the ESP32 but often never uploaded to the server.
- Large uploads closed after a repeatable byte count, and retry behavior could remain on one file indefinitely.
- Upload selection was changed to prefer smaller files first and to skip a file after three failed upload attempts.
- The uploader is preempted while a durable camera job is active so it cannot contend with camera-to-SD writes.
- The firmware can prefer the configured 2.4 GHz transfer network (`hoth`) and fall back to HaLow. Both networks share the same subnet, so source-interface binding and routing are important.
- Upload success still needs an end-to-end validation after the download-and-delete path completes.

### Live view

- The old explicit stream-state field disappeared, leaving the server to infer state from `control_last_action`.
- Firmware now exposes an explicit `live_view_active` boolean in `/control/summary`.
- Live-view start/stop are real actions, and the server should use the explicit flag rather than infer sticky state from unrelated bookkeeping fields.
- The 6+/E6+ live stream has still shown intermittent startup failures tied to camera bring-up and weak signal.

### Wi-Fi/BLE scanner code and radio ownership

- General Wi-Fi/BLE observation scanning is not part of the core camera-bridge goal and created unnecessary radio/filesystem contention.
- Scheduled scanner operation is disabled in camera-priority firmware.
- The observation queue still exists for legacy data but the scanner task is not started.
- Camera control, transfer Wi-Fi, and idle recovery must share radio ownership so one task cannot disconnect another task's active transfer.

## Reliability changes currently in firmware

Firmware versions `0.2.67` through `0.2.76` introduced the following relevant behavior:

1. Durable camera jobs preempt staged-media upload work.
2. Low-battery hold at 3.8 V with 3.9 V recovery.
3. BLE closes immediately after camera wake.
4. SD clock reduced from 4 MHz to 1 MHz, with 400 kHz fallback.
5. Background SD journal and HTTP SD readers are excluded during camera writes.
6. Camera stream timeout and chunk parsing tolerate slow/blank chunk boundaries.
7. SD busy writes receive bounded extended retries.
8. SD/manifest failures trigger remount and job reload rather than consuming a media attempt.
9. Partial Range progress is persisted and immediately resumed without consuming a no-progress attempt.
10. SD recovery continues when the durable job is available only from NVS.
11. The web listener, long operations, HaLow recovery, and failsafe diagnostics have been hardened in earlier revisions.

## Latest live-test state

Job ID: `2906781`  
Action: `download_and_delete_media`  
Manifest size: `275` entries  
Last tested media ID: `496`  
Preserved partial size: approximately `1.72 MB` of `10.52 MB`  
Last state: `waiting_retry / low_battery_wait`  
Last observed battery: approximately `3.83 V` after USB removal  
SD: mounted with approximately `51.9 GB` free  
Firmware: `0.2.76`, installed successfully by OTA

The newer partial-resume behavior is proven to advance the staged file without deleting the camera source. A complete file download followed by a verified camera deletion has not yet been observed under `0.2.76` because the unplugged battery fell below the safety threshold.

## Required next validation

1. Supply stable power or allow the battery to recover above 3.9 V.
2. Confirm the existing job resumes media ID 496 from its `.part` file.
3. Watch `item_bytes_received` reach `item_expected_bytes`.
4. Confirm the job increments both `downloaded` and `deleted` for the same item.
5. Confirm it advances to the next manifest entry without rebuilding or losing state.
6. Reboot during a later item and confirm SD remount, job reload, and Range resume.
7. After the durable job is no longer active, verify staged-media upload to the server, including the preferred 2.4 GHz path and HaLow fallback.
8. Run a fresh download-and-delete cleanup job so entries skipped by older firmware are retried.

## Hardware follow-up recommended

- Test with a known-good, high-endurance SD card freshly formatted as FAT32/exFAT as supported by the board.
- Inspect and clean the SD contacts and verify the socket mechanically retains the card.
- Measure the 3.3 V rail at the SD card during writes and camera/radio activity.
- Verify adequate bulk and local decoupling near the SD socket and radio modules.
- Investigate the battery/solar charging path and the large voltage drop after USB removal.
- Improve ESP32-to-camera placement or antenna orientation; `-78` to `-80 dBm` BLE is marginal for reliable wake and reconnect operations.

## Server behavior that must be preserved

- Submit a durable job once and persist its ESP32-scoped `job_id`.
- Treat `queued`, `running`, and `waiting_retry` as active.
- Do not mark a job failed merely because HaLow is offline.
- Do not submit a replacement job after a connection loss or a `409 durable_job_busy` response.
- Reconcile with `GET /trail-media/status` after reconnecting.
- Consider the camera job complete only when `status == "succeeded"`.
- Retrieve staged files only after job success and verify the returned byte count.
- Parse JSON tolerantly and ignore unknown fields.
- Use `live_view_active` from `/control/summary` rather than inferring live-view state.

