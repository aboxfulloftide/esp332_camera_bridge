#!/usr/bin/env python3
"""Raspberry Pi radio test for the GardePro E6+ trail camera.

Step 1 of RASPBERRY_PI_PORT_PLAN.md: BLE wake, join the camera hotspot on the
USB adapter, list the gallery, download one video, and report camera RSSI and
throughput so they can be compared with the ESP32 bridge (-90 to -97 dBm,
about 47 KB per connection).

Radio layout (see the plan):
  wlan0  onboard, joined to the uplink network (hoth), holds the default route
  wlan1  USB adapter, used only for the camera hotspot
  BLE    onboard, camera wake

This script is read-only towards the camera: it never deletes media.
Do not run it while the ESP32 bridge is also talking to the camera.

Usage:
  python3 tools/pi_radio_test.py                 # full run
  python3 tools/pi_radio_test.py --skip-wake     # camera hotspot already up
  python3 tools/pi_radio_test.py --force-11b     # try 802.11b-only TX rates on wlan1
  python3 tools/pi_radio_test.py --list-only     # wake, join, list; no download
"""

from __future__ import annotations

import argparse
import asyncio
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

import requests

CAMERA_BLE_MAC = "A4:C1:38:98:81:48"
CAMERA_SSID = "CAM8Z8_A4C138988148"
CAMERA_NM_PROFILE = "gardepro-cam"
# Known cameras (same as CAMERA_PROFILES in the ESP32 firmware):
# id -> (BLE MAC, hotspot SSID, NetworkManager profile name)
CAMERAS = {
    "e6_plus": ("A4:C1:38:98:81:48", "CAM8Z8_A4C138988148", "gardepro-cam"),
    "e6_original": ("A4:C1:38:98:26:35", "CAM8Z8_A4C138982635", "gardepro-cam-e6"),
}


def select_camera(name: str) -> None:
    """Point the module at one of CAMERAS (the helpers read these globals)."""
    global CAMERA_BLE_MAC, CAMERA_SSID, CAMERA_NM_PROFILE
    CAMERA_BLE_MAC, CAMERA_SSID, CAMERA_NM_PROFILE = CAMERAS[name]
CAMERA_HOST = "192.168.8.1"
CAMERA_PORT = 8080
BLE_NOTIFY_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
BLE_DATA_UUID = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
BLE_WAKE = b"AT+WAKEPULSE=10\r\n"
HTTP_KEEPALIVE_S = 15  # same interval as the firmware

# Bounded so a long-running service (desktop_bridge) does not grow without limit.
LOG_LINES: collections.deque[str] = collections.deque(maxlen=50000)


def log(msg: str) -> None:
    line = f"{datetime.now().strftime('%H:%M:%S.%f')[:-3]} {msg}"
    LOG_LINES.append(line)
    print(line, flush=True)


def priv(cmd: list[str]) -> list[str]:
    if os.geteuid() == 0:
        return cmd
    return ["sudo", "-n", *cmd]


def run(cmd: list[str], timeout: float = 30, check: bool = False) -> subprocess.CompletedProcess:
    try:
        cp = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        cp = subprocess.CompletedProcess(cmd, 124, "", "timeout")
    if check and cp.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed ({cp.returncode}): {cp.stderr.strip() or cp.stdout.strip()}")
    return cp


# --------------------------------------------------------------------------- network helpers

def iface_state(iface: str) -> dict:
    out = run(["nmcli", "-t", "-f", "GENERAL.STATE,GENERAL.CONNECTION,IP4.ADDRESS,IP4.GATEWAY",
               "dev", "show", iface]).stdout
    info = {}
    for line in out.splitlines():
        k, _, v = line.partition(":")
        info[k] = v
    return info


def link_info(iface: str) -> dict:
    """Parse `iw dev <iface> link` (signal, bitrates)."""
    out = run(["iw", "dev", iface, "link"]).stdout
    info: dict = {"connected": out.startswith("Connected")}
    if m := re.search(r"signal:\s*(-?\d+)", out):
        info["signal_dbm"] = int(m.group(1))
    if m := re.search(r"tx bitrate:\s*(.+)", out):
        info["tx_bitrate"] = m.group(1).strip()
    if m := re.search(r"rx bitrate:\s*(.+)", out):
        info["rx_bitrate"] = m.group(1).strip()
    if m := re.search(r"freq:\s*([\d.]+)", out):
        info["freq_mhz"] = float(m.group(1))
    return info


