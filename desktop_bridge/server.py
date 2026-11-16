"""Board HTTP API on port 18080: the subset of the ESP32 firmware API the server uses
for camera control, durable jobs, staged media and live view.
Field names follow gardepro_dual_radio_bridge/BOARD_HTTP_API.md.
"""

from __future__ import annotations

import json
import shutil
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import requests

from . import VERSION
from .control import ETA_SEC, JOB_ALIASES
from .radio import log, prt

CAMERA_BASE = f"http://{prt.CAMERA_HOST}:{prt.CAMERA_PORT}"


class BridgeHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, bridge):
        self.bridge = bridge
        super().__init__(addr, Handler)


class Handler(BaseHTTPRequestHandler):
    server_version = "gardepro-desktop-bridge"
    protocol_version = "HTTP/1.1"

    # ------------------------------------------------------------------ plumbing
    def log_message(self, fmt, *args):  # keep the journal quiet for status polling
        if not self.path.startswith(("/status", "/healthz", "/control/summary", "/trail-media/status",
                                     "/stream/status", "/jobs")):
            log("HTTP " + (fmt % args))

    @property
    def b(self):
        return self.server.bridge

    def _params(self) -> tuple[str, dict, bytes]:
        u = urllib.parse.urlparse(self.path)
        q = {k: v[-1] for k, v in urllib.parse.parse_qs(u.query, keep_blank_values=True).items()}
        body = b""
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            body = self.rfile.read(n)
            if "application/x-www-form-urlencoded" in (self.headers.get("Content-Type") or ""):
                for k, v in urllib.parse.parse_qs(body.decode(errors="replace")).items():
                    q.setdefault(k, v[-1])
            elif body.strip().startswith(b"{"):
                try:
                    for k, v in json.loads(body).items():
                        if isinstance(v, (str, int, float, bool)):
                            q.setdefault(k, str(v))
                except ValueError:
                    pass
        return u.path.rstrip("/") or "/", q, body

    def _send(self, code: int, payload, ctype: str = "application/json") -> None:
        data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def do_DELETE(self):
        self._dispatch("DELETE")

    def _dispatch(self, method: str) -> None:
        path, q, body = self._params()
        self.b.http_requests += 1
        try:
            route = ROUTES.get((method, path))
            if route is None:
                for (m, prefix), fn in PREFIX_ROUTES.items():
                    if m == method and path.startswith(prefix):
                        route = fn
                        break
            if route is None:
                self._send(404, {"ok": False, "error": "not_found", "path": path})
                return
            route(self, path, q, body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:  # noqa: BLE001
            log(f"HTTP {method} {path} error: {type(e).__name__}: {e}")
            try:
                self._send(500, {"ok": False, "error": "internal_error", "detail": f"{type(e).__name__}: {e}"})
            except OSError:
                pass

    # ------------------------------------------------------------------ status
    def r_healthz(self, path, q, body):
        b = self.b
        self._send(200, {"ok": True, "uptime_ms": b.uptime_ms(), "hostname": b.cfg.hostname,
                         "halow_connected": True, "halow_ip": b.uplink_ip(),
                         "http_service": {"server": "ThreadingHTTPServer", "requests": b.http_requests}})

    def r_status(self, path, q, body):
        b = self.b
        wifi = b.radio.wifi_status()
        mac, ssid, _ = prt.CAMERAS[b.radio.camera]
        self._send(200, {
            "ok": True, "uptime_ms": b.uptime_ms(), "hostname": b.cfg.hostname, "firmware_version": VERSION,
            "bridge": "desktop", "boot_session_id": b.boot_id,
            "halow_connected": True, "halow_ip": b.uplink_ip(), "halow_rssi": None,
            "uplink_iface": b.cfg.uplink_iface,
            "wifi_connected": wifi["wifi_connected"], "camera_wifi_signal_dbm": wifi["signal_dbm"],
            "camera_target_id": b.radio.camera, "camera_target_wifi_ssid": ssid, "camera_target_ble_mac": mac.lower(),
            "clock_valid": True, "storage_type": "disk", "storage_ready": True, "sd_ready": True,
            "free_bytes": shutil.disk_usage(b.jobs.staging).free,
            "scanner_schedule_enabled": False, "onboard_captures": None,
            "control": b.control.status_json(wifi["wifi_connected"]),
            "durable_media_job": b.jobs.job_json(),
            "live_view_active": b.live.active,
            "register": b.registrar.status() if b.registrar else {"enabled": False},
        })

    def r_control_summary(self, path, q, body):
        b = self.b
        wifi = b.radio.wifi_status()
        live = b.live.status()
        out = {"ok": True, **b.control.status_json(wifi["wifi_connected"]),
               "wifi_connected": wifi["wifi_connected"], "wifi_ip": wifi["wifi_ip"],
               "halow_connected": True, "halow_ip": b.uplink_ip(),
               "stream_active": live["stream_active"], "live_view_active": live["stream_active"],
               "tunnel_connected": live["tunnel_connected"], "ble_stage": b.radio.ble["stage"],
               "durable_media_job": b.jobs.job_json()}
        self._send(200, out)

    def r_control_status(self, path, q, body):
        self._send(200, {"ok": True, **self.b.control.status_json(self.b.radio.wifi_status()["wifi_connected"])})

    def r_wifi_status(self, path, q, body):
        self._send(200, {"ok": True, **self.b.radio.wifi_status(), "scheduled_scans_enabled": False})

    def r_ble_status(self, path, q, body):
        b = self.b
        mac, ssid, _ = prt.CAMERAS[b.radio.camera]
        r = b.radio.ble
        self._send(200, {"ok": True, "adapter": b.cfg.ble_adapter, "camera_target_ble_mac": mac.lower(),
                         "camera_target_wifi_ssid": ssid, "ble_stage": r["stage"],
                         "ble_wake_attempted": r["wake_attempted"], "ble_wake_confirmed": r["wake_confirmed"],
                         "ble_last_seen_rssi": r["last_rssi"], "ble_best_seen_rssi": r["best_rssi"],
                         "ble_last_connect_error": r["last_error"], "ble_notify_count": r["notify_count"],
                         "ble_last_notify": r["last_notify"]})

    def r_timing_status(self, path, q, body):
        self._send(200, {"ok": True, **self.b.radio.timing})

    def r_stream_status(self, path, q, body):
        self._send(200, {"ok": True, **self.b.live.status()})

    def r_camera_status(self, path, q, body):
        b = self.b
        wifi = b.radio.wifi_status()
        tgt = b.radio.target_json()["active"]
        self._send(200, {"ok": True, "camera_ip": prt.CAMERA_HOST, "camera_target_ble_mac": tgt["ble_mac"],
                         "camera_target_ble_name": tgt["ble_name"], "camera_target_wifi_ssid": tgt["wifi_ssid"],
                         "camera_wifi_connected": wifi["wifi_connected"],
                         "camera_wifi_ever_connected": b.radio.camera_wifi_ever_connected,
                         "radio_owner": b.radio.owner, "session": b.control.session_json()})

    # ------------------------------------------------------------------ control
    def _queue_control(self, action: str):
        b = self.b
        ok, msg = b.control.submit(action)
        payload = {"ok": ok, "accepted": ok, "durable": False, "action": action, "message": msg,
                   "error": None if ok else msg, "eta_sec": ETA_SEC.get(action, 0),
                   "poll_path": "/control/summary",
                   "control": b.control.status_json(b.radio.wifi_status()["wifi_connected"])}
        self._send(202 if ok else 409, payload)

    def r_control_bringup(self, path, q, body):
        self._queue_control("bringup")

    def r_control_stream_start(self, path, q, body):
        self._queue_control("stream_start")

    def r_control_stream_stop(self, path, q, body):
        self._queue_control("stream_stop")

    def r_jobs_get(self, path, q, body):
        b = self.b
        self._send(200, {"ok": True, "durable": True,
                         "supported_actions": ["bringup", "live_view_start", "live_view_stop", "trigger_photo",
                                               "download_all_media", "download_and_delete_media"],
                         "durable_media_job": b.jobs.job_json(),
                         "control": b.control.status_json(b.radio.wifi_status()["wifi_connected"])})

    def r_jobs_post(self, path, q, body):
        action = (q.get("action") or "").strip().lower()
        if not action and body:
            text = body.decode(errors="replace").lower()
            for key, val in (("live_view_start", "live_view_start"), ("stream_start", "live_view_start"),
                             ("live_view_stop", "live_view_stop"), ("stream_stop", "live_view_stop"),
                             ("trigger_photo", "trigger_photo"), ("take_picture", "trigger_photo"),
                             ("download_and_delete", "download_and_delete_media"),
                             ("download_all", "download_all_media"), ("bringup", "bringup"),
                             ("open_session", "bringup")):
                if key in text:
                    action = val
                    break
        if action.startswith("download_"):
            ok, msg = self.b.jobs.submit(action)
            self._send(202 if ok else 409, {"ok": ok, "accepted": ok, "durable": True, "message": msg,
                                            "error": None if ok else msg, "job": self.b.jobs.job_json()})
            return
        mapped = JOB_ALIASES.get(action)
        if not mapped:
            self._send(400, {"ok": False, "error": "invalid_request", "detail": "unsupported job action"})
            return
        self._queue_control(mapped)

    def r_session_lease(self, path, q, body):
        ttl = self.b.cfg.session_default_lease_s
        if "ttl_ms" in q:
            ttl = int(q["ttl_ms"]) / 1000
        elif "seconds" in q:
            ttl = int(q["seconds"])
        standby = q.get("standby_on_expire", "1") != "0"
        self._send(200, self.b.control.lease(ttl, standby))

    def r_session_release(self, path, q, body):
        self._send(200, self.b.control.release(q.get("standby", "1") != "0"))

    def r_session_status(self, path, q, body):
        self._send(200, self.b.control.session_json())

    # ------------------------------------------------------------------ camera target
    def r_camera_target_get(self, path, q, body):
        self._send(200, {"ok": True, "target": self.b.radio.target_json()})

    def r_camera_target_post(self, path, q, body):
        b = self.b
        if b.jobs.busy_reason():
            self._send(409, {"ok": False, "error": "durable_job_active", "job_id": b.jobs.job.get("job_id")})
            return
        if b.live.active or b.control.active:
            self._send(409, {"ok": False, "error": "camera_busy"})
            return
        cam = q.get("id", "")
        if not cam and q.get("ble_mac"):
            cam = next((c for c, (m, _, _) in prt.CAMERAS.items() if m.lower() == q["ble_mac"].lower()), "")
        if cam not in prt.CAMERAS:
            self._send(400, {"ok": False, "error": "invalid_request",
                             "detail": f"unknown camera; known: {sorted(prt.CAMERAS)}",
                             "target": b.radio.target_json()})
            return
        b.radio.set_camera(cam)
        self._send(200, {"ok": True, "target": b.radio.target_json()})

    # ------------------------------------------------------------------ camera proxy
    def _camera_ready(self) -> bool:
        if not self.b.radio.wifi_status()["wifi_connected"]:
            self._send(503, {"error": "camera_wifi_down"})
            return False
        return True

    def _proxy(self, method: str, cam_path: str, body: bytes = b"", ctype: str | None = None) -> None:
        if not self._camera_ready():
            return
        try:
            r = requests.request(method, CAMERA_BASE + cam_path, data=body or None,
                                 headers={"Content-Type": ctype} if ctype else None, timeout=(5, 30))
        except requests.Timeout:
            self._send(504, {"error": "camera_timeout"})
            return
        except requests.RequestException:
            self._send(502, {"error": "camera_connect_failed"})
            return
        self._send(r.status_code, r.content, "application/json")

    def r_camera_request(self, path, q, body):
        cam_path = q.get("path", "")
        if not cam_path.startswith("/"):
            self._send(400, {"error": "missing_path"})
            return
        # Same as the firmware: method defaults to GET; the raw request body (or a
        # "body" argument) is forwarded unchanged (settings, clock updates).
        method = (q.get("method") or "GET").upper()
        req_body = body if body else q.get("body", "").encode()
        if cam_path.split("?")[0].rstrip("/") == "/cmd/standby/now":
            self._standby_now()
            return
        self._proxy(method, cam_path, req_body, q.get("content_type") or None)

    def _standby_now(self) -> None:
        """Firmware semantics for a proxied /cmd/standby/now.

        Deferred while a session lease is active (as the firmware does). The bridge
        already puts the camera in standby and drops its Wi-Fi after stream_stop and
        trigger_photo, so the server's follow-up standby call finds the link down:
        report that as done instead of 503. Otherwise standby the camera and drop
        the link, like the firmware's powerDownTrailCameraWifiRadio.
        """
        b = self.b
        if b.control._lease_active():
            self._send(200, {"code": 0, "desc": "standby_deferred_by_lease"})
            return
        if b.live.active or b.control.active or b.jobs.busy_reason():
            self._send(409, {"error": "camera_busy", "detail": "standby refused while the camera is in use"})
            return
        if not b.radio.wifi_status()["wifi_connected"]:
            self._send(200, {"code": 0, "desc": "already_standby"})
            return
        b.radio.down(standby=True)
        self._send(200, {"code": 0, "desc": "standby"})

    def r_camera_raw(self, path, q, body):
        cam_path = q.get("path", "")
        if not cam_path.startswith("/"):
            self._send(400, {"error": "missing_path"})
            return
        if not self._camera_ready():
            return
        try:
            r = requests.get(CAMERA_BASE + cam_path, stream=True, timeout=(5, 30))
        except requests.Timeout:
            self._send(504, {"error": "camera_timeout"})
            return
        except requests.RequestException:
            self._send(502, {"error": "camera_connect_failed"})
            return
        with r:
            ctype = r.headers.get("Content-Type", "application/octet-stream")
            length = r.headers.get("Content-Length")
            self.send_response(r.status_code)
            self.send_header("Content-Type", ctype)
            if length:   # requests de-chunks for us; stream with a known length when we have one
                self.send_header("Content-Length", length)
            else:
                self.send_header("Connection", "close")
                self.close_connection = True
            self.end_headers()
            for chunk in r.iter_content(65536):
                self.wfile.write(chunk)

    def r_camera_simple(self, path, q, body):
        mapping = {"/camera/gallery": "/list/detail/backward/900000/60", "/camera/getParaSetting": "/cmd/getParaSetting",
                   "/camera/standby/reset": "/cmd/standby/reset"}
        if path.startswith("/camera/info/"):
            n = path.rsplit("/", 1)[-1]
            if n not in {"1", "2", "3", "4", "5", "6"}:
                self._send(404, {"ok": False, "error": "not_found"})
                return
            cam_path = f"/cmd/info/{n}"
        else:
            cam_path = mapping[path]
        self._proxy("GET", cam_path)

    def r_camera_latest(self, path, q, body):
        if not self._camera_ready():
            return
        limit = max(1, min(20, int(q.get("limit", 6) or 6)))
        tfilter = int(q["type"]) if q.get("type") not in (None, "") else -1
        try:
            data = requests.get(f"{CAMERA_BASE}/list/detail/backward/900000/{limit}", timeout=(5, 20)).json()
        except (requests.RequestException, ValueError):
            self._send(502, {"error": "camera_connect_failed"})
            return
        items = [i for i in (data.get("data") or []) if tfilter < 0 or int(i.get("type", 0)) == tfilter]
        item = max(items, key=lambda i: int(i.get("id", 0))) if items else None
        self._send(200, {"ok": True, "limit": limit, "type_filter": tfilter, "data": item})

    # ------------------------------------------------------------------ staged media
    def r_trail_media_status(self, path, q, body):
        b = self.b
        self._send(200, {"ready": True, "job": b.jobs.job_json(), "free_bytes": shutil.disk_usage(b.jobs.staging).free,
                         "upload": b.jobs.upload})

    def r_trail_media_files(self, path, q, body):
        self._send(200, {"items": self.b.jobs.staged_files()})

    def r_trail_media_file(self, path, q, body):
        rel = urllib.parse.unquote(path[len("/trail-media/file/"):])
        p = self.b.jobs.staged_path(rel)
        if p is None:
            self._send(404, {"error": "not_found"})
            return
        ctype = "image/jpeg" if p.suffix.lower() == ".jpg" else "video/mp4"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(p.stat().st_size))
        self.end_headers()
        with p.open("rb") as f:
            shutil.copyfileobj(f, self.wfile, 65536)

    def r_upload_status(self, path, q, body):
        self._send(200, {"ok": True, "trail_media": self.b.jobs.upload,
                         "register": self.b.registrar.status() if self.b.registrar else {"enabled": False}})


