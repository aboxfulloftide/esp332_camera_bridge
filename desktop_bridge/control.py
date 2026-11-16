"""Control worker: bringup, stream_start, stream_stop, trigger_photo, and session leases.

Mirrors the firmware's control queue: one active action plus at most one
pending; state is reported by /control/summary and /control/status using the
firmware's field names.
"""

from __future__ import annotations

import queue
import threading
import time

import requests

from .radio import log, prt

ACTIONS = ("bringup", "stream_start", "stream_stop", "trigger_photo")
ETA_SEC = {"bringup": 120, "stream_start": 150, "stream_stop": 10, "trigger_photo": 150}
ACTION_STATE = {"bringup": "bringing_up", "stream_start": "stream_starting",
                "stream_stop": "going_standby", "trigger_photo": "triggering_photo"}
PROGRESS_TEXT = {
    "idle": "Idle", "queued": "Queued", "ble_wake": "Waking camera over Bluetooth",
    "hotspot_wait": "Waiting for camera hotspot", "wifi_join": "Joining camera Wi-Fi",
    "camera_http": "Checking camera HTTP", "camera_wifi_connected": "Camera Wi-Fi connected",
    "starting_rtsp": "Starting live view", "streaming": "Live view active", "stopping": "Stopping",
    "taking_photo": "Taking photo", "photo_result": "Waiting for photo", "standby": "Camera standby",
    "done": "Done", "failed": "Failed",
}

JOB_ALIASES = {"bringup": "bringup", "open_session": "bringup",
               "live_view_start": "stream_start", "stream_start": "stream_start",
               "live_view_stop": "stream_stop", "stream_stop": "stream_stop", "close_session": "stream_stop",
               "trigger_photo": "trigger_photo", "take_picture": "trigger_photo", "capture_photo": "trigger_photo"}


def _json_or_text(r):
    try:
        return r.json()
    except ValueError:
        return r.text[:500]