def route_dev(dest: str) -> str | None:
    out = run(["ip", "route", "get", dest]).stdout
    m = re.search(r"\bdev\s+(\S+)", out)
    return m.group(1) if m else None


def uplink_gateway(iface: str) -> str | None:
    out = run(["ip", "route", "show", "default", "dev", iface]).stdout
    m = re.search(r"default via (\S+)", out)
    return m.group(1) if m else None


def scan_for_ssid(iface: str, ssid: str) -> dict | None:
    out = run(["nmcli", "-t", "-f", "SSID,SIGNAL,CHAN,FREQ", "dev", "wifi", "list",
               "ifname", iface, "--rescan", "yes"], timeout=30).stdout
    for line in out.splitlines():
        parts = line.split(":")
        if parts and parts[0] == ssid:
            return {"signal_pct": int(parts[1] or 0), "channel": parts[2], "freq": parts[3]}
    return None


class Sampler(threading.Thread):
    """Samples camera-link RSSI and uplink reachability every couple of seconds."""

    def __init__(self, cam_iface: str, uplink_iface: str, gateway: str | None, interval: float = 2.0):
        super().__init__(daemon=True)
        self.cam_iface, self.uplink_iface, self.gateway = cam_iface, uplink_iface, gateway
        self.interval = interval
        self.samples: list[dict] = []
        self.stop_evt = threading.Event()
        self.phase = "init"

    def run(self) -> None:
        while not self.stop_evt.is_set():
            li = link_info(self.cam_iface)
            up = None
            if self.gateway:
                up = run(["ping", "-c1", "-W1", "-I", self.uplink_iface, self.gateway], timeout=3).returncode == 0
            self.samples.append({"t": time.time(), "phase": self.phase, "cam_connected": li["connected"],
                                 "cam_signal_dbm": li.get("signal_dbm"), "cam_rx": li.get("rx_bitrate"),
                                 "uplink_ok": up})
            self.stop_evt.wait(self.interval)

    def summary(self) -> dict:
        sig = [s["cam_signal_dbm"] for s in self.samples if s["cam_signal_dbm"] is not None]
        up = [s["uplink_ok"] for s in self.samples if s["uplink_ok"] is not None]
        return {
            "samples": len(self.samples),
            "cam_signal_dbm_min": min(sig) if sig else None,
            "cam_signal_dbm_max": max(sig) if sig else None,
            "cam_signal_dbm_avg": round(sum(sig) / len(sig), 1) if sig else None,
            "uplink_ping_ok": sum(up),
            "uplink_ping_fail": len(up) - sum(up),
        }


# --------------------------------------------------------------------------- BLE wake

