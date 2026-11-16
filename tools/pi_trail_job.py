#!/usr/bin/env python3
"""Durable download-and-delete job for the GardePro E6+, run on the Pi.

Phase 1 (camera, over wlan1):
  - wait for the E6+ hotspot (optionally trying a BLE direct wake), join it
  - build the full manifest by paginating the gallery; compare with /cmd/info/3
  - for each item, oldest first: download with Range resume, verify size and
    structure (JPEG SOI/EOI, MP4 ftyp/moov/mdat), re-check the item's identity
    on the camera (IDs are reused), delete it, confirm it is gone
  - skip an item after 3 attempts with no progress; skipped items stay on camera
Phase 2 (server, over wlan0/hoth):
  - upload every verified file (POST /api/board/media + PUT blob, sha256 checked)

All state lives in <job_dir>/job_state.json and is saved after every step, so a
job can be resumed with --resume <job_dir>.

Rules follow RASPBERRY_PI_PORT_PLAN.md ("Durable-job rules to port").
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace

import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pi_radio_test as prt  # noqa: E402
from pi_radio_test import log  # noqa: E402

DEFAULT_STAGING = Path(__file__).resolve().parents[2] / "trail_staging"
SERVER_DEFAULT = "http://192.168.1.42/trail_cam"
CAMERA_LABELS = {"e6_plus": "GardePro E6+", "e6_original": "GardePro E6"}


# --------------------------------------------------------------------------- state

class JobState:
    def __init__(self, job_dir: Path):
        self.path = job_dir / "job_state.json"
        self.data = json.loads(self.path.read_text()) if self.path.exists() else {"items": {}, "events": []}

    def save(self) -> None:
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data, indent=1, sort_keys=True))
        os.replace(tmp, self.path)

    @property
    def items(self) -> dict:
        return self.data["items"]

    def event(self, msg: str) -> None:
        self.data["events"].append({"t": datetime.now().isoformat(timespec="seconds"), "msg": msg})
        log(msg)
        self.save()


# --------------------------------------------------------------------------- camera connection records
# One record per camera_phase run (wake + join + session), kept in
# job_state.json["camera_connections"] and sent to POST /api/board/camera-connections
# at the start of upload_phase. Keys starting with "_" are local bookkeeping only.

def utc_iso(t: float | None = None) -> str:
    return datetime.fromtimestamp(time.time() if t is None else t, timezone.utc) \
        .isoformat(timespec="seconds").replace("+00:00", "Z")


def ms(seconds: float | None) -> int | None:
    return None if seconds is None else int(round(seconds * 1000))


def new_connection(args, job_dir: Path) -> dict:
    now = time.time()
    return {"camera": args.camera, "job_id": job_dir.name, "started_at": utc_iso(now), "_t0": now,
            "connected_at": None, "ended_at": None, "ok": False, "failure_stage": None,
            "failure_message": None, "wake_method": None, "wake_attempts": 0,
            "ble_rssi_dbm": None, "wifi_rssi_dbm": None, "link_losses": 0,
            "items_downloaded": 0, "bytes_downloaded": 0, "avg_kbps": None,
            "details": {"bridge": getattr(args, "bridge_name", "raspberry_pi"),
                        "ble_adapter": None if args.no_wake else args.ble_adapter, "rewakes": 0},
            "_dl_secs": 0.0}


def _initial(conn: dict | None) -> bool:
    """True while the first connection of this record is still being made."""
    return conn is not None and not conn.get("connected_at")


def finish_connection(conn: dict, samples: list[dict], error: str | None) -> None:
    now = time.time()
    conn["ended_at"] = utc_iso(now)
    sig = [s["cam_signal_dbm"] for s in samples if s.get("cam_signal_dbm") is not None]
    if sig:
        conn.update(wifi_rssi_min=min(sig), wifi_rssi_max=max(sig),
                    wifi_rssi_avg=round(sum(sig) / len(sig), 1))
    # Link losses: camera Wi-Fi going from connected to not connected after the first join.
    t_conn = conn.get("_t_connected")
    if t_conn:
        prev, losses = True, 0
        for s in samples:
            if s["t"] < t_conn:
                continue
            if prev and not s["cam_connected"]:
                losses += 1
            prev = s["cam_connected"]
        conn["link_losses"] = losses
    if conn["_dl_secs"] > 0:
        conn["avg_kbps"] = round(conn["bytes_downloaded"] * 8 / 1000 / conn["_dl_secs"], 1)
    if error:
        conn["failure_message"] = error[:255]
    if not conn["ok"]:
        if conn.get("_wifi_joined"):
            conn["failure_stage"] = "camera_http"
        elif conn.get("_hotspot_seen"):
            conn["failure_stage"] = "wifi_join"
        else:
            conn["failure_stage"] = conn.get("_ble_stage") or "hotspot_timeout"


def send_camera_connections(args, st: "JobState") -> dict:
    """POST unsent connection records. Safe to repeat: the server keys rows on
    (scanner_host, camera, started_at) and updates instead of duplicating."""
    pending = [c for c in st.data.get("camera_connections", []) if not c.get("sent")]
    if not pending:
        return {"sent": 0}
    rows = [{k: v for k, v in c.items() if not k.startswith("_") and k != "sent"} for c in pending]
    try:
        r = requests.post(f"{args.server}/api/board/camera-connections", timeout=20,
                          json={"scanner_host": args.scanner_host, "connections": rows},
                          headers={"Accept": "application/json"})
        r.raise_for_status()
        body = r.json()
        if not body.get("ok"):
            raise RuntimeError(f"rejected: {body}")
    except Exception as e:  # noqa: BLE001
        log(f"Camera connections: send failed ({type(e).__name__}: {e}); will retry next upload")
        return {"sent": 0, "error": f"{type(e).__name__}: {e}"}
    stamp = datetime.now().isoformat(timespec="seconds")
    for c in pending:
        c["sent"] = stamp
    st.save()
    log(f"Camera connections: sent {body.get('saved')} record(s)"
        + (f", server skipped {body['skipped']}" if body.get("skipped") else ""))
    return {"sent": body.get("saved", 0), "skipped": body.get("skipped", 0)}


def identity(item: dict) -> str:
    return f"{item['id']}|{item['type']}|{item.get('size', 0)}|{item.get('date', '')}|{item.get('uid', '')}"


def staged_name(item: dict) -> str:
    stamp = (item.get("date") or "").replace("-", "").replace(":", "").replace(" ", "_") or "nodate"
    ext = "jpg" if int(item["type"]) == 1 else "mp4"
    return f"E6P_{stamp}_{item['id']}.{ext}"


# --------------------------------------------------------------------------- camera helpers

def camera_retry(fn, args, sampler, what: str, attempts: int = 3):
    last = None
    for i in range(1, attempts + 1):
        try:
            return fn()
        except Exception as e:  # noqa: BLE001
            last = e
            log(f"{what}: attempt {i}/{attempts} failed: {type(e).__name__}: {e}")
            prt.ensure_camera_link(args, sampler)
            time.sleep(2 * i)
    raise last  # type: ignore[misc]


def camera_item_at(cam: prt.Camera, item_id: int) -> list[dict]:
    """Listing returns IDs strictly below the cursor, newest first, so cursor=id+1
    returns the item itself first if it is still on the camera."""
    body = cam.get_json(f"/list/detail/backward/{item_id + 1}/3", timeout=20)
    if "data" not in body:
        raise RuntimeError(f"unexpected listing: {str(body)[:120]}")
    return body.get("data") or []


def delete_item(cam: prt.Camera, item: dict) -> str:
    ext = "JPG" if int(item["type"]) == 1 else "mp4"
    for path in (f"/cmd/delete/{item['id']}/{ext}", f"/cmd/delete/{item['type']}/{item['id']}"):
        try:
            body = cam.get_json(path, timeout=20)
            if body.get("code") == 0:
                return path
        except requests.RequestException:
            pass
    raise RuntimeError("delete commands did not return code 0")


def wait_for_hotspot(args, sampler, minutes: float | None = None, force_wake: bool = False,
                     conn: dict | None = None) -> bool:
    """force_wake: skip the first scan check. NetworkManager's scan cache can
    still list the hotspot for a few minutes after the camera went to sleep.
    conn (optional): camera connection record; wake details are recorded on it
    until the first successful connection, after that only re-wakes are counted."""
    minutes = args.wait_minutes if minutes is None else minutes
    end = time.time() + minutes * 60
    tried_wake = 0
    initial = _initial(conn)
    log(f"Waiting up to {minutes} min for {prt.CAMERA_SSID} on {args.cam_iface}")
    while time.time() < end:
        skip_scan = force_wake and tried_wake == 0 and args.ble_adapter and not args.no_wake
        if not skip_scan and prt.scan_for_ssid(args.cam_iface, prt.CAMERA_SSID):
            if initial and conn["wake_method"] is None:
                conn["wake_method"] = "none"           # hotspot was already up
            return True
        if args.ble_adapter and not args.no_wake:
            tried_wake += 1
            if initial:
                conn["wake_attempts"] += 1
            elif conn is not None:
                conn["details"]["rewakes"] = conn["details"].get("rewakes", 0) + 1
            # bleak scan+connect first (what worked on the desktop), then the
            # kernel direct connect as a fallback.
            try:
                r = asyncio.run(prt.ble_wake(prt.CAMERA_BLE_MAC, min(args.ble_scan_seconds, max(10, end - time.time())),
                                             args.cam_iface, 90, args.ble_adapter))
                if initial:
                    if r.get("found"):
                        conn["ble_rssi_dbm"] = r.get("ble_rssi")
                        conn["ble_scan_ms"] = ms(r.get("scan_s"))
                        conn["_ble_stage"] = None
                    else:
                        conn["_ble_stage"] = "ble_not_found"
                    if r.get("connect_s") is not None:
                        conn["ble_connect_ms"] = ms(r["connect_s"])
                        conn["details"]["ble_ok_notify"] = r.get("ok_notify")
                if r.get("hotspot_seen_s") is not None:
                    if initial:
                        conn["wake_method"] = "bleak"
                        conn["details"]["hotspot_scan"] = r.get("hotspot_scan")
                    return True
            except Exception as e:  # noqa: BLE001
                log(f"BLE wake attempt {tried_wake} failed: {type(e).__name__}: {e}")
                if initial:
                    # ble_wake only raises after the scan found the camera (connect/GATT).
                    conn["_ble_stage"] = "ble_connect"
            if time.time() >= end:
                break
            try:
                import ble_direct
                r = ble_direct.direct_wake(prt.CAMERA_BLE_MAC, args.ble_adapter, log=log)
                log(f"BLE direct wake result: ok={r.get('ok_notify')}")
                if initial and r.get("ok_notify"):
                    conn["wake_method"] = "direct"     # no RSSI from the kernel L2CAP path
            except Exception as e:  # noqa: BLE001
                log(f"BLE direct wake attempt {tried_wake} failed: {type(e).__name__}")
        else:
            time.sleep(5)
    return False


def camera_reachable(args, sampler, minutes: float | None = None, conn: dict | None = None) -> bool:
    """Make sure the camera link is up; wake the camera if the join fails.
    False means the camera stayed unreachable for `minutes` (default --lost-minutes)."""
    minutes = args.lost_minutes if minutes is None else minutes
    if prt.ensure_camera_link(args, sampler):
        return True
    log(f"Camera link not up; waking/recovering for up to {minutes} min")
    end = time.time() + minutes * 60
    while time.time() < end:
        if (wait_for_hotspot(args, sampler, max(0.5, (end - time.time()) / 60), force_wake=True, conn=conn)
                and prt.ensure_camera_link(args, sampler)):
            return True
    return False


# --------------------------------------------------------------------------- phase 1

def camera_phase(args, job_dir: Path, st: JobState, progress: dict | None = None,
                 stop_event=None) -> dict:
    """progress (optional) is updated in place for status reporting; stop_event
    (optional threading.Event) ends the item loop early with summary['stopped']='cancelled'."""
    summary: dict = {}
    progress = progress if progress is not None else {}
    progress.update(stage="camera_bringup", current_media_id=0, current_file=None, item_expected_bytes=0)
    gw = prt.uplink_gateway(args.uplink_iface)
    sampler = prt.Sampler(args.cam_iface, args.uplink_iface, gw, interval=5)
    conn = new_connection(args, job_dir)
    st.data.setdefault("camera_connections", []).append(conn)
    st.save()
    sampler.start()
    keepalive = None
    phase_error = None
    try:
        t = time.time()
        hotspot = wait_for_hotspot(args, sampler, conn=conn)
        conn["hotspot_wait_ms"] = ms(time.time() - t)  # includes any BLE wake time
        conn["_hotspot_seen"] = hotspot
        t = time.time()
        if not (hotspot and camera_reachable(args, sampler, args.wait_minutes, conn=conn)):
            raise RuntimeError("could not wake/join the camera hotspot")
        conn["wifi_join_ms"] = ms(time.time() - t)
        conn["_wifi_joined"] = True
        link = prt.link_info(args.cam_iface)
        summary["link_at_join"] = link
        conn.update(wifi_rssi_dbm=link.get("signal_dbm"), wifi_freq_mhz=link.get("freq_mhz"),
                    wifi_tx_bitrate=link.get("tx_bitrate"), wifi_rx_bitrate=link.get("rx_bitrate"))
        log(f"Joined: {link}")
        cam = prt.Camera()
        progress["stage"] = "manifest"
        info3 = camera_retry(lambda: cam.get_json("/cmd/info/3"), args, sampler, "info/3")
        now = time.time()
        conn.update(ok=True, connected_at=utc_iso(now), total_connect_ms=ms(now - conn["_t0"]),
                    _t_connected=now)
        st.save()
        log(f"Camera connected: BLE {conn['ble_rssi_dbm']} dBm, Wi-Fi {conn['wifi_rssi_dbm']} dBm, "
            f"wake={conn['wake_method']}, {conn['total_connect_ms']} ms")
        summary["info3_before"] = info3.get("data")
        log(f"Camera storage before: {info3.get('data')}")
        keepalive = prt.KeepAlive(cam)
        keepalive.start()

        # Manifest
        sampler.phase = "manifest"
        items = camera_retry(cam.gallery, args, sampler, "gallery")
        (job_dir / f"manifest_{datetime.now():%H%M%S}.json").write_text(json.dumps(items, indent=1))
        d = info3.get("data") or {}
        counted = int(d.get("photo", 0)) + int(d.get("video", 0))
        summary["manifest_count"] = len(items)
        summary["camera_counted"] = counted
        st.event(f"Manifest: {len(items)} items listed; camera counters say {counted} "
                 f"(photo={d.get('photo')} video={d.get('video')})")
        if not items and counted:
            raise RuntimeError("empty gallery but camera counters are non-zero; refusing to treat as done")

        for it in items:
            key = identity(it)
            st.items.setdefault(key, {"item": it, "status": "pending", "attempts": 0,
                                      "file": staged_name(it)})
        st.save()

        todo = sorted((k for k, v in st.items.items() if v["status"] in ("pending", "downloaded", "verified")),
                      key=lambda k: int(st.items[k]["item"]["id"]))
        if args.limit:
            todo = todo[:args.limit]
        st.event(f"Camera phase: {len(todo)} item(s) to process"
                 + (f" (limited to {args.limit})" if args.limit else ""))

        dl_args = SimpleNamespace(max_seconds=args.item_seconds, max_rejoins=args.max_rejoins,
                                  force_11b=False, cam_iface=args.cam_iface,
                                  # Re-wake inside the download loop when a join fails, instead of
                                  # retrying joins for the whole --item-seconds budget.
                                  rewake_on_loss=bool(args.ble_adapter) and not args.no_wake,
                                  ble_scan_seconds=args.ble_scan_seconds, ble_adapter=args.ble_adapter)
        t_phase = time.time()
        for n, key in enumerate(todo, 1):
            rec = st.items[key]
            it = rec["item"]
            dest = job_dir / rec["file"]
            label = f"[{n}/{len(todo)}] id={it['id']} {'JPG' if int(it['type']) == 1 else 'mp4'} {it.get('size')}B"

            if stop_event is not None and stop_event.is_set():
                summary["stopped"] = "cancelled"
                st.event(f"{label}: stop requested; ending camera phase (state saved)")
                break
            progress.update(stage="transfer", current_media_id=int(it["id"]), current_file=str(dest),
                            item_expected_bytes=int(it.get("size") or 0), item_started=time.time())
            if not camera_reachable(args, sampler):
                summary["stopped"] = "camera_unreachable"
                st.event(f"{label}: camera unreachable for {args.lost_minutes} min; stopping camera phase "
                         f"(state saved, resume with --resume {job_dir})")
                break

            if rec["status"] == "pending" or rec["status"] == "downloaded":
                sampler.phase = "download"
                before = dest.stat().st_size if dest.exists() else 0
                res = prt.download(cam, it, dest, dl_args, sampler)
                conn["bytes_downloaded"] += sum(c["bytes"] for c in res["connections"])
                conn["_dl_secs"] += sum(c["secs"] for c in res["connections"])
                rec["attempts"] += 1
                rec["download"] = {k: res[k] for k in ("bytes_downloaded", "total_secs", "connection_count",
                                                       "avg_kbps_overall", "verify")}
                ver = res["verify"]
                if ver["size_ok"] and ver["structure_ok"]:
                    rec["status"] = "verified"
                    rec["sha256"] = sha256(dest)
                    conn["items_downloaded"] += 1
                else:
                    progressed = res["bytes_downloaded"] > before
                    rec["no_progress"] = 0 if progressed else rec.get("no_progress", 0) + 1
                    if rec["no_progress"] >= 3 or rec["attempts"] >= 6:
                        rec["status"] = "skipped"
                        st.event(f"{label}: SKIPPED after {rec['attempts']} attempts (verify={ver}); left on camera")
                    else:
                        rec["status"] = "downloaded" if dest.exists() else "pending"
                        st.event(f"{label}: not verified yet (verify={ver}); will retry")
                        todo.append(key)
                    st.save()
                    continue
                st.save()

            if rec["status"] == "verified" and not args.no_delete:
                sampler.phase = "delete"
                try:
                    current = camera_retry(lambda: camera_item_at(cam, int(it["id"])), args, sampler,
                                           f"{label} identity check")
                    match = [c for c in current if int(c.get("id", 0)) == int(it["id"])
                             and int(c.get("type", 0)) == int(it["type"])]
                    if not match:
                        rec["status"] = "gone_before_delete"
                        st.event(f"{label}: already absent from camera before delete")
                    elif identity(match[0]) != key:
                        rec["status"] = "identity_mismatch"
                        st.event(f"{label}: camera item changed ({identity(match[0])}); NOT deleting")
                    else:
                        used = camera_retry(lambda: delete_item(cam, it), args, sampler, f"{label} delete")
                        after = camera_retry(lambda: camera_item_at(cam, int(it["id"])), args, sampler,
                                             f"{label} delete confirm")
                        still = any(int(c.get("id", 0)) == int(it["id"]) and int(c.get("type", 0)) == int(it["type"])
                                    for c in after)
                        if still:
                            rec["status"] = "delete_unconfirmed"
                            st.event(f"{label}: delete via {used} returned ok but item still listed")
                        else:
                            rec["status"] = "deleted"
                            rec["deleted_at"] = datetime.now().isoformat(timespec="seconds")
                except Exception as e:  # noqa: BLE001
                    rec["status"] = "verified"
                    rec["delete_error"] = f"{type(e).__name__}: {e}"
                    st.event(f"{label}: delete failed ({rec['delete_error']}); copy kept, will retry next run")
                st.save()
            done = sum(1 for v in st.items.values() if v["status"] in ("deleted", "gone_before_delete"))
            if n % 10 == 0 or n == len(todo):
                el = time.time() - t_phase
                log(f"Progress: {n}/{len(todo)} processed, {done} deleted total, {el:.0f}s elapsed, "
                    f"signal {prt.link_info(args.cam_iface).get('signal_dbm')} dBm")

        progress.update(stage="verify_camera", current_file=None)
        if summary.get("stopped") not in ("camera_unreachable", "cancelled"):
            info3_after = camera_retry(lambda: cam.get_json("/cmd/info/3"), args, sampler, "info/3 after")
            summary["info3_after"] = info3_after.get("data")
            remaining = camera_retry(cam.gallery, args, sampler, "gallery after")
            summary["gallery_after_count"] = len(remaining)
            (job_dir / "remaining_on_camera.json").write_text(json.dumps(remaining, indent=1))
            st.event(f"Camera after: storage {info3_after.get('data')}; {len(remaining)} item(s) still listed")
    except Exception as e:  # noqa: BLE001
        phase_error = f"{type(e).__name__}: {e}"
        raise
    finally:
        if keepalive:
            keepalive.stop_evt.set()
        sampler.stop_evt.set()
        sampler.join(timeout=10)
        summary["camera_link_samples"] = sampler.summary()
        if summary.get("stopped"):
            conn["details"]["stopped"] = summary["stopped"]
        finish_connection(conn, sampler.samples, phase_error)
        summary["camera_connection"] = {k: v for k, v in conn.items() if not k.startswith("_")}
        st.save()
        (job_dir / "camera_samples.json").write_text(json.dumps(sampler.samples, indent=1))
        if getattr(args, "standby_after", False):
            try:  # let the camera power its Wi-Fi down (firmware does this after jobs)
                requests.get(f"http://{prt.CAMERA_HOST}:{prt.CAMERA_PORT}/cmd/standby/now", timeout=(3, 5))
            except requests.RequestException:
                pass
        prt.run(prt.priv(["nmcli", "con", "down", prt.CAMERA_NM_PROFILE]), timeout=20)
        progress.update(stage="camera_done", current_file=None)
        log("Camera Wi-Fi brought down")
    return summary


# --------------------------------------------------------------------------- phase 2

def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def upload_phase(args, job_dir: Path, st: JobState) -> dict:
    conn_res = send_camera_connections(args, st)
    todo = [k for k, v in st.items.items()
            if v["status"] in ("deleted", "gone_before_delete", "verified", "identity_mismatch",
                               "delete_unconfirmed") and not v.get("uploaded")
            and (job_dir / v["file"]).exists()]
    st.event(f"Upload phase: {len(todo)} file(s) to {args.server}")
    ok = failed = 0
    up_bytes = 0
    t0 = time.time()
    for n, key in enumerate(sorted(todo, key=lambda k: int(st.items[k]["item"]["id"])), 1):
        rec = st.items[key]
        it = rec["item"]
        path = job_dir / rec["file"]
        size = path.stat().st_size
        digest = rec.get("sha256") or sha256(path)
        ctype = "image/jpeg" if int(it["type"]) == 1 else "video/mp4"
        meta = {
            "scanner_host": args.scanner_host,
            "source": "trail_camera",
            "media_type": ctype,
            "filename": rec["file"],
            "recorded_at": (it.get("date") or "").replace(" ", "T") or None,
            "bytes": size,
            "sha256": digest,
            "metadata": {"trail_media_id": int(it["id"]), "camera": CAMERA_LABELS[args.camera],
                         "camera_profile": args.camera,
                         "camera_uid": it.get("uid"),
                         "bridge": getattr(args, "bridge_name", "raspberry_pi")},
        }
        last = None
        for attempt in range(1, 6):
            try:
                r = requests.post(f"{args.server}/api/board/media", json=meta, timeout=45,
                                  headers={"Accept": "application/json"})
                r.raise_for_status()
                acc = r.json()
                if not acc.get("ok"):
                    raise RuntimeError(f"metadata rejected: {acc}")
                if acc.get("upload_url"):
                    with path.open("rb") as f:
                        r2 = requests.put(requests.compat.urljoin(args.server + "/", acc["upload_url"]), data=f,
                                          timeout=(10, 300),
                                          headers={"Content-Type": ctype, "Content-Length": str(size),
                                                   "X-SHA256": digest, "Accept": "application/json"})
                    r2.raise_for_status()
                    res = r2.json()
                    if not res.get("ok") or int(res.get("bytes", -1)) != size:
                        raise RuntimeError(f"blob verification failed: {res}")
                rec["uploaded"] = {"media_id": acc.get("media_id"),
                                   "at": datetime.now().isoformat(timespec="seconds")}
                ok += 1
                up_bytes += size
                break
            except Exception as e:  # noqa: BLE001
                last = f"{type(e).__name__}: {e}"
                time.sleep(min(30, 2 ** attempt))
        else:
            failed += 1
            rec["upload_error"] = last
            log(f"Upload {rec['file']} FAILED: {last}")
        st.save()
        if n % 25 == 0 or n == len(todo):
            log(f"Upload progress: {n}/{len(todo)} ({ok} ok, {failed} failed, "
                f"{up_bytes / 1e6:.1f} MB, {time.time() - t0:.0f}s)")
    return {"uploaded": ok, "failed": failed, "bytes": up_bytes, "secs": round(time.time() - t0, 1),
            "camera_connections_sent": conn_res.get("sent", 0)}


# --------------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--resume", type=Path, help="existing job directory to continue")
    ap.add_argument("--staging", type=Path, default=DEFAULT_STAGING)
    ap.add_argument("--cam-iface", default="wlan1")
    ap.add_argument("--uplink-iface", default="wlan0")
    ap.add_argument("--ble-adapter", default="hci1", help="adapter for direct-wake attempts while waiting")
    ap.add_argument("--no-wake", action="store_true", help="only wait for the hotspot, never try BLE")
    ap.add_argument("--wait-minutes", type=float, default=30)
    ap.add_argument("--lost-minutes", type=float, default=5,
                    help="stop the camera phase (and upload) if the camera is unreachable this long mid-job")
    ap.add_argument("--ble-scan-seconds", type=float, default=120)
    ap.add_argument("--limit", type=int, default=0, help="process only the first N items (oldest first)")
    ap.add_argument("--no-delete", action="store_true", help="download and verify but never delete")
    ap.add_argument("--item-seconds", type=float, default=300, help="per-item download time budget")
    ap.add_argument("--max-rejoins", type=int, default=10)
    ap.add_argument("--skip-camera", action="store_true", help="upload phase only")
    ap.add_argument("--no-upload", action="store_true", help="camera phase only")
    ap.add_argument("--server", default=SERVER_DEFAULT)
    ap.add_argument("--scanner-host", default="trail_pi")
    ap.add_argument("--camera", choices=sorted(prt.CAMERAS), default="e6_plus",
                    help="which camera to target (default e6_plus)")
    args = ap.parse_args()
    prt.select_camera(args.camera)
    log(f"Camera: {args.camera} BLE {prt.CAMERA_BLE_MAC} SSID {prt.CAMERA_SSID} profile {prt.CAMERA_NM_PROFILE}")
    args.force_11b = False  # used by pi_radio_test.ensure_camera_link

    job_dir = args.resume or (args.staging / datetime.now().strftime("job_%Y%m%d-%H%M%S"))
    job_dir.mkdir(parents=True, exist_ok=True)
    st = JobState(job_dir)
    st.data.setdefault("runs", []).append({"started": datetime.now().isoformat(timespec="seconds"),
                                           "args": {k: str(v) for k, v in vars(args).items()}})
    st.save()
    log(f"Job directory: {job_dir}")
    rc = 0
    if not args.skip_camera:
        try:
            st.data["runs"][-1]["camera"] = camera_phase(args, job_dir, st)
        except Exception as e:  # noqa: BLE001
            # Upload whatever was staged even if the camera phase failed.
            st.event(f"CAMERA PHASE ERROR: {type(e).__name__}: {e}")
            rc = 1
        st.save()
    try:
        if not args.no_upload:
            for _ in range(12):
                if prt.run(["ping", "-c1", "-W2", "-I", args.uplink_iface,
                            requests.utils.urlparse(args.server).hostname], timeout=5).returncode == 0:
                    break
                log("Uplink to server not reachable yet; waiting")
                time.sleep(5)
            st.data["runs"][-1]["upload"] = upload_phase(args, job_dir, st)
            st.save()
    except Exception as e:  # noqa: BLE001
        st.event(f"JOB ERROR: {type(e).__name__}: {e}")
        rc = 1
    counts: dict = {}
    for v in st.items.values():
        counts[v["status"]] = counts.get(v["status"], 0) + 1
    up = sum(1 for v in st.items.values() if v.get("uploaded"))
    st.data["runs"][-1]["finished"] = datetime.now().isoformat(timespec="seconds")
    st.data["runs"][-1]["status_counts"] = counts
    st.save()
    with (job_dir / "job.log").open("a") as _f: _f.write("\n".join(prt.LOG_LINES) + "\n")
    log(f"Item status: {counts}; uploaded: {up}")
    log(f"State: {st.path}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