R = Handler
ROUTES = {
    ("GET", "/healthz"): R.r_healthz,
    ("GET", "/status"): R.r_status,
    ("GET", "/control/summary"): R.r_control_summary,
    ("GET", "/control/status"): R.r_control_status,
    ("POST", "/control/bringup"): R.r_control_bringup,
    ("POST", "/control/stream_start"): R.r_control_stream_start,
    ("POST", "/control/stream_stop"): R.r_control_stream_stop,
    ("GET", "/jobs"): R.r_jobs_get,
    ("POST", "/jobs"): R.r_jobs_post,
    ("POST", "/session/lease"): R.r_session_lease,
    ("POST", "/session/release"): R.r_session_release,
    ("GET", "/session/status"): R.r_session_status,
    ("GET", "/wifi/status"): R.r_wifi_status,
    ("GET", "/ble/status"): R.r_ble_status,
    ("GET", "/timing/status"): R.r_timing_status,
    ("GET", "/stream/status"): R.r_stream_status,
    ("GET", "/camera/status"): R.r_camera_status,
    ("GET", "/camera/target"): R.r_camera_target_get,
    ("POST", "/camera/target"): R.r_camera_target_post,
    ("GET", "/camera/request"): R.r_camera_request,
    ("POST", "/camera/request"): R.r_camera_request,
    ("GET", "/camera/raw"): R.r_camera_raw,
    ("GET", "/camera/gallery"): R.r_camera_simple,
    ("GET", "/camera/getParaSetting"): R.r_camera_simple,
    ("GET", "/camera/standby/reset"): R.r_camera_simple,
    ("GET", "/camera/latest"): R.r_camera_latest,
    ("GET", "/trail-media/status"): R.r_trail_media_status,
    ("GET", "/trail-media/files"): R.r_trail_media_files,
    ("GET", "/upload/status"): R.r_upload_status,
}
PREFIX_ROUTES = {
    ("GET", "/camera/info/"): R.r_camera_simple,
    ("GET", "/trail-media/file/"): R.r_trail_media_file,
}
