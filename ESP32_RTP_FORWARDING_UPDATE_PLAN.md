# ESP32 RTP forwarding update plan

## Current deployed state

The stable firmware is deployed and includes:

- 64 KB receive-buffer request on the camera-side UDP sockets.
- RTP sequence telemetry in `GET /stream/status`.
- Received, forwarded, missing, out-of-order, and failed packet counters.
- RTP loss percentage and keyframe detection.
- `rtp_flowing` is false when recent RTP is not being forwarded or loss exceeds 20%.
- RTSP keepalive and automatic stream recovery.

The last outdoor test measured approximately 56% RTP loss, with zero tunnel send failures and zero reported UDP overruns. This indicates that the direct receive task is still spending too much time writing to the HaLow TCP tunnel.

## Next firmware change

Implement a bounded receive/forward pipeline:

1. Keep the camera UDP receive task dedicated to `recvfrom()` and packet accounting.
2. Use fixed-size packet buffers and a bounded queue/ring buffer; do not allocate per packet.
3. Give the receive task higher priority than HTTP, BLE, camera control, and tunnel reconnect work.
4. Run tunnel writes in a separate forwarding task while preserving packet order.
5. Count queue-full events as `udp_receive_overruns` and expose queue depth/high-water mark.
6. Connect the GPRT tunnel before RTSP `PLAY` so SPS/PPS/IDR packets are not lost during startup.
7. Keep the 64 KB UDP receive-buffer request and RTP telemetry.

## Safety requirements

- Use a small bounded queue first (for example 8–16 packets) and check allocation failure.
- Do not start tasks until all shared objects are initialized.
- Keep the existing stable direct-forwarding firmware available for rollback.
- Compile and flash by USB first; verify boot, `/diagnostics/status`, and `/stream/status` before testing live view.
- If the board does not boot, reflash the known-good binary over USB before continuing.
- Do not flash an untested queue implementation OTA while the board is outside.

## Acceptance test

During a 2–5 minute live-view test, record:

- `rtp_packets_received`
- `rtp_packets_forwarded`
- `rtp_packets_missing`
- `rtp_sequence_gaps`
- `rtp_forward_failures`
- `udp_receive_overruns`
- `rtp_loss_pct`
- `tunnel_packets_sent`
- `rtsp_keepalive_failures`

The update is successful when the board remains responsive, `udp_receive_overruns` and `rtp_forward_failures` remain zero, tunnel packets continue increasing, and RTP loss is within the server's configured tolerance.

## Outside-laptop workflow

Clone `https://github.com/aboxfulloftide/esp332_camera_bridge.git`, install Arduino CLI and the Heltec ESP HaLow board package, connect the ESP32 by USB, and run:

```bash
arduino-cli compile --export-binaries --fqbn 'heltec:esp_halow:HT-HC33:PSRAM=opi' gardepro_dual_radio_bridge
arduino-cli upload -p /dev/ttyUSB0 --fqbn 'heltec:esp_halow:HT-HC33:PSRAM=opi' gardepro_dual_radio_bridge
```

After flashing, verify the board's assigned HaLow IP with the local network tools, then query `/diagnostics/status` and `/stream/status` before moving the unit outside.
