# Laptop field setup for ESP32 trail-camera bridge

This guide is for setting up a laptop that can debug, build, and flash the HT-HC33 ESP32-S3 HaLow trail-camera bridge in the field.

## Repository

Browser repo:

```text
https://github.com/aboxfulloftide/esp332_camera_bridge
```

Main firmware sketch:

```text
gardepro_dual_radio_bridge/gardepro_dual_radio_bridge.ino
```

Current target board/FQBN:

```text
heltec:esp_halow:HT-HC33:PSRAM=opi
```

`PSRAM=opi` is required for the onboard camera’s UXGA mode.

## Recommended Windows setup

Use WSL2 Ubuntu if possible. It most closely matches the current development machine.

### Windows prerequisites

Install:

- Git for Windows
- Windows Terminal
- WSL2 with Ubuntu
- Arduino CLI in WSL
- Python 3 in WSL
- `pyserial` in WSL for serial diagnostics
- `usbipd-win` if flashing from WSL over USB

Native Windows can also work with Arduino IDE or Arduino CLI, but the existing scripts are Linux shell scripts.

## Clone or copy the repo

In WSL:

```bash
git clone https://github.com/aboxfulloftide/esp332_camera_bridge.git
cd esp332_camera_bridge
```

If copying the existing folder instead of cloning, copy the whole repository, not just the `.ino` file.

## Files that are not in git

The real local config is intentionally ignored.

Create:

```text
gardepro_dual_radio_bridge/local_config.h
```

Start from:

```text
gardepro_dual_radio_bridge/local_config.example.h
```

Required values:

```cpp
#define HALOW_SSID "tightbeam"
#define HALOW_PASS "your-password"
#define BOARD_HOSTNAME "trail_esp32"
#define UPSTREAM_API_HOST "192.168.1.42"
#define UPSTREAM_API_PORT 80
#define UPSTREAM_API_PREFIX "/trail_cam"
#define UPSTREAM_TUNNEL_HOST UPSTREAM_API_HOST
#define UPSTREAM_TUNNEL_PORT 6000
```

Do not commit `local_config.h`.

## Arduino CLI setup

Install Arduino CLI, then install/configure:

- ESP32 core
- Heltec ESP32 HaLow core
- NimBLE-Arduino

The current Linux machine has toolchain pieces under:

```text
/home/matheau/.arduino15/
/home/matheau/Arduino/hardware/heltec/esp_halow/
```

Those directories are not part of the repo. A copied repo alone is not enough to build.

Check installed cores:

```bash
arduino-cli core list
```

Expected current cores on the working machine:

```text
esp32:esp32      3.3.7
heltec:esp_halow 3.0.0
```

## USB access from WSL

On Windows, install `usbipd-win`, then from an elevated PowerShell:

```powershell
usbipd list
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

In WSL:

```bash
ls -l /dev/ttyUSB* /dev/ttyACM*
```

The current board normally appears as:

```text
/dev/ttyUSB0
```

## Build and flash over USB

From the repo root in WSL:

```bash
./build.sh /home/<user>/esp332_camera_bridge/gardepro_dual_radio_bridge /dev/ttyUSB0
```

If your local path differs, adjust the first argument.

Equivalent Arduino CLI command:

```bash
arduino-cli compile --upload \
  --fqbn heltec:esp_halow:HT-HC33:PSRAM=opi \
  --port /dev/ttyUSB0 \
  /path/to/esp332_camera_bridge/gardepro_dual_radio_bridge
```

## Serial debug

Serial baud:

```text
115200
```

Quick Python serial command helper:

```bash
python3 - <<'PY'
import time, serial
ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.2)
for cmd in ['status', 'halow_status', 'sd_status', 'onboard_status']:
    print('>>>', cmd)
    ser.write((cmd + '\n').encode())
    ser.flush()
    end = time.time() + 3
    out = b''
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            out += chunk
    print(out.decode('utf-8', 'replace'))
ser.close()
PY
```

Useful serial commands:

```text
status
halow_status
http_restart
sd_status
sd_mount
onboard_status
onboard_capture
bringup
stream_start
stream_stop
upload_status
```

Avoid `onboard_delete_all` in field testing.

## HTTP field checks

Current board address:

```text
http://192.168.1.160:18080
```

Use sequential requests. Avoid firing many curls in parallel.

```bash
curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/healthz
curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/status
curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/system/status
curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/jobs
curl -sS --connect-timeout 5 --max-time 10 http://192.168.1.160:18080/scanner/config
```

If the AP shows the board associated but HTTP is not responding, try serial:

```text
http_restart
```

If HTTP still works, remote listener restart is:

```bash
curl -sS --connect-timeout 5 --max-time 10 \
  -X POST http://192.168.1.160:18080/system/http_restart
```

## OTA

OTA exists but requires the board HTTP server to accept the upload.

Prefer USB flashing when the board is on the bench. Use OTA only when `/firmware/status` is reliable.

Build OTA binary:

```bash
arduino-cli compile \
  --fqbn heltec:esp_halow:HT-HC33:PSRAM=opi \
  --export-binaries \
  /path/to/esp332_camera_bridge/gardepro_dual_radio_bridge
```

Upload OTA:

```bash
curl -sS --connect-timeout 10 --max-time 300 \
  -F "firmware=@/path/to/gardepro_dual_radio_bridge/build/heltec.esp_halow.HT-HC33/gardepro_dual_radio_bridge.ino.bin" \
  http://192.168.1.160:18080/firmware/update
```

## Current API docs

- Board HTTP API: `gardepro_dual_radio_bridge/BOARD_HTTP_API.md`
- Onboard media API: `gardepro_dual_radio_bridge/ONBOARD_MEDIA_API.md`
- Field-service checks: `gardepro_dual_radio_bridge/FIELD_SERVICE_FIX_PLAN.md`
- Current handoff: `CURRENT_WORK_HANDOFF.md`
