# GardePro camera discovery firmware

Small USB-serial-only firmware for identifying support details for another GardePro trail camera model.

It intentionally excludes:

- onboard ESP camera
- SD storage
- HaLow HTTP server
- OTA
- Wi-Fi/BLE RF scanner uploads
- live-view tunnel

Use it only while the ESP32 is on USB near the camera.

## Build/flash

```bash
arduino-cli compile --upload \
  --fqbn heltec:esp_halow:HT-HC33:PSRAM=opi \
  --port /dev/ttyUSB0 \
  /home/matheau/esp32_camera/gardepro_camera_discovery
```

## Serial

Baud:

```text
115200
```

Commands:

```text
help
status
ble_scan [ms]
select_ble <mac>
wake [mac]
wifi_scan
select_wifi <ssid>
wifi_join [ssid]
probe
take_picture
standby
```

Expected workflow:

```text
ble_scan 20000
wake <new-camera-ble-mac>
wifi_scan
wifi_join <new-camera-ssid>
probe
take_picture
standby
```

Capture serial output and copy the `BLE_CAMERA`, `WIFI_AP`, and `CAMERA_GET` lines into the handoff notes.

## Confirmed camera profiles

### GardePro E6+

Bench-tested on 2026-08-20 with the camera next to the ESP32 over USB serial.

- BLE MAC: `a4:c1:38:98:81:48`
- BLE name: `CAM8Z8_NoName_G_E6+`
- BLE RSSI at bench: about `-50 dBm`
- advertised service: `6e000100-b5a3-f393-e0a9-e50e24dcca9e`
- advertised service data: `0x0001:A4C138988148`
- wake GATT service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- wake data characteristic: `6e400004-b5a3-f393-e0a9-e50e24dcca9e`
- wake payload: `AT+WAKEPULSE=10\r\n`
- wake result: `OK` notifications received
- Wi-Fi SSID: `CAM8Z8_A4C138988148`
- Wi-Fi password: `1234567890`
- ESP32 camera-side IP after join: `192.168.8.30`
- camera HTTP host/port: `192.168.8.1:8080`

Confirmed HTTP endpoints:

- `GET /cmd/standby/reset` -> `{"code":0}`
- `GET /cmd/info/1` -> brand `GardePro`, product/model `E6+`, version `V82.2.152 MCU V84`
- `GET /cmd/info/2` -> battery/power telemetry
- `GET /cmd/info/3` -> storage/media counters
- `GET /cmd/info/4` -> camera clock/timezone
- `GET /cmd/info/5` -> software/hardware/SD/BLE/Wi-Fi versions
- `GET /cmd/getParaSetting` -> full setting payload
- `GET /list/detail/backward/900000/20` -> media listing
- `GET /media/getIrStatus` -> IR status
- `GET /media/pic/take` -> `{"code":0,"desc":"OK"}`
- `GET /media/pic/result` -> `{"code":0,"data":{"fileIdx":1}}`
