"""GPRT tunnel framing and board registration (same wire format as the ESP32 firmware).

Header: "!4sBBBBIH" = magic "GPRT", version 1, stream_id, flags, reserved,
timestamp_ms, payload_len. Control frames use stream_id 0xFF with flags =
START(1) / STOP(2) / REGISTER(3) and a JSON payload. Media frames use
stream_id 0 (RTP) or 1 (RTCP). Parser: gardepro_tunnel_server.py.
"""

from __future__ import annotations

import json
import re
import socket
import struct
import subprocess
import threading
import time
from pathlib import Path

HEADER = struct.Struct("!4sBBBBIH")
MAGIC = b"GPRT"
VERSION = 1
STREAM_RTP, STREAM_RTCP, STREAM_CONTROL = 0, 1, 0xFF
CONTROL_START, CONTROL_STOP, CONTROL_REGISTER = 1, 2, 3

_T0 = time.monotonic()


def frame(stream_id: int, flags: int, payload: bytes) -> bytes:
    if len(payload) > 0xFFFF:
        raise ValueError("payload too large for a GPRT frame")
    ts = int((time.monotonic() - _T0) * 1000) & 0xFFFFFFFF
    return HEADER.pack(MAGIC, VERSION, stream_id, flags, 0, ts, len(payload)) + payload


def control_frame(control_type: int, metadata: dict) -> bytes:
    return frame(STREAM_CONTROL, control_type, json.dumps(metadata, separators=(",", ":")).encode())


def iface_identity(iface: str) -> dict:
    """IP/MAC/gateway of the uplink, reported under the firmware's halow_* names."""
    ip = mac = gw = ""
    out = subprocess.run(["ip", "-4", "-o", "addr", "show", "dev", iface], capture_output=True, text=True).stdout
    if m := re.search(r"inet (\d+\.\d+\.\d+\.\d+)", out):
        ip = m.group(1)
    try:
        mac = Path(f"/sys/class/net/{iface}/address").read_text().strip()
    except OSError:
        pass
    out = subprocess.run(["ip", "-4", "route", "show", "default", "dev", iface], capture_output=True, text=True).stdout
    if m := re.search(r"default via (\S+)", out):
        gw = m.group(1)
    return {"ip": ip, "mac": mac, "gateway": gw}


class Registrar(threading.Thread):
    """Sends REGISTER at start, every interval, and whenever the uplink IP changes."""

    def __init__(self, cfg, log):
        super().__init__(daemon=True, name="registrar")
        self.cfg, self.log = cfg, log
        self.stop_evt = threading.Event()
        self.kick = threading.Event()
        self.sent = self.failures = 0
        self.last_ok_at: float | None = None
        self.last_error: str | None = None
        self.last_ip = ""

    def metadata(self) -> dict:
        ident = iface_identity(self.cfg.uplink_iface)
        return {
            "type": "register",
            "hostname": self.cfg.hostname,
            "halow_ip": ident["ip"],
            "halow_mac": ident["mac"],
            "halow_bssid": "",
            "halow_rssi": 0,
            "halow_ssid": f"ethernet:{self.cfg.uplink_iface}",
            "halow_gateway": ident["gateway"],
            "bridge": "desktop",
            "http_port": self.cfg.http_port,
        }

    def send_once(self) -> bool:
        md = self.metadata()
        if not md["halow_ip"]:
            self.last_error = f"no IPv4 address on {self.cfg.uplink_iface}"
            return False
        try:
            with socket.create_connection((self.cfg.tunnel_host, self.cfg.tunnel_port), timeout=10) as s:
                s.sendall(control_frame(CONTROL_REGISTER, md))
            self.sent += 1
            self.last_ok_at = time.time()
            self.last_error = None
            self.last_ip = md["halow_ip"]
            self.log(f"Register: sent ip={md['halow_ip']} to {self.cfg.tunnel_host}:{self.cfg.tunnel_port}")
            return True
        except OSError as e:
            self.failures += 1
            self.last_error = f"{type(e).__name__}: {e}"
            self.log(f"Register: failed: {self.last_error}")
            return False

    def run(self) -> None:
        next_at = 0.0
        while not self.stop_evt.is_set():
            ip = iface_identity(self.cfg.uplink_iface)["ip"]
            if time.time() >= next_at or (ip and ip != self.last_ip) or self.kick.is_set():
                self.kick.clear()
                ok = self.send_once()
                next_at = time.time() + (self.cfg.register_interval_s if ok else 30)
            self.stop_evt.wait(10)

    def status(self) -> dict:
        return {"enabled": self.cfg.register, "target": f"{self.cfg.tunnel_host}:{self.cfg.tunnel_port}",
                "sent": self.sent, "failures": self.failures, "last_ip": self.last_ip,
                "last_ok_age_s": round(time.time() - self.last_ok_at, 1) if self.last_ok_at else None,
                "last_error": self.last_error}
