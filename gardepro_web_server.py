#!/usr/bin/env python3
"""GardePro local web server — browser UI for the ESP32 bridge."""
from __future__ import annotations

import logging
import tempfile
from functools import wraps
from pathlib import Path
from typing import Any

from flask import Flask, Response, after_this_request, jsonify, render_template, request, send_file

from gardepro_server_api import GardeProServerAPI, MediaItem
from gardepro_server_jobs import GardeProSessionJobs

app = Flask(__name__)
log = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

_tmp_dir = tempfile.mkdtemp(prefix="gardepro_web_")

SAFE_SETTINGS_KEYS = {
    "standby_timeout",
    "wifi",
    "date_format",
    "time_format",
    "temperature_format",
    "photo_or_video",
    "photo_quality",
    "video_quality",
    "video_length",
}

SETTINGS_SCHEMA = [
    {"key": "standby_timeout", "label": "Standby Timeout (sec)", "type": "integer", "editable": True},
    {"key": "wifi", "label": "WiFi Enable", "type": "boolean", "editable": True},
    {"key": "date_format", "label": "Date Format", "type": "string", "editable": True},
    {"key": "time_format", "label": "Time Format", "type": "select",
     "options": [{"value": 0, "label": "12h"}, {"value": 1, "label": "24h"}], "editable": True},
    {"key": "temperature_format", "label": "Temperature", "type": "select",
     "options": [{"value": 0, "label": "°F"}, {"value": 1, "label": "°C"}], "editable": True},
    {"key": "photo_or_video", "label": "Capture Mode", "type": "select",
     "options": [{"value": 0, "label": "Photo"}, {"value": 1, "label": "Video"}, {"value": 2, "label": "Both"}],
     "editable": True},
    {"key": "photo_quality", "label": "Photo Quality", "type": "integer", "editable": True},
    {"key": "video_quality", "label": "Video Quality", "type": "integer", "editable": True},
    {"key": "video_length", "label": "Video Length (sec)", "type": "integer", "editable": True},
]


def _make_api(**kwargs: Any) -> GardeProServerAPI:
    return GardeProServerAPI(**kwargs)


def _make_jobs(**kwargs: Any) -> GardeProSessionJobs:
    return GardeProSessionJobs(api=_make_api(**kwargs))


def ok(data: Any) -> Response:
    return jsonify({"ok": True, "data": data, "error": None})


def err(code: str, message: str, http_status: int = 500) -> tuple[Response, int]:
    return jsonify({"ok": False, "data": None, "error": {"code": code, "message": message}}), http_status


def catch_errors(fn):
    @wraps(fn)
    def wrapper(*args, **kwargs):
        try:
            return fn(*args, **kwargs)
        except TimeoutError as exc:
            log.warning("timeout in %s: %s", fn.__name__, exc)
            return err("camera_timeout", str(exc))
        except RuntimeError as exc:
            log.warning("error in %s: %s", fn.__name__, exc)
            return err("camera_action_failed", str(exc))
        except Exception as exc:
            log.exception("unhandled error in %s", fn.__name__)
            return err("internal_error", str(exc))
    return wrapper


def _normalize_status(raw: Any) -> dict[str, Any]:
    if not isinstance(raw, dict):
        return {
            "camera_reachable": False,
            "session_open": False,
            "live_view_active": False,
            "halow_connected": False,
            "wifi_connected": False,
            "camera_ip": None,
            "camera_target_ble_mac": None,
            "camera_target_ble_name": None,
            "camera_target_wifi_ssid": None,
            "bridge_ip": None,
            "ble_stage": None,
            "standby_requested": False,
            "tunnel_connected": False,
            "control_last_message": None,
            "control_error": None,
            "control_state": None,
            "control_progress": None,
            "control_progress_text": None,
            "raw": raw,
        }
    wifi = bool(raw.get("wifi_connected", False))
    return {
        "camera_reachable": wifi,
        "session_open": wifi,
        "live_view_active": bool(raw.get("stream_active", False)),
        "halow_connected": bool(raw.get("halow_connected", False)),
        "wifi_connected": wifi,
        "camera_ip": raw.get("camera_ip"),
        "camera_target_ble_mac": raw.get("camera_target_ble_mac"),
        "camera_target_ble_name": raw.get("camera_target_ble_name"),
        "camera_target_wifi_ssid": raw.get("camera_target_wifi_ssid"),
        "bridge_ip": raw.get("bridge_ip"),
        "ble_stage": raw.get("ble_stage"),
        "standby_requested": bool(raw.get("standby_requested", False)),
        "tunnel_connected": bool(raw.get("tunnel_connected", False)),
        "control_last_message": raw.get("control_last_message"),
        "control_error": raw.get("control_error"),
        "control_state": raw.get("control_state"),
        "control_progress": raw.get("control_progress"),
        "control_progress_text": raw.get("control_progress_text"),
        "raw": raw,
    }