async def ble_wake(mac: str, scan_timeout: float, cam_iface: str, hotspot_timeout: float,
                   adapter: str | None = None) -> dict:
    from bleak import BleakClient, BleakScanner

    result: dict = {"found": False, "ok_notify": False, "notifications": []}
    t0 = time.time()
    found = {}

    def on_adv(device, adv):
        if device.address.upper() == mac.upper():
            found["device"], found["rssi"] = device, adv.rssi

    kw = {"adapter": adapter} if adapter else {}  # bleak ignores unknown keywords such as bluetooth_adapter
    result["adapter"] = adapter or "default"
    log(f"BLE: scanning for {mac} on {result['adapter']} (up to {scan_timeout:.0f}s)")
    async with BleakScanner(detection_callback=on_adv, **kw):
        while "device" not in found and time.time() - t0 < scan_timeout:
            await asyncio.sleep(0.5)
    if "device" not in found:
        log("BLE: camera not seen")
        return result
    result.update(found=True, ble_rssi=found["rssi"], scan_s=round(time.time() - t0, 1))
    log(f"BLE: found, RSSI {found['rssi']} dBm")

    def on_notify(char, data: bytearray):
        text = bytes(data).decode(errors="replace").strip()
        result["notifications"].append({"char": str(char.uuid), "text": text})
        log(f"BLE notify {str(char.uuid)[:8]}: {text!r}")
        if text == "OK":
            result["ok_notify"] = True

    t1 = time.time()
    async with BleakClient(found["device"], timeout=20, **kw) as client:
        result["connect_s"] = round(time.time() - t1, 1)
        log(f"BLE: connected in {result['connect_s']}s")
        notify = client.services.get_characteristic(BLE_NOTIFY_UUID)
        data = client.services.get_characteristic(BLE_DATA_UUID)
        if data is None:
            raise RuntimeError("BLE characteristic 6e400004 not found")
        for ch in (notify, data):
            if ch is not None and ({"notify", "indicate"} & set(ch.properties)):
                await client.start_notify(ch, on_notify)
        await asyncio.sleep(0.15)
        for attempt in range(1, 4):
            log(f"BLE: wake pulse {attempt}/3")
            await client.write_gatt_char(data, BLE_WAKE, response=True)
            await asyncio.sleep(0.35)
        await asyncio.sleep(2.0)
        log(f"BLE: wake {'acknowledged (OK)' if result['ok_notify'] else 'sent, no OK notify'}")

        # Hold the BLE session until the hotspot is visible, then disconnect
        # before joining Wi-Fi (as in the plan).
        t2 = time.time()
        log(f"Wi-Fi: waiting for hotspot {CAMERA_SSID} on {cam_iface}")
        while time.time() - t2 < hotspot_timeout:
            seen = await asyncio.to_thread(scan_for_ssid, cam_iface, CAMERA_SSID)
            if seen:
                result["hotspot_seen_s"] = round(time.time() - t2, 1)
                result["hotspot_scan"] = seen
                log(f"Wi-Fi: hotspot visible after {result['hotspot_seen_s']}s ({seen})")
                break
            await asyncio.sleep(2)
        else:
            log("Wi-Fi: hotspot not seen before timeout")
    log("BLE: disconnected")
    return result


# --------------------------------------------------------------------------- camera HTTP

class Camera:
    def __init__(self) -> None:
        self.base = f"http://{CAMERA_HOST}:{CAMERA_PORT}"
        self.s = requests.Session()
        self.lock = threading.Lock()
        self.last_request = time.time()

    def get_json(self, path: str, timeout: float = 15) -> dict:
        r = self.s.get(self.base + path, timeout=(5, timeout))
        self.last_request = time.time()
        r.raise_for_status()
        return r.json()

    def keepalive(self) -> bool:
        try:
            requests.get(self.base + "/cmd/standby/reset", timeout=(3, 5))
            return True
        except requests.RequestException:
            return False

    def gallery(self, max_pages: int = 256) -> list[dict]:
        items: list[dict] = []
        cursor = 900000
        for page in range(max_pages):
            for attempt in range(1, 4):
                try:
                    body = self.get_json(f"/list/detail/backward/{cursor}/60", timeout=20)
                    break
                except requests.RequestException as e:
                    self.page_retries = getattr(self, "page_retries", 0) + 1
                    log(f"HTTP: gallery page {page} attempt {attempt}/3 failed: {type(e).__name__}")
                    if attempt == 3:
                        raise
                    self.keepalive()
                    time.sleep(2 * attempt)
            data = body.get("data") or []
            page_items = [i for i in data if int(i.get("id", 0)) > 0 and int(i.get("type", 0)) in (1, 2)]
            items.extend(page_items)
            log(f"HTTP: gallery page {page} cursor={cursor} items={len(page_items)}")
            if not page_items:
                break
            # The listing returns IDs strictly below the cursor, so the next
            # page starts at the lowest ID seen (not lowest - 1, which would
            # skip one item per page).
            min_id = min(int(i["id"]) for i in page_items)
            if min_id <= 1:
                break
            if min_id >= cursor:
                raise RuntimeError("gallery pagination did not advance")
            cursor = min_id
        return items


class KeepAlive(threading.Thread):
    def __init__(self, cam: Camera):
        super().__init__(daemon=True)
        self.cam = cam
        self.stop_evt = threading.Event()
        self.ok = self.fail = 0

    def run(self) -> None:
        while not self.stop_evt.wait(HTTP_KEEPALIVE_S):
            if self.cam.keepalive():
                self.ok += 1
            else:
                self.fail += 1


