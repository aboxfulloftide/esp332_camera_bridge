#!/usr/bin/env python3
"""
Receive the ESP32 GPRT tunnel stream and fan media back out locally.

Protocol:
- 16-byte frame header:
    magic[4]     = b"GPRT"
    version[1]   = 1
    stream_id[1] = 0 video RTP, 1 video RTCP, 255 control
    flags[1]
    reserved[1]
    timestamp_ms[4] big-endian
    payload_len[2] big-endian
- followed by payload_len bytes

Control frames use stream_id 255 and flags:
- 1 start
- 2 stop
- 3 register
"""

from __future__ import annotations

import argparse
import json
import logging
import socket
import struct
import threading
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Optional


LOG = logging.getLogger("gardepro_tunnel_server")
HEADER_STRUCT = struct.Struct("!4sBBBBIH")
MAGIC = b"GPRT"
VERSION = 1
STREAM_RTP = 0
STREAM_RTCP = 1
STREAM_CONTROL = 255
CONTROL_START = 1
CONTROL_STOP = 2
CONTROL_REGISTER = 3


@dataclass
class TunnelConfig:
    bind_host: str = "0.0.0.0"
    bind_port: int = 6000
    udp_host: str = "127.0.0.1"
    udp_port_primary: int = 5004
    udp_port_secondary: int = 5005
    sdp_path: str = "/tmp/gardepro_live.sdp"
    registration_path: str = "/tmp/gardepro_board_registration.json"
    write_payload_dir: str = ""
    verbose_packets: bool = False
    packet_log_first: int = 20
    packet_log_every: int = 200
    stats_interval_sec: float = 5.0


@dataclass
class TunnelStats:
    started_at: float
    active_client: Optional[str] = None
    last_registration: Optional[dict] = None
    last_start_metadata: Optional[dict] = None
    last_stop_metadata: Optional[dict] = None
    last_packet_at: float = 0.0
    rtp_packets: int = 0
    rtcp_packets: int = 0
    control_frames: int = 0
    bytes_forwarded_primary: int = 0
    bytes_forwarded_secondary: int = 0
    bad_frames: int = 0