def _normalize_media_item(item: dict[str, Any]) -> dict[str, Any]:
    media_id = item.get("id", 0)
    raw_type = item.get("type", 0)
    type_str = "photo" if raw_type == 1 else "video" if raw_type == 2 else "unknown"
    uid = item.get("uid", "")
    status = "pending" if raw_type == 2 and uid == "00000000" else "ready"
    return {
        "id": media_id,
        "type": type_str,
        "timestamp": item.get("date", ""),
        "size_bytes": item.get("size", 0),
        "status": status,
        "download_path": f"/api/media/{media_id}/download?type={type_str}",
        "thumbnail_path": f"/api/media/{media_id}/thumb?type={type_str}",
    }


# --- Page ---

@app.get("/")
def index():
    return render_template("index.html")


# --- Status / session ---

@app.get("/api/status")
@catch_errors
def get_status():
    raw = _make_api().status()
    return ok(_normalize_status(raw))


@app.post("/api/session/open")
@catch_errors
def session_open():
    body = request.get_json(silent=True) or {}
    timeout = body.get("timeout_sec")
    poll_interval = float(body.get("poll_interval_sec", 1.0))
    result = _make_api().open_session(timeout=timeout, poll_interval=poll_interval)
    return ok({
        "session_ready": result.get("session_ready", False),
        "status": _normalize_status(result.get("after", {})),
    })


@app.post("/api/session/close")
@catch_errors
def session_close():
    body = request.get_json(silent=True) or {}
    timeout = body.get("timeout_sec")
    poll_interval = float(body.get("poll_interval_sec", 1.0))
    standby = bool(body.get("standby", True))
    result = _make_api().close_session(timeout=timeout, poll_interval=poll_interval, standby=standby)
    return ok({
        "session_closed": result.get("session_closed", False),
        "standby_requested": result.get("standby_requested", False),
        "status": _normalize_status(result.get("after", {})),
    })


@app.get("/api/camera/target")
@catch_errors
def camera_target_get():
    return ok(_make_api().camera_target())


@app.post("/api/camera/target")
@catch_errors
def camera_target_post():
    body = request.get_json(silent=True) or {}
    result = _make_api().set_camera_target(
        profile_id=str(body.get("id", "")),
        ble_mac=str(body.get("ble_mac", "")),
        wifi_ssid=str(body.get("wifi_ssid", "")),
        ble_name=str(body.get("ble_name", "")),
    )
    return ok(result)


# --- Settings ---

@app.get("/api/settings")
@catch_errors
def get_settings():
    result = _make_jobs().fetch_setting_values()
    settings = result.get("result", {})
    if isinstance(settings, dict) and "data" in settings:
        settings = settings["data"]
    return ok({"settings": settings})


@app.get("/api/settings/schema")
def get_settings_schema():
    return ok({"fields": SETTINGS_SCHEMA})


@app.post("/api/settings")
@catch_errors
def update_settings():
    body = request.get_json(silent=True) or {}
    patch = body.get("patch", {})
    if not isinstance(patch, dict) or not patch:
        return err("invalid_request", "patch must be a non-empty object", 400)
    unsafe_keys = set(patch.keys()) - SAFE_SETTINGS_KEYS
    if unsafe_keys:
        return err("unsafe_action_blocked", f"unsafe settings keys: {', '.join(sorted(unsafe_keys))}", 400)

    def do_update(api: GardeProServerAPI) -> Any:
        return api.update_settings(patch)

    result = _make_api().run_in_session(do_update)
    return ok(result.get("result", result))


# --- Media ---

@app.get("/api/media")
@catch_errors
def list_media():
    result = _make_jobs().list_media()
    raw_items = result.get("result", [])
    if not isinstance(raw_items, list):
        raw_items = []
    return ok({"items": [_normalize_media_item(item) for item in raw_items]})


