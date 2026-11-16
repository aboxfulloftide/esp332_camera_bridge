"""Run the desktop bridge: python -m desktop_bridge [--config PATH] [--no-register]."""

from __future__ import annotations

import argparse
import os
import signal
import socket
import threading
import time
import uuid
from pathlib import Path

from . import VERSION
from .config import Config
from .control import Control
from .jobs import Jobs
from .live import LiveView
from .radio import Radio, log
from .server import BridgeHTTPServer
from .tunnel import Registrar, iface_identity


def sd_notify(msg: str) -> None:
    """Minimal sd_notify(3) so systemd Type=notify / WatchdogSec work without extra packages."""
    addr = os.environ.get("NOTIFY_SOCKET")
    if not addr:
        return
    if addr.startswith("@"):
        addr = "\0" + addr[1:]
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
            s.connect(addr)
            s.sendall(msg.encode())
    except OSError:
        pass


class Bridge:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.started = time.time()
        self.boot_id = uuid.uuid4().hex[:12]
        self.http_requests = 0
        self.radio = Radio(cfg)
        self.control: Control | None = None
        self.live = LiveView(cfg, on_stall=lambda: self.control and self.control.stall_callback())
        self.jobs = Jobs(cfg, self.radio, live_active=lambda: self.live.active,
                         control_busy=lambda: self.control is not None and self.control.active is not None)
        self.control = Control(cfg, self.radio, self.live, jobs_busy=self.jobs.busy_reason)
        self.registrar = Registrar(cfg, log) if cfg.register else None

    def uptime_ms(self) -> int:
        return int((time.time() - self.started) * 1000)

    def uplink_ip(self) -> str:
        return iface_identity(self.cfg.uplink_iface)["ip"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", type=Path, help="JSON settings file (default /etc/gardepro-bridge.json)")
    ap.add_argument("--no-register", action="store_true", help="do not send REGISTER to the server")
    ap.add_argument("--port", type=int, help="HTTP port (default 18080)")
    args = ap.parse_args()

    cfg = Config.load(args.config)
    if args.no_register:
        cfg.register = False
    if args.port:
        cfg.http_port = args.port

    log(f"Desktop bridge {VERSION} starting: camera iface {cfg.cam_iface}, BLE {cfg.ble_adapter}, "
        f"uplink {cfg.uplink_iface}, server {cfg.server}, register={cfg.register}")
    bridge = Bridge(cfg)
    host = cfg.http_host
    if host == "uplink":
        for _ in range(30):                     # at boot the uplink may still be getting its address
            host = bridge.uplink_ip()
            if host:
                break
            time.sleep(2)
        else:
            raise SystemExit(f"no IPv4 address on {cfg.uplink_iface}; cannot bind the HTTP API")
    httpd = BridgeHTTPServer((host, cfg.http_port), bridge)
    threading.Thread(target=httpd.serve_forever, daemon=True, name="http").start()
    if bridge.registrar:
        bridge.registrar.start()
    log(f"HTTP API listening on {host}:{cfg.http_port}; active camera {bridge.radio.camera}")

    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stop.set())
    sd_notify("READY=1")
    while not stop.wait(20):
        # Watchdog: only pet it while the HTTP listener still answers.
        try:
            socket.create_connection((host if host != "0.0.0.0" else "127.0.0.1", cfg.http_port), timeout=5).close()
            sd_notify("WATCHDOG=1")
        except OSError:
            log("Watchdog: HTTP listener not answering")
    log("Shutting down")
    sd_notify("STOPPING=1")
    bridge.live.stop("shutdown")
    bridge.jobs.stop_evt.set()
    httpd.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
