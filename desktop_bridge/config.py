"""Settings: defaults, overridden by a JSON file (default /etc/gardepro-bridge.json)."""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]          # esp332_camera_bridge/
DEFAULT_CONFIG_PATH = Path(os.environ.get("GARDEPRO_BRIDGE_CONFIG", "/etc/gardepro-bridge.json"))


@dataclass
class Config:
    hostname: str = "trail_desktop"
    # "uplink" = bind to the uplink interface's IPv4 address only. This machine already
    # has llama-server on 127.0.0.1:18080, so a 0.0.0.0 bind would fail.
    http_host: str = "uplink"
    http_port: int = 18080

    cam_iface: str = "wlp5s0"            # Wi-Fi card used only for the camera hotspot
    uplink_iface: str = "enp4s0"         # interface that reaches the server
    ble_adapter: str = "hci0"
    camera: str = "e6_plus"              # initial target; the active one persists in state_path

    server: str = "http://192.168.1.42/trail_cam"
    scanner_host: str = "trail_desktop"
    tunnel_host: str = "192.168.1.42"
    tunnel_port: int = 6000
    register: bool = True                # send REGISTER so the server uses this bridge
    register_interval_s: int = 300

    staging_dir: str = str(REPO_ROOT.parent / "trail_staging")
    state_path: str = str(REPO_ROOT.parent / "trail_staging" / "bridge_state.json")

    # Camera bring-up and durable job tuning (same meaning as tools/pi_trail_job.py)
    ble_scan_seconds: float = 120
    hotspot_wait_seconds: float = 75     # E6 hotspot has taken up to ~53 s after a good wake
    bringup_minutes: float = 4
    lost_minutes: float = 5
    item_seconds: float = 300
    max_rejoins: int = 10
    retry_quick_count: int = 3
    retry_quick_s: int = 60
    retry_backoff_s: int = 900           # firmware: every 15 min after 3 quick retries
    retry_give_up_s: int = 3 * 3600      # firmware: fail after ~3 h of consecutive failures
    upload_sweep_s: int = 30

    # Live view
    media_port_primary: int = 25748
    media_port_secondary: int = 25749
    rtsp_keepalive_s: float = 5
    rtp_stall_s: float = 12
    rtp_first_packet_s: float = 20
    rtp_keyframe_gate: bool = True          # forward only complete frames; after loss wait for the next IDR

    # Sessions
    session_default_lease_s: int = 120

    extra: dict = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path | None = None) -> "Config":
        path = path or DEFAULT_CONFIG_PATH
        cfg = cls()
        if path.exists():
            data = json.loads(path.read_text())
            known = {f.name for f in fields(cls)}
            for k, v in data.items():
                if k in known:
                    setattr(cfg, k, v)
                else:
                    cfg.extra[k] = v
        return cfg

    def to_dict(self) -> dict:
        return asdict(self)