@app.get("/api/media/<int:media_id>/download")
@catch_errors
def download_media(media_id: int):
    type_str = request.args.get("type", "")
    raw_type = 1 if type_str == "photo" else 2 if type_str == "video" else None
    if raw_type is None:
        return err("invalid_request", "type must be 'photo' or 'video'", 400)

    item = MediaItem(id=media_id, type=raw_type)
    tmp = tempfile.NamedTemporaryFile(
        dir=_tmp_dir, suffix=f".{item.file_extension.lower()}", delete=False
    )
    dest = tmp.name
    tmp.close()

    api = _make_api()
    status, _ = api.download_media_to_path(item, dest, auto_bringup=True)

    dest_path = Path(dest)
    if not dest_path.exists() or not (200 <= status < 300):
        dest_path.unlink(missing_ok=True)
        return err("not_found", f"camera returned HTTP {status}", 404)

    mimetype = "image/jpeg" if item.file_extension == "JPG" else "video/mp4"
    download_name = f"gardepro_{media_id}.{item.file_extension.lower()}"

    @after_this_request
    def cleanup(response):
        dest_path.unlink(missing_ok=True)
        return response

    return send_file(dest, mimetype=mimetype, as_attachment=True, download_name=download_name)


@app.get("/api/media/<int:media_id>/thumb")
@catch_errors
def thumb_media(media_id: int):
    type_str = request.args.get("type", "")
    raw_type = 1 if type_str == "photo" else 2 if type_str == "video" else None
    if raw_type is None:
        return err("invalid_request", "type must be 'photo' or 'video'", 400)

    item = MediaItem(id=media_id, type=raw_type)
    tmp = tempfile.NamedTemporaryFile(dir=_tmp_dir, suffix=".jpg", delete=False)
    dest = tmp.name
    tmp.close()

    api = _make_api()
    status, _ = api.download_thumbnail_to_path(item, dest, auto_bringup=True)

    dest_path = Path(dest)
    if not dest_path.exists() or not (200 <= status < 300):
        dest_path.unlink(missing_ok=True)
        return err("not_found", f"camera returned HTTP {status}", 404)

    @after_this_request
    def cleanup(response):
        dest_path.unlink(missing_ok=True)
        return response

    return send_file(dest, mimetype="image/jpeg")


# --- Actions ---

@app.post("/api/actions/take-picture")
@catch_errors
def take_picture():
    def do_capture(api: GardeProServerAPI) -> Any:
        return api.take_picture_verified()

    result = _make_api().run_in_session(do_capture)
    data = result.get("result", {})
    return ok({
        "captured": data.get("captured", False),
        "media": _normalize_media_item(data["after"]) if data.get("after") else None,
        "poll_attempts": data.get("poll_attempts"),
        "elapsed_sec": data.get("elapsed_sec"),
        "timed_out": data.get("timed_out", False),
    })


@app.post("/api/actions/video/start")
@catch_errors
def video_start():
    # Open session then start recording; leave session open for recording duration.
    api = _make_api()
    api.open_session()
    result = api.start_video_recording()
    started = isinstance(result, dict) and result.get("code") == 0
    return ok({"started": started, "response": result})


@app.post("/api/actions/video/stop")
@catch_errors
def video_stop():
    def do_stop(api: GardeProServerAPI) -> Any:
        return api.stop_video_recording_verified()

    result = _make_api().run_in_session(do_stop)
    data = result.get("result", {})
    return ok({
        "stopped": data.get("stopped", False),
        "new_video_observed": data.get("new_video_observed", False),
        "media": _normalize_media_item(data["after"]) if data.get("after") else None,
        "elapsed_sec": data.get("elapsed_sec"),
        "timed_out": data.get("timed_out", False),
    })


# --- Live view ---

@app.post("/api/live/start")
@catch_errors
def live_start():
    api = _make_api()
    api.open_session()
    result = api.stream_start()
    normalized = _normalize_status(result if isinstance(result, dict) else {})
    return ok({
        "stream_active": normalized.get("live_view_active", False),
        "tunnel_connected": normalized.get("tunnel_connected", False),
    })


@app.post("/api/live/stop")
@catch_errors
def live_stop():
    api = _make_api()
    result = api.stream_stop()
    normalized = _normalize_status(result if isinstance(result, dict) else {})
    return ok({
        "stream_active": normalized.get("live_view_active", True),
        "stopped": not normalized.get("live_view_active", True),
    })


@app.get("/api/live/status")
@catch_errors
def live_status():
    raw = _make_api().status()
    normalized = _normalize_status(raw)
    return ok({
        "stream_active": normalized.get("live_view_active", False),
        "tunnel_connected": normalized.get("tunnel_connected", False),
        "session_open": normalized.get("session_open", False),
    })


# --- Admin ---

@app.get("/api/admin/clock")
@catch_errors
def get_clock():
    def do_get(api: GardeProServerAPI) -> Any:
        return api.get_info(4)

    result = _make_api().run_in_session(do_get)
    return ok({"clock": result.get("result")})


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="GardePro web server")
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()

    log.info("Starting on http://%s:%d", args.host, args.port)
    app.run(host=args.host, port=args.port, debug=args.debug)