class GPRTTunnelServer:
    def __init__(self, config: TunnelConfig) -> None:
        self.config = config
        self.stats = TunnelStats(started_at=time.time())
        self._server_sock: Optional[socket.socket] = None
        self._stop = threading.Event()
        self._udp_primary = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._udp_secondary = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._udp_primary.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        self._udp_secondary.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        self._last_stats_log = 0.0

    def serve_forever(self) -> None:
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((self.config.bind_host, self.config.bind_port))
        server.listen(1)
        server.settimeout(1.0)
        self._server_sock = server

        LOG.info(
            "Listening for GPRT tunnel on %s:%d; UDP fanout primary=%s:%d secondary=%s:%d",
            self.config.bind_host,
            self.config.bind_port,
            self.config.udp_host,
            self.config.udp_port_primary,
            self.config.udp_host,
            self.config.udp_port_secondary,
        )

        while not self._stop.is_set():
            try:
                conn, addr = server.accept()
            except socket.timeout:
                continue
            except OSError:
                break

            peer = f"{addr[0]}:{addr[1]}"
            self.stats.active_client = peer
            LOG.info("Client connected: %s", peer)
            try:
                self._handle_client(conn, peer)
            except Exception:
                LOG.exception("Client handler failed for %s", peer)
            finally:
                try:
                    conn.close()
                except OSError:
                    pass
                if self.stats.active_client == peer:
                    self.stats.active_client = None
                LOG.info("Client disconnected: %s", peer)

    def stop(self) -> None:
        self._stop.set()
        if self._server_sock is not None:
            try:
                self._server_sock.close()
            except OSError:
                pass

    def status(self) -> dict:
        result = asdict(self.stats)
        result["uptime_sec"] = round(time.time() - self.stats.started_at, 1)
        return result

    def _handle_client(self, conn: socket.socket, peer: str) -> None:
        conn.settimeout(15.0)
        while not self._stop.is_set():
            header = self._recv_exact(conn, HEADER_STRUCT.size)
            if header is None:
                return

            magic, version, stream_id, flags, _reserved, ts_ms, payload_len = HEADER_STRUCT.unpack(header)
            if magic != MAGIC or version != VERSION:
                self.stats.bad_frames += 1
                LOG.error(
                    "Bad frame from %s: magic=%r version=%d payload_len=%d",
                    peer,
                    magic,
                    version,
                    payload_len,
                )
                return

            payload = self._recv_exact(conn, payload_len)
            if payload is None:
                return

            self.stats.last_packet_at = time.time()
            if stream_id == STREAM_CONTROL:
                self._handle_control_frame(flags, payload, peer)
                continue

            if stream_id == STREAM_RTP:
                self.stats.rtp_packets += 1
                self.stats.bytes_forwarded_primary += len(payload)
                self._udp_primary.sendto(payload, (self.config.udp_host, self.config.udp_port_primary))
                if self._should_log_packet(self.stats.rtp_packets):
                    LOG.info("RTP packet ts=%d len=%d flags=0x%02x", ts_ms, len(payload), flags)
                self._maybe_dump_payload("primary", payload, self.stats.rtp_packets)
                self._maybe_log_stats()
                continue

            if stream_id == STREAM_RTCP:
                self.stats.rtcp_packets += 1
                self.stats.bytes_forwarded_secondary += len(payload)
                self._udp_secondary.sendto(payload, (self.config.udp_host, self.config.udp_port_secondary))
                if self._should_log_packet(self.stats.rtcp_packets):
                    LOG.info("RTCP packet ts=%d len=%d flags=0x%02x", ts_ms, len(payload), flags)
                self._maybe_dump_payload("secondary", payload, self.stats.rtcp_packets)
                self._maybe_log_stats()
                continue

            self.stats.bad_frames += 1
            LOG.warning("Unknown stream_id=%d len=%d from %s", stream_id, len(payload), peer)

    def _handle_control_frame(self, flags: int, payload: bytes, peer: str) -> None:
        self.stats.control_frames += 1
        text = payload.decode("utf-8", errors="replace")
        try:
            metadata = json.loads(text) if text else {}
        except json.JSONDecodeError:
            metadata = {"raw": text}

        if isinstance(metadata, dict) and "peer" not in metadata:
            metadata["peer"] = peer

        if flags == CONTROL_REGISTER:
            self.stats.last_registration = metadata
            self._write_registration(metadata)
            LOG.info("REGISTER metadata: %s", json.dumps(metadata, indent=2))
            return

        if flags == CONTROL_START:
            self.stats.last_start_metadata = metadata
            LOG.info("START metadata: %s", json.dumps(metadata, indent=2))
            self._write_sdp(metadata)
            return

        if flags == CONTROL_STOP:
            self.stats.last_stop_metadata = metadata
            LOG.info("STOP metadata: %s", json.dumps(metadata, indent=2))
            return

        LOG.info("CONTROL flags=%d metadata=%s", flags, json.dumps(metadata))

    def _write_registration(self, metadata: dict) -> None:
        path = Path(self.config.registration_path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        LOG.info("Wrote registration to %s", path)

    def _maybe_dump_payload(self, label: str, payload: bytes, index: int) -> None:
        if not self.config.write_payload_dir:
            return
        base = Path(self.config.write_payload_dir)
        base.mkdir(parents=True, exist_ok=True)
        path = base / f"{label}-{index:06d}.bin"
        path.write_bytes(payload)

    def _write_sdp(self, metadata: dict) -> None:
        sdp = metadata.get("sdp")
        if not isinstance(sdp, str) or not sdp.strip():
            return

        # Rewrite the SDP so local players subscribe to the receiver fanout ports.
        rewritten_lines = []
        for raw_line in sdp.splitlines():
            line = raw_line.strip()
            if line.startswith("c=IN IP4 "):
                rewritten_lines.append(f"c=IN IP4 {self.config.udp_host}")
                continue
            if line.startswith("m=video "):
                rewritten_lines.append(f"m=video {self.config.udp_port_primary} RTP/AVP 96")
                continue
            rewritten_lines.append(line)
        rewritten = "\n".join(rewritten_lines) + "\n"

        path = Path(self.config.sdp_path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(rewritten, encoding="utf-8")
        LOG.info("Wrote SDP to %s", path)
        LOG.info("ffplay command: ffplay -protocol_whitelist file,udp,rtp -fflags nobuffer -flags low_delay %s", path)

    def _should_log_packet(self, packet_index: int) -> bool:
        if not self.config.verbose_packets:
            return False
        return (
            packet_index <= self.config.packet_log_first
            or (
                self.config.packet_log_every > 0
                and (packet_index % self.config.packet_log_every) == 0
            )
        )

    def _maybe_log_stats(self) -> None:
        if self.config.stats_interval_sec <= 0:
            return
        now = time.time()
        if now - self._last_stats_log < self.config.stats_interval_sec:
            return
        self._last_stats_log = now
        LOG.info(
            "Stats: rtp=%d rtcp=%d bytes_primary=%d bytes_secondary=%d bad_frames=%d client=%s",
            self.stats.rtp_packets,
            self.stats.rtcp_packets,
            self.stats.bytes_forwarded_primary,
            self.stats.bytes_forwarded_secondary,
            self.stats.bad_frames,
            self.stats.active_client or "-",
        )

    @staticmethod
    def _recv_exact(conn: socket.socket, size: int) -> Optional[bytes]:
        data = bytearray()
        while len(data) < size:
            try:
                chunk = conn.recv(size - len(data))
            except socket.timeout:
                continue
            except OSError:
                return None
            if not chunk:
                return None
            data.extend(chunk)
        return bytes(data)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Receive the ESP32 GPRT tunnel stream locally")
    parser.add_argument("--bind-host", default="0.0.0.0")
    parser.add_argument("--bind-port", type=int, default=6000)
    parser.add_argument("--udp-host", default="127.0.0.1")
    parser.add_argument("--udp-port-primary", type=int, default=5004)
    parser.add_argument("--udp-port-secondary", type=int, default=5005)
    parser.add_argument("--sdp-path", default="/tmp/gardepro_live.sdp")
    parser.add_argument("--registration-path", default="/tmp/gardepro_board_registration.json")
    parser.add_argument("--write-payload-dir", default="")
    parser.add_argument("--verbose-packets", action="store_true")
    parser.add_argument("--packet-log-first", type=int, default=20)
    parser.add_argument("--packet-log-every", type=int, default=200)
    parser.add_argument("--stats-interval-sec", type=float, default=5.0)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    server = GPRTTunnelServer(
        TunnelConfig(
            bind_host=args.bind_host,
            bind_port=args.bind_port,
            udp_host=args.udp_host,
            udp_port_primary=args.udp_port_primary,
            udp_port_secondary=args.udp_port_secondary,
            sdp_path=args.sdp_path,
            registration_path=args.registration_path,
            write_payload_dir=args.write_payload_dir,
            verbose_packets=args.verbose_packets,
            packet_log_first=args.packet_log_first,
            packet_log_every=args.packet_log_every,
            stats_interval_sec=args.stats_interval_sec,
        )
    )

    print(
        json.dumps(
            {
                "listen": f"{args.bind_host}:{args.bind_port}",
                "udp_primary": f"{args.udp_host}:{args.udp_port_primary}",
                "udp_secondary": f"{args.udp_host}:{args.udp_port_secondary}",
                "sdp_path": args.sdp_path,
                "registration_path": args.registration_path,
            },
            indent=2,
        )
    )

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        LOG.info("Stopping tunnel server")
    finally:
        server.stop()
        LOG.info("Final status: %s", json.dumps(server.status(), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
