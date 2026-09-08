# Server changes for ESP32 live-view firmware 0.2.7

Firmware 0.2.7 keeps the existing GPRT-over-TCP transport on port 6000. The
server does not need a protocol decoder change. It does need to treat stream
lifecycle, transport availability, packet reception, and video quality as
separate states.

## Before starting live view

1. Start and verify the GPRT TCP listener on the configured tunnel port.
2. Submit `POST /jobs?action=live_view_start` once.
3. Poll the returned `poll_path` (currently `/control/summary`) until the action
   finishes. Do not submit duplicate start jobs while `control_busy` is true.
4. Consider the control action successful only when `control_last_ok` is true.

The ESP32 now connects the GPRT tunnel after RTSP SETUP and before RTSP PLAY.
The listener therefore must be ready before the live-view job is submitted.

## Authoritative runtime state

Poll `GET /control/summary` for lightweight state:

- `live_view_active`: the camera RTSP session is active.
- `tunnel_connected`: the ESP32 has a connected GPRT transport.
- `control_busy`, `control_state`, `control_error`: job lifecycle.

Poll `GET /stream/status` for media health. Important fields under
`stream_status` are:

- `stream_session_id`: changes for every start or automatic recovery.
- `rtp_receiving`: RTP arrived recently, regardless of quality.
- `rtp_quality_ok`: RTP loss is within `rtp_quality_limit_pct`.
- `rtp_flowing`: compatibility field; true only when RTP is receiving,
  forwarding, and within the quality limit.
- `rtp_stalled`: an active session has stopped receiving RTP.
- `rtp_loss_pct`, `rtp_packets_missing`, `rtp_sequence_gaps`: current-session
  camera-to-ESP32 loss measurements.
- `rtp_packets_received_total`, `rtp_packets_forwarded_total`,
  `rtp_packets_missing_total`, `rtp_forward_failures_total`, and
  `udp_receive_overruns_total`: boot-lifetime counters that do not reset during
  automatic stream recovery.
- `forward_queue`: queue capacity, current depth, high-water mark, drops, and
  tunnel-write timing.
- `recovery`: last recovery reason, result, age, and duration.
- `udp_primary.receive_buffer_bytes` and
  `udp_secondary.receive_buffer_bytes`: actual socket receive-buffer sizes.

Continue accepting all pre-0.2.7 fields and ignore unknown fields. For older
firmware that lacks `rtp_receiving`, derive only an `unknown` or legacy state;
do not equate `rtp_flowing=false` with no packets arriving.

## Recommended server state model

Use these independent states:

| Condition | Server presentation |
|---|---|
| `live_view_active=false` | stopped |
| live view active, tunnel disconnected | local stream / transport offline |
| tunnel connected, `rtp_receiving=false` | waiting for media or stalled |
| `rtp_receiving=true`, `rtp_quality_ok=false` | streaming, degraded |
| `rtp_receiving=true`, `rtp_quality_ok=true` | streaming, healthy |
| board HTTP unreachable | board state unknown; do not infer stopped |

Key records by board identity and `stream_session_id`. When the session ID
changes, start a new per-session sample series while retaining the boot-total
counters. A counter decrease indicates a board reboot unless the session ID
also changed and the field is a current-session counter.

## Alerts and acceptance checks

- Warn when `forward_queue.dropped` or `udp_receive_overruns` increases.
- Warn when `rtp_forward_failures` increases or the tunnel disconnects.
- Report degraded quality when `rtp_quality_ok=false` for at least three
  consecutive samples; do not call it stopped while `rtp_receiving=true`.
- Record a recovery event whenever `stream_session_id` changes or
  `recovery.started_age_ms` resets.
- During a 2–5 minute acceptance test, require HTTP responsiveness, continued
  growth in received and forwarded totals, zero queue drops, zero forwarding
  failures, and loss within the configured tolerance.

## Stopping live view

Submit `POST /jobs?action=live_view_stop`, poll `/control/summary`, and finish
only when `control_busy=false`, `live_view_active=false`, and
`tunnel_connected=false`. A temporary HTTP outage makes the result unknown; it
does not prove the stop failed.