def verify_media(path: Path, kind: int, expected: int) -> dict:
    size = path.stat().st_size if path.exists() else 0
    res = {"size_on_disk": size, "size_ok": size == expected, "structure_ok": False}
    if not size:
        return res
    with path.open("rb") as f:
        if kind == 1:  # JPEG: SOI at start, EOI in the last 4 KB (cameras pad after EOI)
            head = f.read(2)
            f.seek(-min(size, 4096), os.SEEK_END)
            res["structure_ok"] = head == b"\xff\xd8" and b"\xff\xd9" in f.read()
        else:  # MP4: walk top-level atoms
            atoms, pos = [], 0
            while pos + 8 <= size:
                f.seek(pos)
                hdr = f.read(8)
                n, name = int.from_bytes(hdr[:4], "big"), hdr[4:8].decode("latin1")
                if n == 1:
                    n = int.from_bytes(f.read(8), "big")
                elif n == 0:
                    n = size - pos
                if n < 8:
                    break
                atoms.append(name)
                pos += n
            res["atoms"] = atoms
            res["structure_ok"] = {"ftyp", "mdat", "moov"} <= set(atoms)
    return res


def ensure_camera_link(args, sampler: Sampler) -> bool:
    """(Re)join the camera hotspot on the camera interface."""
    li = link_info(args.cam_iface)
    if li["connected"] and iface_state(args.cam_iface).get("GENERAL.CONNECTION") == CAMERA_NM_PROFILE:
        return True
    sampler.phase = "join"
    t = time.time()
    cp = run(priv(["nmcli", "--wait", "45", "con", "up", CAMERA_NM_PROFILE]), timeout=60)
    ok = cp.returncode == 0
    log(f"Wi-Fi: join {'ok' if ok else 'FAILED'} in {time.time() - t:.1f}s"
        + ("" if ok else f": {cp.stderr.strip() or cp.stdout.strip()}"))
    if ok and args.force_11b:
        apply_11b(args.cam_iface)
    return ok


def apply_11b(iface: str) -> bool:
    cp = run(priv(["iw", "dev", iface, "set", "bitrates", "legacy-2.4", "1", "2", "5.5", "11"]))
    ok = cp.returncode == 0
    log(f"Wi-Fi: force 802.11b TX rates on {iface}: {'accepted' if ok else 'rejected: ' + cp.stderr.strip()}")
    return ok


def download(cam: Camera, item: dict, dest: Path, args, sampler: Sampler) -> dict:
    kind = int(item["type"])
    expected = int(item.get("size", 0))
    path = f"/file/{item['id']}/" + ("JPG" if kind == 1 else "mp4")
    stats: dict = {"path": path, "expected_bytes": expected, "connections": [], "rejoins": 0}
    t_start = time.time()
    no_progress = 0
    sampler.phase = "download"
    while True:
        have = dest.stat().st_size if dest.exists() else 0
        if expected and have >= expected:
            break
        if time.time() - t_start > args.max_seconds:
            log("Download: time budget exhausted")
            stats["stopped"] = "time_budget"
            break
        if no_progress >= 3:
            log("Download: 3 attempts with no progress, giving up")
            stats["stopped"] = "no_progress"
            break
        if not ensure_camera_link(args, sampler):
            stats["rejoins"] += 1
            if stats["rejoins"] > args.max_rejoins:
                stats["stopped"] = "rejoin_limit"
                break
            if args.rewake_on_loss:
                try:
                    asyncio.run(ble_wake(CAMERA_BLE_MAC, args.ble_scan_seconds, args.cam_iface, 60, args.ble_adapter))
                except Exception as e:  # noqa: BLE001
                    log(f"BLE: re-wake failed: {e}")
            time.sleep(5)
            continue
        sampler.phase = "download"
        headers = {"Range": f"bytes={have}-"} if have else {}
        conn = {"start_offset": have, "bytes": 0, "t0": time.time()}
        try:
            with cam.s.get(cam.base + path, headers=headers, stream=True, timeout=(5, 20)) as r:
                conn["status"] = r.status_code
                if have and r.status_code == 200:
                    log("Download: camera ignored Range, restarting from 0")
                    have, mode = 0, "wb"
                elif r.status_code in (200, 206):
                    mode = "ab" if have else "wb"
                else:
                    raise RuntimeError(f"HTTP {r.status_code}")
                with dest.open(mode) as f:
                    for chunk in r.iter_content(16384):
                        f.write(chunk)
                        conn["bytes"] += len(chunk)
                conn["end"] = "eof"
        except Exception as e:  # noqa: BLE001
            conn["end"] = f"{type(e).__name__}: {e}"[:200]
        conn["secs"] = round(time.time() - conn.pop("t0"), 2)
        conn["kbps"] = round(conn["bytes"] / 1024 / conn["secs"], 1) if conn["secs"] else None
        stats["connections"].append(conn)
        log(f"Download: conn #{len(stats['connections'])} offset={conn['start_offset']} "
            f"+{conn['bytes']} B in {conn['secs']}s ({conn['kbps']} KB/s) end={conn['end']}")
        no_progress = 0 if conn["bytes"] > 0 else no_progress + 1
        if conn["bytes"] == 0:
            time.sleep(3)
    total = time.time() - t_start
    got = dest.stat().st_size if dest.exists() else 0
    per_conn = [c["bytes"] for c in stats["connections"]]
    stats.update(
        bytes_downloaded=got,
        complete=bool(expected) and got >= expected,
        total_secs=round(total, 1),
        avg_kbps_overall=round(got / 1024 / total, 1) if total else None,
        connection_count=len(per_conn),
        avg_bytes_per_connection=round(sum(per_conn) / len(per_conn)) if per_conn else 0,
        verify=verify_media(dest, kind, expected),
    )
    return stats


