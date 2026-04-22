# GardePro Camera HTTP Candidates

This file separates camera HTTP paths into two evidence levels:

- `confirmed`: directly exercised against the live camera
- `candidate`: extracted from the Android app binary and not yet confirmed on the camera

## Confirmed

- `GET /cmd/delete/<type>/<id>` or `GET /cmd/delete/<id>/<type>`
- `GET /cmd/standby/reset`
- `GET /cmd/standby/now`
- `GET /cmd/getParaSetting`
- `GET /cmd/getSetting`
- `GET /cmd/info/1`
- `GET /cmd/info/2`
- `GET /cmd/info/3`
- `GET /cmd/info/4`
- `GET /cmd/info/5`
- `GET /cmd/info/6`
- `GET /list/detail/backward/900000/60`
- `GET /media/getIrStatus`
- `GET /media/pic/take`
- `GET /media/pic/result`
- `GET /media/video/start`
- `GET /media/video/stop`
- `GET /cmd/reboot`
- `POST /cmd/setSetting`
- `POST /cmd/setGmtClock`
- `GET /file/<id>/JPG`
- `GET /file/<id>/mp4`
- `GET /thumb/<id>/JPG`

Confirmed observed behavior:

- `/cmd/getSetting` returns the full current settings object, including mode, PIR, intervals, timezone, camera name, standby timeout, and WiFi flag.
- `/media/pic/result` returns the latest `fileIdx`.
- `/media/pic/take` successfully created photo `111` during live testing.
- `/media/video/start` returned `{ "code": 0, "desc": "OK" }`.
- `/media/video/stop` returned `{ "code": 0, "desc": "success" }`.
- `/cmd/standby/now` returned `{ "code": 0 }`.
- `/cmd/reboot` is real and causes the camera hotspot to disappear for a slower-than-normal recovery window.
- delete syntax findings:
  - `/cmd/delete/114` -> `wrong file para`
  - `/cmd/delete/982b6007` -> `wrong file para`
  - `/cmd/delete/1/114` -> `success`
  - `/cmd/delete/114/1` -> `success`
  - `/cmd/delete/1/982b6007` -> `file does not exist`
  - `/cmd/delete/982b6007/1` -> `wrong file para`
  - `/cmd/delete/128/JPG` -> `success`
  - `/cmd/delete/128/1` -> `file does not exist` when retried after the extension-based delete
  - `/cmd/delete/128/mp4` -> `success`
  - `/cmd/delete/2/128` -> `success`
  - after the successful delete tests, gallery item `114` disappeared and photo count dropped from `114` to `112`
- settings-write syntax findings:
  - `/cmd/setGmtClock`, `/cmd/setGmtClock?tz=US/Eastern`, and `/cmd/setGmtClock?time_zone=US/Eastern` all returned `code: -3`
  - `/cmd/setSetting`, `/cmd/setSetting?date_format=1`, `/cmd/setSetting?time_format=0`, and `/cmd/setSetting?standby_timeout=300` all returned `code: -2`
- POST JSON syntax findings:
  - `POST /cmd/setSetting` with body `{"data":{"date_format":0}}` returned `code: 0`
  - reading `/cmd/getSetting` afterward confirmed `date_format` changed to `0`
  - `POST /cmd/setSetting` with body `{"data":{"date_format":1}}` returned `code: 0`
  - reading `/cmd/getSetting` afterward confirmed `date_format` changed back to `1`
  - `POST /cmd/setGmtClock` with body `{"data":"2026-04-21 20:05:00"}` returned `code: 0, desc: success`

Current `/file/` probing status:

- `/file/111` returned an empty `500` through the board raw proxy
- `/file/111.JPG` caused the board raw proxy connection to reset
- `/file/2f1a892d` caused the board raw proxy connection to reset
- `/file/114` returned a real camera-side `500`
- `/file/1/115` returned a real camera-side `500`
- `/file/115/1` caused a connection reset
- `/file/2/112` caused a connection reset
- `/thumb/126/JPG` downloaded successfully as a valid JPEG thumbnail
- `/thumb/112/JPG` downloaded successfully as a valid JPEG thumbnail for a video item
- `/file/126/JPG` downloaded successfully as a valid full-resolution JPEG after the board raw relay was updated to dechunk camera responses
- `/file/112/MP4` failed
- `/file/112/mp4` downloaded successfully as a valid MP4 file

Interpretation:

- the camera-side file path shape is now partially confirmed:
- the camera-side delete path shape is now partially confirmed:
  - photos: `/file/<id>/JPG`
  - thumbnails: `/thumb/<id>/JPG`
  - videos: `/file/<id>/mp4`
- extension-based delete is the cleanest confirmed form:
  - photos: `/cmd/delete/<id>/JPG`
  - videos: `/cmd/delete/<id>/mp4`
- legacy type/id delete still works in some cases, so server code should keep it only as a fallback
- extension case matters at least for video download; uppercase `MP4` failed while lowercase `mp4` succeeded
- the board `/camera/raw` relay now dechunks camera file responses, which was required for valid full-file downloads

## Candidates From `libapp.so`

- `GET /cmd/delete/`
- `GET /cmd/format/result`
- `GET /cmd/format/start`
- `GET /cmd/info/`
- `GET /cmd/resetFact`
- `GET /cmd/setGmtClock`
- `GET /cmd/setSetting`
- `GET /cmd/upgrade/result`
- `GET /cmd/upgrade/start`
- `GET /file/`
- `GET /media/setDayNightMode`
- `GET /media/video/stop?`

## Current Server-Side Support

The current server helper and Python API now expose wrappers or raw-path access for:

- `get_settings()`
- `get_setting_values()`
- `get_gallery()`
- `get_info(index)`
- `get_ir_status()`
- `take_picture()`
- `picture_result()`
- `start_video_recording()`
- `stop_video_recording_path(...)`
- `format_sd_start()`
- `format_sd_result()`
- `reboot_camera()`
- `reset_camera_factory()`
- `standby_now()`
- `camera_request_json(method, path)`
- `camera_request_json_body(method, path, payload)`
- `download_file_to_path(file_path, dest_path)`

For any path that is still only a candidate, prefer testing via:

```bash
python3 /home/matheau/esp32_camera/gardepro_server_control.py camera-request --camera-path /candidate/path
```

or for file payloads:

```bash
python3 /home/matheau/esp32_camera/gardepro_server_control.py file-download --camera-path relative/or/full/path --output /tmp/out.bin
```