class Control:
    def __init__(self, cfg, radio, live, jobs_busy=lambda: None):
        self.cfg, self.radio, self.live = cfg, radio, live
        self.jobs_busy = jobs_busy          # returns a reason string while a durable job owns the camera
        self.q: queue.Queue[str] = queue.Queue()
        self.lock = threading.Lock()
        self.active: str | None = None
        self.pending: str | None = None
        self.active_since: float | None = None
        self.last_action: str | None = None
        self.last_ok = True
        self.last_error: str | None = None
        self.last_message: str | None = None
        self.last_finished: float | None = None
        self.progress = "idle"
        self.last_photo: dict | None = None
        self.lease_until: float | None = None
        self.lease_standby = True
        self.thread = threading.Thread(target=self._run, daemon=True, name="control")
        self.thread.start()
        threading.Thread(target=self._lease_loop, daemon=True, name="lease").start()

    # ------------------------------------------------------------------ queue
    def submit(self, action: str) -> tuple[bool, str | None]:
        if action not in ACTIONS:
            return False, "unsupported_action"
        busy = self.jobs_busy()
        if busy and action != "stream_stop":
            return False, busy
        with self.lock:
            if self.active == action or self.pending == action:
                return True, "already_queued"
            if self.pending is not None:
                return False, "control_busy"
            if self.active is None:
                self.active, self.active_since, self.progress = action, time.time(), "queued"
            else:
                self.pending = action
        self.q.put(action)
        return True, None

    def _run(self) -> None:
        while True:
            action = self.q.get()
            with self.lock:
                self.active = action
                if self.pending == action:
                    self.pending = None
                self.active_since = time.time()
            ok, err, msg = False, None, None
            try:
                ok, msg = getattr(self, f"_do_{action}")()
                if not ok:
                    err = msg
            except Exception as e:  # noqa: BLE001
                err = msg = f"{type(e).__name__}: {e}"
                log(f"Control: {action} crashed: {err}")
            with self.lock:
                self.last_action, self.last_ok = action, ok
                self.last_error = None if ok else (err or "failed")
                self.last_message = msg
                self.last_finished = time.time()
                self.progress = "done" if ok else "failed"
                if self.pending:
                    self.active, self.pending = self.pending, None
                    self.active_since = time.time()
                else:
                    self.active = None
            log(f"Control: {action} -> {'ok' if ok else 'FAILED'} ({msg})")

    def _set_progress(self, p: str) -> None:
        self.progress = p

    # ------------------------------------------------------------------ actions
    def _do_bringup(self) -> tuple[bool, str]:
        ok = self.radio.bringup(self._set_progress, owner="control")
        return (True, "bringup_complete") if ok else (False, "camera_bringup_failed")

    def _do_stream_start(self) -> tuple[bool, str]:
        if self.live.active:
            return True, "camera_stream_active"
        if not self.radio.bringup(self._set_progress, owner="live_view"):
            return False, "camera_bringup_failed"
        self.progress = "starting_rtsp"
        try:
            self.live.start()
        except Exception as e:  # noqa: BLE001
            return False, str(e) or "rtsp_failed"
        self.progress = "streaming"
        return True, self.live.last_message or "camera_stream_active"

    def _do_stream_stop(self) -> tuple[bool, str]:
        self.progress = "stopping"
        self.live.stop("stream_stop")
        if not self._lease_active():
            self.progress = "standby"
            self.radio.down(standby=True)
        return True, "stream_stopped"

    def _do_trigger_photo(self) -> tuple[bool, str]:
        if not self.radio.bringup(self._set_progress, owner="trigger_photo"):
            return False, "camera_bringup_failed"
        base = f"http://{prt.CAMERA_HOST}:{prt.CAMERA_PORT}"
        try:
            cam = prt.Camera()
            before = {int(i["id"]) for i in self._first_page(cam)}
            self.progress = "taking_photo"
            r = requests.get(base + "/media/pic/take", timeout=(5, 20))
            if not 200 <= r.status_code < 300:
                return False, "trigger_photo_take_failed"
            take = _json_or_text(r)
            self.progress = "photo_result"
            # The firmware stops at the first 2xx, but the E6 first answers
            # {"code":1,"desc":"on-going"}; keep polling until it reports something else.
            result = None
            for _ in range(20):
                try:
                    rr = requests.get(base + "/media/pic/result", timeout=(5, 10))
                    if 200 <= rr.status_code < 300:
                        result = _json_or_text(rr)
                        if not (isinstance(result, dict) and result.get("code") == 1):
                            break
                except requests.RequestException:
                    pass
                time.sleep(2)
            if result is None:
                return False, "trigger_photo_result_timeout"
            file_idx = 0
            data = result.get("data") if isinstance(result, dict) else None
            if isinstance(data, dict):
                try:
                    file_idx = int(data.get("fileIdx") or 0)
                except (TypeError, ValueError):
                    file_idx = 0
            for _ in range(5 if not file_idx else 0):   # fileIdx can be 0 on success: diff the gallery
                time.sleep(2)
                new = [i for i in self._first_page(cam)
                       if int(i["id"]) not in before and int(i.get("type", 0)) == 1]
                if new:
                    file_idx = max(int(i["id"]) for i in new)
                    break
            self.last_photo = {"take": take, "result": result, "media_id": file_idx or None,
                               "file_path": f"/file/{file_idx}/JPG" if file_idx else None, "at": time.time()}
            return True, "trigger_photo_complete"
        finally:
            if not self._lease_active() and not self.live.active:
                self.progress = "standby"
                self.radio.down(standby=True)

    @staticmethod
    def _first_page(cam) -> list[dict]:
        try:
            body = cam.get_json("/list/detail/backward/900000/60", timeout=20)
            return [i for i in (body.get("data") or []) if int(i.get("id", 0)) > 0]
        except (requests.RequestException, ValueError):
            return []

    # ------------------------------------------------------------------ leases
    def lease(self, ttl_s: float, standby_on_expire: bool = True) -> dict:
        self.lease_until = time.time() + ttl_s
        self.lease_standby = standby_on_expire
        return self.session_json()

    def release(self, standby: bool = True) -> dict:
        self.lease_until = None
        if standby and not self.live.active and self.active is None and not self.jobs_busy():
            self.radio.down(standby=True)
        return self.session_json()

    def _lease_active(self) -> bool:
        return self.lease_until is not None and time.time() < self.lease_until

    def _lease_loop(self) -> None:
        while True:
            time.sleep(5)
            if self.lease_until is not None and time.time() >= self.lease_until:
                self.lease_until = None
                if (self.lease_standby and not self.live.active and self.active is None
                        and not self.jobs_busy()):
                    log("Control: session lease expired; camera standby")
                    self.radio.down(standby=True)

    def session_json(self) -> dict:
        left = max(0, int((self.lease_until - time.time()) * 1000)) if self._lease_active() else 0
        return {"ok": True, "lease_active": self._lease_active(), "lease_remaining_ms": left,
                "standby_on_expire": self.lease_standby}

    # ------------------------------------------------------------------ status
    def state_name(self, wifi_connected: bool) -> str:
        if self.active:
            return ACTION_STATE.get(self.active, "busy")
        if self.live.active:
            return "streaming"
        return "connected" if wifi_connected else "idle"

    def status_json(self, wifi_connected: bool) -> dict:
        now = time.time()
        busy = self.active is not None
        return {
            "control_busy": busy,
            "control_state": self.state_name(wifi_connected),
            "control_action": self.active,
            "control_pending": self.pending,
            "control_last_action": self.last_action,
            "control_last_ok": self.last_ok,
            "control_error": self.last_error,
            "control_message": self.last_message,
            "control_progress": self.progress,
            "control_progress_text": PROGRESS_TEXT.get(self.progress, self.progress),
            "control_active_ms": int((now - self.active_since) * 1000) if busy and self.active_since else 0,
            "control_last_finished_ms": int((now - self.last_finished) * 1000) if self.last_finished else None,
            "eta_sec": ETA_SEC.get(self.active, 0) if busy else 0,
            "last_photo": self.last_photo,
        }

    def stall_callback(self) -> None:
        """Live view stalled: report it like the firmware (control_error stream_rtp_stalled)."""
        with self.lock:
            self.last_action, self.last_ok = "stream_start", False
            self.last_error = self.last_message = "stream_rtp_stalled"
            self.last_finished = time.time()
        if not self._lease_active():
            self.radio.down(standby=True)