def direct_wake_with_retries(args) -> dict:
    """Firmware-style fallback: connect straight to the camera's public address."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import ble_direct

    adapter = args.ble_adapter or "hci0"
    end = time.time() + args.direct_connect_seconds
    attempts, last_err = 0, None
    log(f"BLE: not seen in scan; direct connect to {CAMERA_BLE_MAC} on {adapter} "
        f"(retrying up to {args.direct_connect_seconds:.0f}s)")
    while time.time() < end:
        attempts += 1
        try:
            res = ble_direct.direct_wake(CAMERA_BLE_MAC, adapter, log=log)
            res["attempts"] = attempts
            return res
        except Exception as e:  # noqa: BLE001
            last_err = f"{type(e).__name__}: {e}"
            log(f"BLE direct: attempt {attempts} failed: {last_err}")
            time.sleep(1)
    return {"connected": False, "attempts": attempts, "last_error": last_err}


def wait_for_hotspot(args, result: dict, timeout: float) -> None:
    t = time.time()
    log(f"Wi-Fi: waiting for hotspot {CAMERA_SSID} on {args.cam_iface}")
    while time.time() - t < timeout:
        seen = scan_for_ssid(args.cam_iface, CAMERA_SSID)
        if seen:
            result["hotspot_seen_s"] = round(time.time() - t, 1)
            result["hotspot_scan"] = seen
            log(f"Wi-Fi: hotspot visible after {result['hotspot_seen_s']}s ({seen})")
            return
        time.sleep(2)
    log("Wi-Fi: hotspot not seen before timeout")


# --------------------------------------------------------------------------- main

def preflight(args) -> dict:
    pf: dict = {}
    for tool in ("nmcli", "iw", "ip", "ping"):
        if not shutil.which(tool):
            raise SystemExit(f"missing tool: {tool}")
    up = iface_state(args.uplink_iface)
    cam = iface_state(args.cam_iface)
    pf["uplink"] = up
    pf["uplink_gateway"] = uplink_gateway(args.uplink_iface)
    pf["cam_iface_state_before"] = cam
    log(f"Preflight: uplink {args.uplink_iface} -> {up.get('GENERAL.CONNECTION')} "
        f"{up.get('IP4.ADDRESS[1]', '')} gw {pf['uplink_gateway']}")
    if not up.get("GENERAL.STATE", "").startswith("100"):
        if not args.allow_no_uplink:
            raise SystemExit(f"uplink {args.uplink_iface} is not connected; refusing to run")
        log(f"Preflight: uplink {args.uplink_iface} not connected (allowed by --allow-no-uplink)")
    if "unmanaged" in cam.get("GENERAL.STATE", ""):
        raise SystemExit(f"{args.cam_iface} is unmanaged by NetworkManager")
    if run(["systemctl", "is-active", "--quiet", "mobile-scanner"]).returncode == 0:
        raise SystemExit("mobile-scanner.service is active and owns wlan1; refusing to run")
    if run(["nmcli", "-t", "-f", "NAME", "con", "show"]).stdout.split().count(CAMERA_NM_PROFILE) != 1:
        raise SystemExit(f"NetworkManager profile {CAMERA_NM_PROFILE!r} not found")
    pf["camera_route_dev_before"] = route_dev(CAMERA_HOST)
    if pf["camera_route_dev_before"] not in (None, args.cam_iface) and pf["camera_route_dev_before"] != "lo":
        log(f"Preflight: WARNING {CAMERA_HOST} currently routes via {pf['camera_route_dev_before']}")
    return pf


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cam-iface", default="wlan1")
    ap.add_argument("--uplink-iface", default="wlan0")
    ap.add_argument("--ble-adapter", help="BlueZ adapter for the wake, e.g. hci1 (default: system default)")
    ap.add_argument("--ble-scan-seconds", type=float, default=30, help="how long to look for the camera (default 30)")
    ap.add_argument("--skip-wake", action="store_true", help="skip BLE wake (hotspot already up)")
    ap.add_argument("--no-direct-connect", action="store_true",
                    help="do not fall back to a direct connect to the camera's public address")
    ap.add_argument("--direct-connect-seconds", type=float, default=120,
                    help="how long to keep retrying the direct connect (default 120)")
    ap.add_argument("--allow-no-uplink", action="store_true",
                    help="run even if the uplink interface is not connected (results stay on disk)")
    ap.add_argument("--force-11b", action="store_true", help="try forcing 802.11b TX rates on the camera iface")
    ap.add_argument("--list-only", action="store_true", help="do not download anything")
    ap.add_argument("--media-id", type=int, help="download this media id instead of picking a video")
    ap.add_argument("--pick", choices=("newest", "smallest", "largest"), default="newest",
                    help="which video to download (default newest)")
    ap.add_argument("--max-seconds", type=float, default=900, help="download time budget (default 900)")
    ap.add_argument("--max-rejoins", type=int, default=10)
    ap.add_argument("--rewake-on-loss", action="store_true", help="re-run BLE wake when the camera link drops")
    ap.add_argument("--keep-connected", action="store_true", help="leave wlan1 joined to the camera at the end")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parents[2] / "radio_test_runs"))
    ap.add_argument("--camera", choices=sorted(CAMERAS), default="e6_plus",
                    help="which camera to target (default e6_plus)")
    args = ap.parse_args()
    select_camera(args.camera)
    log(f"Camera: {args.camera} BLE {CAMERA_BLE_MAC} SSID {CAMERA_SSID} profile {CAMERA_NM_PROFILE}")

    run_dir = Path(args.out) / datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)
    report: dict = {"started": datetime.now().isoformat(timespec="seconds"), "args": vars(args)}
    log(f"Run directory: {run_dir}")

    sampler = None
    keepalive = None
    rc = 1
    try:
        report["preflight"] = preflight(args)
        sampler = Sampler(args.cam_iface, args.uplink_iface, report["preflight"]["uplink_gateway"])
        sampler.start()

        if not args.skip_wake and (seen := scan_for_ssid(args.cam_iface, CAMERA_SSID)):
            log(f"Wi-Fi: hotspot already up ({seen}); skipping BLE wake")
            report["hotspot_already_up"] = seen
            args.skip_wake = True
        if not args.skip_wake:
            sampler.phase = "ble_wake"
            report["ble"] = asyncio.run(ble_wake(CAMERA_BLE_MAC, args.ble_scan_seconds, args.cam_iface, 90,
                                                 args.ble_adapter))
            if not report["ble"]["found"]:
                if args.no_direct_connect:
                    raise RuntimeError("camera not found over BLE")
                report["ble_direct"] = direct_wake_with_retries(args)
                if not report["ble_direct"].get("connected"):
                    raise RuntimeError("camera not found over BLE and direct connect failed")
                wait_for_hotspot(args, report["ble_direct"], 90)

        if not ensure_camera_link(args, sampler):
            raise RuntimeError("could not join camera hotspot")
        time.sleep(1)
        report["camera_link"] = link_info(args.cam_iface)
        report["camera_ip"] = iface_state(args.cam_iface).get("IP4.ADDRESS[1]")
        report["routes"] = {"camera": route_dev(CAMERA_HOST), "internet": route_dev("1.1.1.1")}
        log(f"Wi-Fi: link {report['camera_link']}  ip {report['camera_ip']}")
        log(f"Routes: camera via {report['routes']['camera']}, default via {report['routes']['internet']}")
        if report["routes"]["internet"] == args.cam_iface:
            log("WARNING: default route moved to the camera interface")

        cam = Camera()
        sampler.phase = "http"
        for p in ("/cmd/standby/reset", "/cmd/info/1", "/cmd/info/2", "/cmd/info/3"):
            try:
                report.setdefault("camera_info", {})[p] = cam.get_json(p)
                log(f"HTTP: {p} -> {json.dumps(report['camera_info'][p])[:200]}")
            except Exception as e:  # noqa: BLE001
                report.setdefault("camera_info", {})[p] = {"error": str(e)}
                log(f"HTTP: {p} failed: {e}")

        keepalive = KeepAlive(cam)
        keepalive.start()

        t = time.time()
        items = cam.gallery()
        report["gallery"] = {"count": len(items), "photos": sum(1 for i in items if int(i["type"]) == 1),
                             "videos": sum(1 for i in items if int(i["type"]) == 2),
                             "secs": round(time.time() - t, 1)}
        (run_dir / "gallery.json").write_text(json.dumps(items, indent=1))
        log(f"HTTP: gallery {report['gallery']}")

        if not args.list_only:
            if args.media_id:
                pick = next((i for i in items if int(i["id"]) == args.media_id), None)
            else:
                vids = [i for i in items if int(i["type"]) == 2]
                key = {"newest": lambda i: -int(i["id"]), "smallest": lambda i: int(i.get("size", 0)),
                       "largest": lambda i: -int(i.get("size", 0))}[args.pick]
                pick = sorted(vids, key=key)[0] if vids else None
            if pick is None:
                log("Download: nothing to download (no matching item)")
            else:
                ext = "jpg" if int(pick["type"]) == 1 else "mp4"
                dest = run_dir / f"media_{pick['id']}.{ext}"
                log(f"Download: id={pick['id']} type={pick['type']} size={pick.get('size')} date={pick.get('date')}")
                report["download"] = {"item": pick, **download(cam, pick, dest, args, sampler)}
                d = report["download"]
                log(f"Download: {d['bytes_downloaded']}/{d['expected_bytes']} B in {d['total_secs']}s, "
                    f"{d['connection_count']} connection(s), avg {d['avg_bytes_per_connection']} B/conn, "
                    f"{d['avg_kbps_overall']} KB/s overall, verify={d['verify']}")
        rc = 0
    except SystemExit as e:
        log(f"ABORT: {e}")
        report["error"] = str(e)
        rc = 2
    except Exception as e:  # noqa: BLE001
        log(f"ERROR: {type(e).__name__}: {e}")
        report["error"] = f"{type(e).__name__}: {e}"
    finally:
        if keepalive:
            keepalive.stop_evt.set()
            report["http_keepalive"] = {"ok": keepalive.ok, "fail": keepalive.fail}
        if sampler:
            sampler.stop_evt.set()
            sampler.join(timeout=5)
            report["samples_summary"] = sampler.summary()
            (run_dir / "samples.json").write_text(json.dumps(sampler.samples, indent=1))
        if args.force_11b:
            run(priv(["iw", "dev", args.cam_iface, "set", "bitrates"]))
        if not args.keep_connected and "preflight" in report:
            run(priv(["nmcli", "con", "down", CAMERA_NM_PROFILE]), timeout=20)
            log(f"Wi-Fi: {CAMERA_NM_PROFILE} brought down")
        report["uplink_after"] = iface_state(args.uplink_iface)
        report["finished"] = datetime.now().isoformat(timespec="seconds")
        (run_dir / "report.json").write_text(json.dumps(report, indent=2, default=str))
        (run_dir / "run.log").write_text("\n".join(LOG_LINES) + "\n")
        if "samples_summary" in report:
            log(f"Summary: {report['samples_summary']}")
        log(f"Report: {run_dir / 'report.json'}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
