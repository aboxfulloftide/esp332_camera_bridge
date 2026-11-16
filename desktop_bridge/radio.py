"""Camera radio ownership: BLE wake, camera hotspot join, standby, target selection.

Wraps the tested helpers in tools/pi_radio_test.py and tools/pi_trail_job.py.
Everything that touches BLE or the camera Wi-Fi interface takes `Radio.lock`.
"""

from __future__ import annotations

import asyncio
import json
import sys
import threading
import time
from pathlib import Path
from types import SimpleNamespace

import requests

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import pi_radio_test as prt  # noqa: E402
import pi_trail_job as ptj  # noqa: E402

log = prt.log

# Labels as in the firmware's CAMERA_PROFILES.
PROFILE_INFO = {
    "e6_plus": {"label": "GardePro E6+", "ble_name": "CAM8Z8_NoName_G_E6+"},
    "e6_original": {"label": "GardePro E6", "ble_name": "CAM8Z8_NoName_G_E6"},
}


class Radio:
    def __init__(self, cfg):
        self.cfg = cfg
        self.lock = threading.RLock()
        self.owner: str | None = None          # what currently holds the radio, for status
        self.ble = {"stage": "idle", "wake_attempted": 0, "wake_confirmed": 0, "last_rssi": None,
                    "best_rssi": None, "last_error": None, "notify_count": 0, "last_notify": None}
        self.timing: dict = {}
        self.camera_wifi_ever_connected = False
        self.camera = self._load_camera()
        prt.select_camera(self.camera)

    # ------------------------------------------------------------------ target
    def _load_camera(self) -> str:
        try:
            cam = json.loads(Path(self.cfg.state_path).read_text()).get("camera")
            if cam in prt.CAMERAS:
                return cam
        except (OSError, ValueError):
            pass
        return self.cfg.camera if self.cfg.camera in prt.CAMERAS else "e6_plus"

    def set_camera(self, cam: str) -> None:
        if cam not in prt.CAMERAS:
            raise ValueError(f"unknown camera {cam!r}")
        with self.lock:
            if cam == self.camera:
                return
            self.down()
            self.camera = cam
            prt.select_camera(cam)
            p = Path(self.cfg.state_path)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(json.dumps({"camera": cam}))
            log(f"Radio: camera target -> {cam}")

    def target_json(self) -> dict:
        mac, ssid, profile = prt.CAMERAS[self.camera]
        profiles = []
        for cid, (m, s, p) in prt.CAMERAS.items():
            profiles.append({"id": cid, "label": PROFILE_INFO.get(cid, {}).get("label", cid),
                             "ble_mac": m.lower(), "ble_name": PROFILE_INFO.get(cid, {}).get("ble_name", ""),
                             "wifi_ssid": s, "nm_profile": p, "selected": cid == self.camera})
        return {"active": {"id": self.camera, "ble_mac": mac.lower(),
                           "ble_name": PROFILE_INFO.get(self.camera, {}).get("ble_name", ""),
                           "wifi_ssid": ssid},
                "profiles": profiles, "recent_ble_devices": []}

    # ------------------------------------------------------------------ args for the tools
    def args(self, **over) -> SimpleNamespace:
        a = SimpleNamespace(
            cam_iface=self.cfg.cam_iface, uplink_iface=self.cfg.uplink_iface, ble_adapter=self.cfg.ble_adapter,
            no_wake=False, wait_minutes=self.cfg.bringup_minutes, lost_minutes=self.cfg.lost_minutes,
            ble_scan_seconds=self.cfg.ble_scan_seconds, item_seconds=self.cfg.item_seconds,
            max_rejoins=self.cfg.max_rejoins, limit=0, no_delete=False, force_11b=False,
            server=self.cfg.server, scanner_host=self.cfg.scanner_host, camera=self.camera,
            bridge_name="desktop", standby_after=True)
        for k, v in over.items():
            setattr(a, k, v)
        return a

    # ------------------------------------------------------------------ state
    def connected(self) -> bool:
        li = prt.link_info(self.cfg.cam_iface)
        if not li["connected"]:
            return False
        return prt.iface_state(self.cfg.cam_iface).get("GENERAL.CONNECTION") == prt.CAMERA_NM_PROFILE

    def wifi_status(self) -> dict:
        li = prt.link_info(self.cfg.cam_iface)
        st = prt.iface_state(self.cfg.cam_iface)
        connected = li["connected"] and st.get("GENERAL.CONNECTION") == prt.CAMERA_NM_PROFILE
        return {"wifi_connected": connected, "wifi_ip": st.get("IP4.ADDRESS[1]", "").split("/")[0] if connected else "",
                "interface": self.cfg.cam_iface, "nm_connection": st.get("GENERAL.CONNECTION") or None,
                "signal_dbm": li.get("signal_dbm"), "tx_bitrate": li.get("tx_bitrate"),
                "rx_bitrate": li.get("rx_bitrate"), "freq_mhz": li.get("freq_mhz"),
                "camera_target_wifi_ssid": prt.CAMERA_SSID}

    # ------------------------------------------------------------------ actions
    def _wake(self, progress) -> bool:
        progress("ble_wake")
        self.ble["stage"] = "scanning"
        self.ble["wake_attempted"] += 1
        t = time.time()
        try:
            r = asyncio.run(prt.ble_wake(prt.CAMERA_BLE_MAC, self.cfg.ble_scan_seconds, self.cfg.cam_iface,
                                         self.cfg.hotspot_wait_seconds, self.cfg.ble_adapter))
        except Exception as e:  # noqa: BLE001
            self.ble.update(stage="connect_failed", last_error=f"{type(e).__name__}: {e}")
            self.timing["last_ble_wake_elapsed_ms"] = int((time.time() - t) * 1000)
            log(f"Radio: BLE wake failed: {self.ble['last_error']}")
            return False
        self.timing["last_ble_wake_elapsed_ms"] = int((r.get("connect_s") or 0) * 1000 + (r.get("scan_s") or 0) * 1000)
        if r.get("hotspot_seen_s") is not None:
            self.timing["last_hotspot_wait_elapsed_ms"] = int(r["hotspot_seen_s"] * 1000)
        if not r.get("found"):
            self.ble.update(stage="not_found", last_error="camera_not_seen")
            return False
        rssi = r.get("ble_rssi")
        self.ble["last_rssi"] = rssi
        if rssi is not None and (self.ble["best_rssi"] is None or rssi > self.ble["best_rssi"]):
            self.ble["best_rssi"] = rssi
        notes = r.get("notifications") or []
        self.ble["notify_count"] += len(notes)
        if notes:
            self.ble["last_notify"] = notes[-1]["text"]
        self.ble["wake_confirmed"] += 1 if r.get("ok_notify") else 0
        self.ble.update(stage="wake_ok" if r.get("ok_notify") else "wake_sent", last_error=None)
        return r.get("hotspot_seen_s") is not None

    def bringup(self, progress=lambda p: None, owner: str = "bringup") -> bool:
        """Wake the camera (if needed), join its hotspot, verify camera HTTP."""
        with self.lock:
            self.owner = owner
            t0 = time.time()
            sampler = SimpleNamespace(phase="join")
            args = self.args()
            ok = self.connected()
            deadline = t0 + self.cfg.bringup_minutes * 60
            attempt = 0
            while not ok and time.time() < deadline:
                attempt += 1
                # A wake can be acknowledged without the hotspot ever appearing (seen on
                # the E6); joining then just burns 25 s per try, so wake again instead.
                if not self._wake(progress):
                    log(f"Radio: hotspot not up after wake attempt {attempt}; waking again")
                    continue
                progress("wifi_join")
                tj = time.time()
                ok = prt.ensure_camera_link(args, sampler)   # can still fail on a stale scan entry
                self.timing["last_wifi_join_elapsed_ms"] = int((time.time() - tj) * 1000)
            self.timing["last_bringup_attempts"] = attempt
            if ok:
                progress("camera_http")
                th = time.time()
                try:
                    requests.get(f"http://{prt.CAMERA_HOST}:{prt.CAMERA_PORT}/cmd/standby/reset", timeout=(5, 10))
                except requests.RequestException as e:
                    log(f"Radio: camera HTTP check failed: {e}")
                    ok = False
                self.timing["last_camera_http_elapsed_ms"] = int((time.time() - th) * 1000)
            self.timing["last_bringup_elapsed_ms"] = int((time.time() - t0) * 1000)
            self.timing["last_bringup_ok"] = ok
            if ok:
                self.camera_wifi_ever_connected = True
                progress("camera_wifi_connected")
            return ok

    def down(self, standby: bool = True) -> None:
        """Ask the camera to sleep, then drop the hotspot connection."""
        with self.lock:
            if standby and self.connected():
                try:
                    requests.get(f"http://{prt.CAMERA_HOST}:{prt.CAMERA_PORT}/cmd/standby/now", timeout=(3, 5))
                except requests.RequestException:
                    pass
            prt.run(prt.priv(["nmcli", "con", "down", prt.CAMERA_NM_PROFILE]), timeout=20)
            self.owner = None
