"""Durable download(-and-delete) jobs plus background upload of staged media.

The camera and upload work is tools/pi_trail_job.py (`camera_phase`,
`upload_phase`); this module adds the firmware's job lifecycle: one job at a
time, persisted across restarts, retry/backoff when the camera is unreachable,
and the /jobs + /trail-media JSON shapes.
"""

from __future__ import annotations

import json
import socket
import threading
import time
from datetime import datetime
from pathlib import Path
from urllib.parse import urlparse

from .radio import log, prt, ptj

ACTIVE = ("queued", "running", "waiting_retry")
DONE_STATUSES = ("deleted", "gone_before_delete")
DOWNLOADED_STATUSES = ("verified", "deleted", "gone_before_delete", "identity_mismatch", "delete_unconfirmed")


class Jobs:
    def __init__(self, cfg, radio, live_active=lambda: False, control_busy=lambda: False):
        self.cfg, self.radio = cfg, radio
        self.live_active, self.control_busy = live_active, control_busy
        self.staging = Path(cfg.staging_dir)
        self.staging.mkdir(parents=True, exist_ok=True)
        self.path = self.staging / "bridge_job.json"
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.stop_evt = threading.Event()
        self.progress: dict = {}
        self.upload = {"state": "idle", "uploaded": 0, "failed": 0, "bytes": 0, "last_sweep_at": None,
                       "last_error": None, "blocked_reason": None, "pending": 0}
        self.job = self._load()
        if self.job.get("status") == "running":      # service restarted mid-job: resume it
            self.job["status"] = "queued"
            self.job["stage"] = "resume_after_restart"
            self._save()
        self.thread = threading.Thread(target=self._run, daemon=True, name="jobs")
        self.thread.start()

    # ------------------------------------------------------------------ persistence
    def _load(self) -> dict:
        try:
            return json.loads(self.path.read_text())
        except (OSError, ValueError):
            return {"job_id": 0, "status": "idle", "action": "none", "stage": "", "last_error": ""}

    def _save(self) -> None:
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.job, indent=1))
        tmp.replace(self.path)

    # ------------------------------------------------------------------ API
    def busy_reason(self) -> str | None:
        return "durable_job_active" if self.job.get("status") in ACTIVE else None

    def submit(self, action: str) -> tuple[bool, str]:
        action = {"download_all": "download_all_media",
                  "download_and_delete": "download_and_delete_media"}.get(action, action)
        if action not in ("download_all_media", "download_and_delete_media"):
            return False, "unsupported_action"
        with self.lock:
            if self.job.get("status") in ACTIVE:
                return False, "durable_job_busy"
            if self.live_active():
                return False, "live_view_active"
            if self.control_busy():
                return False, "control_busy"
            job_id = int(self.job.get("job_id", 0)) + 1
            job_dir = self.staging / f"job_{datetime.now():%Y%m%d-%H%M%S}_{self.radio.camera}"
            self.job = {"job_id": job_id, "action": action, "status": "queued", "stage": "queued",
                        "camera": self.radio.camera, "job_dir": str(job_dir), "created": time.time(),
                        "retry_count": 0, "first_failure_at": None, "next_retry_at": None, "last_error": ""}
            self._save()
        self.wake.set()
        log(f"Jobs: accepted {action} #{job_id} for {self.radio.camera}")
        return True, "accepted"

    # ------------------------------------------------------------------ worker
    def _run(self) -> None:
        next_sweep = 0.0
        while not self.stop_evt.is_set():
            st = self.job.get("status")
            due = st == "queued" or (st == "waiting_retry" and time.time() >= (self.job.get("next_retry_at") or 0))
            if due:
                self._run_job()
                next_sweep = time.time() + 5
                continue
            if time.time() >= next_sweep and st not in ACTIVE:
                self._sweep_uploads()
                next_sweep = time.time() + self.cfg.upload_sweep_s
            self.wake.wait(5)
            self.wake.clear()

    def _run_job(self) -> None:
        job = self.job
        job_dir = Path(job["job_dir"])
        job_dir.mkdir(parents=True, exist_ok=True)
        if job.get("camera") and job["camera"] != self.radio.camera:
            self.radio.set_camera(job["camera"])
        job.update(status="running", stage="camera_bringup", started=job.get("started") or time.time())
        self._save()
        st = ptj.JobState(job_dir)
        st.data.setdefault("runs", []).append({"started": datetime.now().isoformat(timespec="seconds"),
                                               "bridge_job_id": job["job_id"], "action": job["action"]})
        st.save()
        args = self.radio.args(no_delete=job["action"] == "download_all_media",
                               wait_minutes=self.cfg.bringup_minutes)
        self.progress = {}
        summary, err = {}, None
        with self.radio.lock:
            self.radio.owner = "durable_job"
            try:
                summary = ptj.camera_phase(args, job_dir, st, self.progress, self.stop_evt)
            except Exception as e:  # noqa: BLE001
                err = f"{type(e).__name__}: {e}"
                log(f"Jobs: camera phase error: {err}")
            finally:
                self.radio.owner = None
        st.data["runs"][-1]["camera"] = summary
        st.save()
        self.progress["stage"] = "upload"
        job["stage"] = "upload"
        self._save()
        try:
            if self._server_reachable():
                st.data["runs"][-1]["upload"] = ptj.upload_phase(args, job_dir, st)
                st.save()
            else:
                self.upload["blocked_reason"] = "server_unreachable"
        except Exception as e:  # noqa: BLE001
            log(f"Jobs: upload error: {type(e).__name__}: {e}")
        try:
            with (job_dir / "job.log").open("a") as _f: _f.write("\n".join(list(prt.LOG_LINES)[-20000:]) + "\n")
        except OSError:
            pass

        unreachable = err is not None or summary.get("stopped") == "camera_unreachable"
        now = time.time()
        if unreachable and summary.get("stopped") != "cancelled":
            job["first_failure_at"] = job.get("first_failure_at") or now
            job["retry_count"] = int(job.get("retry_count", 0)) + 1
            job["last_error"] = err or "camera_unreachable"
            if now - job["first_failure_at"] > self.cfg.retry_give_up_s:
                job.update(status="failed", stage="camera_bringup_failed", finished=now)
                log(f"Jobs: #{job['job_id']} failed after {job['retry_count']} attempts")
            else:
                delay = (self.cfg.retry_quick_s if job["retry_count"] <= self.cfg.retry_quick_count
                         else self.cfg.retry_backoff_s)
                job.update(status="waiting_retry", next_retry_at=now + delay,
                           stage="camera_unavailable_backoff" if delay > self.cfg.retry_quick_s else "retry")
                log(f"Jobs: #{job['job_id']} camera unreachable; retry {job['retry_count']} in {delay}s")
        elif summary.get("stopped") == "cancelled":
            # Only the service shutting down sets stop_evt: resume the job on next start.
            job.update(status="queued", stage="interrupted")
        else:
            job.update(status="succeeded", stage="complete", finished=now, first_failure_at=None, last_error="")
            log(f"Jobs: #{job['job_id']} complete")
        self._save()

    def _server_reachable(self) -> bool:
        """Cheap TCP check (the server's /api/status calls back into this bridge)."""
        u = urlparse(self.cfg.server)
        try:
            socket.create_connection((u.hostname, u.port or (443 if u.scheme == "https" else 80)), timeout=5).close()
            return True
        except OSError:
            return False

    def _sweep_uploads(self) -> None:
        """Upload verified files from any job directory that have not been uploaded yet."""
        self.upload["last_sweep_at"] = time.time()
        pending_dirs, pending = [], 0
        for state_file in sorted(self.staging.glob("job_*/job_state.json")):
            try:
                data = json.loads(state_file.read_text())
            except (OSError, ValueError):
                continue
            items = data.get("items", {})
            n = sum(1 for v in items.values()
                    if v.get("status") in DOWNLOADED_STATUSES and not v.get("uploaded")
                    and (state_file.parent / v["file"]).exists())
            # upload_phase also sends unsent camera connection records (incl. failed attempts).
            unsent_conns = any(not c.get("sent") for c in data.get("camera_connections", []))
            if n or unsent_conns:
                pending += n
                pending_dirs.append(state_file.parent)
        self.upload["pending"] = pending
        if not pending_dirs:
            self.upload.update(state="idle", blocked_reason=None)
            return
        if not self._server_reachable():
            self.upload.update(state="waiting", blocked_reason="server_unreachable")
            return
        self.upload.update(state="uploading", blocked_reason=None)
        for d in pending_dirs:
            cam = next((c for c in prt.CAMERAS if d.name.endswith("_" + c)), self.radio.camera)
            args = self.radio.args(camera=cam)
            if self.job.get("status") in ("queued",) or self.stop_evt.is_set():
                break                                          # camera jobs have priority
            try:
                res = ptj.upload_phase(args, d, ptj.JobState(d))
                self.upload["uploaded"] += res["uploaded"]
                self.upload["failed"] += res["failed"]
                self.upload["bytes"] += res["bytes"]
            except Exception as e:  # noqa: BLE001
                self.upload["last_error"] = f"{type(e).__name__}: {e}"
        self.upload["state"] = "idle"

    # ------------------------------------------------------------------ status
    def _counts(self) -> dict:
        try:
            items = json.loads((Path(self.job["job_dir"]) / "job_state.json").read_text()).get("items", {})
        except (KeyError, OSError, ValueError):
            items = {}
        vals = list(items.values())
        return {
            "total": len(vals),
            "processed": sum(1 for v in vals if v.get("status") != "pending"),
            "downloaded": sum(1 for v in vals if v.get("status") in DOWNLOADED_STATUSES),
            "deleted": sum(1 for v in vals if v.get("status") in DONE_STATUSES),
            "skipped": sum(1 for v in vals if v.get("status") == "skipped"),
            "uploaded": sum(1 for v in vals if v.get("uploaded")),
            "bytes_downloaded": sum(int((v.get("download") or {}).get("bytes_downloaded") or 0)
                                    for v in vals if v.get("status") in DOWNLOADED_STATUSES),
        }

    def job_json(self) -> dict:
        j, p, now = self.job, self.progress, time.time()
        c = self._counts()
        item_bytes = 0
        if j.get("status") == "running" and p.get("current_file"):
            try:
                item_bytes = Path(p["current_file"]).stat().st_size
            except OSError:
                pass
        started = p.get("item_started")
        return {
            "job_id": j.get("job_id", 0), "state_source": "disk",
            "action": j.get("action", "none"), "status": j.get("status", "idle"),
            "stage": p.get("stage") if j.get("status") == "running" and p.get("stage") else j.get("stage", ""),
            "camera": j.get("camera"),
            "total": c["total"], "processed": c["processed"],
            "current_media_id": p.get("current_media_id", 0) if j.get("status") == "running" else 0,
            "downloaded": c["downloaded"], "skipped": c["skipped"], "deleted": c["deleted"],
            "uploaded": c["uploaded"],
            "retry_count": j.get("retry_count", 0), "bytes_downloaded": c["bytes_downloaded"],
            "item_bytes_received": item_bytes,
            "item_expected_bytes": p.get("item_expected_bytes", 0) if j.get("status") == "running" else 0,
            "item_elapsed_ms": int((now - started) * 1000) if started and j.get("status") == "running" else 0,
            "item_progress_age_ms": 0,
            "next_retry_in_s": max(0, int(j["next_retry_at"] - now)) if j.get("status") == "waiting_retry"
            and j.get("next_retry_at") else None,
            "last_error": j.get("last_error", ""),
            "battery_mv": None, "minimum_battery_mv": None, "recovery_battery_mv": None,
            "staging_directory": j.get("job_dir", str(self.staging)),
        }

    def staged_files(self) -> list[dict]:
        out = []
        for d in sorted(self.staging.glob("job_*")):
            for f in sorted(d.iterdir()):
                if f.suffix.lower() in (".jpg", ".mp4") and f.is_file():
                    out.append({"filename": f.name, "bytes": f.stat().st_size, "job": d.name,
                                "url": f"/trail-media/file/{d.name}/{f.name}"})
        return out

    def staged_path(self, rel: str) -> Path | None:
        p = (self.staging / rel).resolve()
        if self.staging.resolve() not in p.parents or not p.is_file():
            return None
        return p
