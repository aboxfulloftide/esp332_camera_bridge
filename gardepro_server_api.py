#!/usr/bin/env python3
"""
Server-side client for the registered GardePro ESP32 bridge.
"""

from __future__ import annotations

import json
import socket
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Optional


DEFAULT_REGISTRATION_PATH = "/tmp/gardepro_board_registration.json"
DEFAULT_BOARD_PORT = 18080
DEFAULT_HTTP_TIMEOUT = 30
DEFAULT_CONTROL_TIMEOUT = 150
DEFAULT_PICTURE_TIMEOUT = 30


@dataclass
class BoardRegistration:
    halow_ip: str
    halow_mac: str
    halow_bssid: str = ""
    halow_rssi: int = 0
    halow_ssid: str = ""
    halow_gateway: str = ""
    peer: str = ""

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "BoardRegistration":
        return cls(
            halow_ip=str(data.get("halow_ip", "")),
            halow_mac=str(data.get("halow_mac", "")),
            halow_bssid=str(data.get("halow_bssid", "")),
            halow_rssi=int(data.get("halow_rssi", 0)),
            halow_ssid=str(data.get("halow_ssid", "")),
            halow_gateway=str(data.get("halow_gateway", "")),
            peer=str(data.get("peer", "")),
        )


@dataclass
class MediaItem:
    id: int
    type: int
    date: str = ""
    size: int = 0
    uid: str = ""

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "MediaItem":
        return cls(
            id=int(data.get("id", 0)),
            type=int(data.get("type", 0)),
            date=str(data.get("date", "")),
            size=int(data.get("size", 0)),
            uid=str(data.get("uid", "")),
        )

    @property
    def is_photo(self) -> bool:
        return self.type == 1

    @property
    def is_video(self) -> bool:
        return self.type == 2

    @property
    def file_extension(self) -> str:
        if self.is_photo:
            return "JPG"
        if self.is_video:
            return "mp4"
        raise ValueError(f"unsupported media type: {self.type}")

    @property
    def file_path(self) -> str:
        return f"/file/{self.id}/{self.file_extension}"

    @property
    def thumbnail_path(self) -> str:
        return f"/thumb/{self.id}/JPG"

    @property
    def delete_path(self) -> str:
        return f"/cmd/delete/{self.id}/{self.file_extension}"

    @property
    def delete_legacy_path(self) -> str:
        return f"/cmd/delete/{self.type}/{self.id}"

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "type": self.type,
            "date": self.date,
            "size": self.size,
            "uid": self.uid,
            "file_path": self.file_path,
            "thumbnail_path": self.thumbnail_path,
            "delete_path": self.delete_path,
            "delete_legacy_path": self.delete_legacy_path,
        }


class GardeProServerAPI:
    def __init__(
        self,
        host: str = "",
        port: int = DEFAULT_BOARD_PORT,
        registration_path: str = DEFAULT_REGISTRATION_PATH,
        http_timeout: int = DEFAULT_HTTP_TIMEOUT,
        control_timeout: int = DEFAULT_CONTROL_TIMEOUT,
        picture_timeout: int = DEFAULT_PICTURE_TIMEOUT,
    ) -> None:
        self.registration_path = registration_path
        self.port = port
        self.http_timeout = http_timeout
        self.control_timeout = control_timeout
        self.picture_timeout = picture_timeout
        self.host = host or self.registration().halow_ip
        if not self.host:
            raise ValueError("board host is empty")

    def registration(self) -> BoardRegistration:
        data = json.loads(Path(self.registration_path).read_text(encoding="utf-8"))
        return BoardRegistration.from_dict(data)

    def board_url(self, path: str) -> str:
        return f"http://{self.host}:{self.port}{path}"

    def _request(
        self,
        method: str,
        path: str,
        *,
        query: Optional[dict[str, str]] = None,
        body: bytes | None = None,
        content_type: str = "",
    ) -> tuple[int, dict[str, str], bytes]:
        url = self.board_url(path)
        if query:
            url += "?" + urllib.parse.urlencode(query)
        req = urllib.request.Request(url, method=method, data=body)
        if body is not None and content_type:
            req.add_header("Content-Type", content_type)
        try:
            with urllib.request.urlopen(req, timeout=self.http_timeout) as resp:
                return resp.status, dict(resp.headers.items()), resp.read()
        except urllib.error.HTTPError as exc:
            return exc.code, dict(exc.headers.items()), exc.read()
        except (urllib.error.URLError, ConnectionResetError, socket.timeout, OSError) as exc:
            message = str(exc).encode("utf-8", errors="replace")
            return 599, {"Content-Type": "text/plain"}, message

    def request_json(
        self,
        method: str,
        path: str,
        *,
        query: Optional[dict[str, str]] = None,
        body: bytes | None = None,
        content_type: str = "",
    ) -> tuple[int, dict[str, str], Any]:
        status, headers, response_body = self._request(
            method,
            path,
            query=query,
            body=body,
            content_type=content_type,
        )
        if not response_body:
            parsed: Any = {}
        else:
            text = response_body.decode("utf-8", errors="replace")
            try:
                parsed = json.loads(text)
            except json.JSONDecodeError:
                parsed = {"raw": text}
        return status, headers, parsed

    def status(self) -> Any:
        return self.request_json("GET", "/status")[2]

    def _wait_for_control_state(
        self,
        *,
        expect_action: str,
        predicate,
        timeout: Optional[int] = None,
        poll_interval: float = 1.0,
    ) -> Any:
        deadline = time.time() + float(timeout or self.control_timeout)
        last_status: Any = {}
        while time.time() < deadline:
            last_status = self.status()
            if predicate(last_status):
                return last_status
            if (
                isinstance(last_status, dict)
                and not last_status.get("control_busy", False)
                and last_status.get("control_pending") == "none"
                and last_status.get("control_last_action") == expect_action
                and not last_status.get("control_last_ok", False)
            ):
                raise RuntimeError(last_status.get("control_last_message", f"{expect_action} failed"))
            time.sleep(poll_interval)
        raise TimeoutError(f"timed out waiting for {expect_action}")

    def _poll_until(
        self,
        fetch: Callable[[], Any],
        *,
        predicate: Callable[[Any], bool],
        timeout: float,
        poll_interval: float,
    ) -> tuple[Any, int, float, bool]:
        started_at = time.time()
        deadline = started_at + timeout
        attempts = 0
        last_value: Any = None

        while time.time() < deadline:
            last_value = fetch()
            attempts += 1
            if predicate(last_value):
                return last_value, attempts, time.time() - started_at, False
            time.sleep(poll_interval)

        return last_value, attempts, time.time() - started_at, True

    def bringup(
        self,
        *,
        wait: bool = True,
        timeout: int | None = None,
        poll_interval: float = 1.0,
    ) -> Any:
        payload = self.request_json("POST", "/control/bringup")[2]
        if not wait:
            return payload
        return self._wait_for_control_state(
            expect_action="bringup",
            predicate=lambda status: isinstance(status, dict)
            and status.get("wifi_connected", False)
            and not status.get("control_busy", False),
            timeout=timeout,
            poll_interval=poll_interval,
        )

    def stream_start(
        self,
        *,
        wait: bool = True,
        timeout: int | None = None,
        poll_interval: float = 1.0,
    ) -> Any:
        payload = self.request_json("POST", "/control/stream_start")[2]
        if not wait:
            return payload
        return self._wait_for_control_state(
            expect_action="stream_start",
            predicate=lambda status: isinstance(status, dict)
            and status.get("stream_active", False)
            and status.get("tunnel_connected", False)
            and not status.get("control_busy", False),
            timeout=timeout,
            poll_interval=poll_interval,
        )

    def stream_stop(
        self,
        *,
        wait: bool = True,
        timeout: int | None = None,
        poll_interval: float = 1.0,
    ) -> Any:
        payload = self.request_json("POST", "/control/stream_stop")[2]
        if not wait:
            return payload
        return self._wait_for_control_state(
            expect_action="stream_stop",
            predicate=lambda status: isinstance(status, dict)
            and not status.get("stream_active", True)
            and not status.get("control_busy", False),
            timeout=timeout,
            poll_interval=poll_interval,
        )

    def _camera_endpoint_json(self, path: str) -> Any:
        status, _, payload = self.request_json("GET", path)
        if status == 200 and not self._is_camera_transport_error(payload):
            return payload
        self.bringup()
        status, _, payload = self.request_json("GET", path)
        return payload

    @staticmethod
    def _is_camera_transport_error(payload: Any) -> bool:
        if not isinstance(payload, dict):
            return False
        return payload.get("error") in {"camera_connect_failed", "camera_wifi_down", "camera_timeout"}

    @staticmethod
    def _normalize_file_path(file_path: str) -> str:
        if file_path.startswith("/file/"):
            return file_path
        normalized = file_path.lstrip("/")
        return f"/file/{normalized}"

    def normalize_file_path(self, file_path: str) -> str:
        return self._normalize_file_path(file_path)

    def media_file_path(self, media: MediaItem | int, *, media_type: int | None = None) -> str:
        return self.resolve_media(media, media_type=media_type).file_path

    def media_thumbnail_path(self, media: MediaItem | int, *, media_type: int | None = None) -> str:
        return self.resolve_media(media, media_type=media_type).thumbnail_path

    def media_delete_path(self, media: MediaItem | int, *, media_type: int | None = None) -> str:
        return self.resolve_media(media, media_type=media_type).delete_path

    def media_delete_legacy_path(self, media: MediaItem | int, *, media_type: int | None = None) -> str:
        return self.resolve_media(media, media_type=media_type).delete_legacy_path

    def camera_request_json(
        self,
        method: str,
        camera_path: str,
        *,
        auto_bringup: bool = True,
        body: bytes | None = None,
        content_type: str = "",
    ) -> Any:
        status, _, payload = self.request_json(
            "POST" if body is not None else "GET",
            "/camera/request",
            query={
                "method": method.upper(),
                "path": camera_path,
                **({"content_type": content_type} if content_type else {}),
            },
            body=body,
            content_type=content_type,
        )
        if status == 200 and not self._is_camera_transport_error(payload):
            return payload
        if not auto_bringup:
            return payload
        self.bringup()
        status, _, payload = self.request_json(
            "POST" if body is not None else "GET",
            "/camera/request",
            query={
                "method": method.upper(),
                "path": camera_path,
                **({"content_type": content_type} if content_type else {}),
            },
            body=body,
            content_type=content_type,
        )
        return payload

    def camera_request_json_body(
        self,
        method: str,
        camera_path: str,
        payload: Any,
        *,
        auto_bringup: bool = True,
    ) -> Any:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        return self.camera_request_json(
            method,
            camera_path,
            auto_bringup=auto_bringup,
            body=body,
            content_type="application/json",
        )

    @staticmethod
    def _decode_json_bytes(body: bytes) -> Any:
        if not body:
            return None
        try:
            return json.loads(body.decode("utf-8", errors="replace"))
        except json.JSONDecodeError:
            return None

    def _is_camera_raw_transport_error(self, status: int, body: bytes) -> bool:
        if status == 599:
            return True
        payload = self._decode_json_bytes(body)
        return self._is_camera_transport_error(payload)

    def camera_get(self, camera_path: str, *, auto_bringup: bool = True) -> tuple[int, dict[str, str], bytes]:
        status, headers, body = self._request("GET", "/camera/raw", query={"path": camera_path})
        if not auto_bringup or not self._is_camera_raw_transport_error(status, body):
            return status, headers, body
        self.bringup()
        return self._request("GET", "/camera/raw", query={"path": camera_path})

    def camera_get_result(self, camera_path: str, *, auto_bringup: bool = True) -> dict[str, Any]:
        status, headers, body = self.camera_get(camera_path, auto_bringup=auto_bringup)
        text = body.decode("utf-8", errors="replace")
        payload = self._decode_json_bytes(body)
        return {
            "camera_path": camera_path,
            "status": status,
            "headers": headers,
            "body": text,
            "json": payload,
        }

    def get_settings(self) -> Any:
        return self._camera_endpoint_json("/camera/getParaSetting")

    def get_setting_values(self) -> Any:
        return self.camera_request_json("GET", "/cmd/getSetting")

    def get_setting_data(self) -> dict[str, Any]:
        payload = self.get_setting_values()
        if not isinstance(payload, dict):
            raise ValueError("setting-values response was not a JSON object")
        data = payload.get("data")
        if not isinstance(data, dict):
            raise ValueError("setting-values response does not include an object at data")
        return data

    def get_setting_value(self, key: str) -> Any:
        data = self.get_setting_data()
        if key not in data:
            raise KeyError(key)
        return data[key]

    def list_setting_keys(self) -> list[str]:
        return sorted(self.get_setting_data().keys())

    def get_gallery(self) -> Any:
        return self._camera_endpoint_json("/camera/gallery")

    def list_media(self) -> list[MediaItem]:
        payload = self.get_gallery()
        if not isinstance(payload, dict):
            return []
        items = payload.get("data", [])
        if not isinstance(items, list):
            return []
        return [MediaItem.from_dict(item) for item in items if isinstance(item, dict)]

    def latest_media(self, *, media_type: int | None = None) -> MediaItem | None:
        for item in self.list_media():
            if media_type is None or item.type == media_type:
                return item
        return None

    def resolve_media(self, media: MediaItem | int, *, media_type: int | None = None) -> MediaItem:
        if isinstance(media, MediaItem):
            return media
        if media_type is None:
            raise ValueError("media_type is required when media is not a MediaItem")
        return MediaItem(id=int(media), type=int(media_type))

    def media_paths(self, media: MediaItem | int, *, media_type: int | None = None) -> dict[str, str | int]:
        item = self.resolve_media(media, media_type=media_type)
        payload: dict[str, str | int] = {
            "id": item.id,
            "type": item.type,
            "file_path": self.media_file_path(item),
            "thumbnail_path": self.media_thumbnail_path(item),
            "delete_path": self.media_delete_path(item),
            "delete_legacy_path": self.media_delete_legacy_path(item),
        }
        if item.date:
            payload["date"] = item.date
        if item.size:
            payload["size"] = item.size
        if item.uid:
            payload["uid"] = item.uid
        return payload

    def get_info(self, index: int) -> Any:
        return self._camera_endpoint_json(f"/camera/info/{index}")

    def set_setting_path(self, camera_path: str) -> Any:
        return self.camera_request_json("GET", camera_path)

    def set_setting_json(self, data: dict[str, Any]) -> Any:
        return self.camera_request_json_body("POST", "/cmd/setSetting", {"data": data})

    def update_settings(self, patch: dict[str, Any]) -> dict[str, Any]:
        before = self.get_setting_data()
        response = self.set_setting_json(patch)
        after = self.get_setting_data()
        changed: dict[str, dict[str, Any]] = {}
        unchanged: dict[str, Any] = {}
        for key, expected in patch.items():
            previous = before.get(key)
            current = after.get(key)
            if current == expected:
                changed[key] = {
                    "before": previous,
                    "after": current,
                }
            else:
                unchanged[key] = current
        return {
            "request": patch,
            "response": response,
            "changed": changed,
            "unchanged": unchanged,
            "before": {key: before.get(key) for key in patch},
            "after": {key: after.get(key) for key in patch},
        }

    def set_clock_path(self, camera_path: str) -> Any:
        return self.camera_request_json("GET", camera_path)

    def set_clock(self, timestamp_text: str, *, path: str = "/cmd/setGmtClock") -> Any:
        return self.camera_request_json_body("POST", path, {"data": timestamp_text})

    def delete_media_path(self, camera_path: str) -> Any:
        return self.camera_request_json("GET", camera_path)

    def delete_media(self, media: MediaItem | int, *, media_type: int | None = None, prefer_legacy: bool = False) -> Any:
        item = self.resolve_media(media, media_type=media_type)
        primary = (
            self.media_delete_legacy_path(item)
            if prefer_legacy
            else self.media_delete_path(item)
        )
        fallback = (
            self.media_delete_path(item)
            if prefer_legacy
            else self.media_delete_legacy_path(item)
        )
        payload = self.delete_media_path(primary)
        if isinstance(payload, dict) and payload.get("code") == 0:
            return payload
        return self.delete_media_path(fallback)

    def delete_media_verified(
        self,
        media: MediaItem | int,
        *,
        media_type: int | None = None,
        prefer_legacy: bool = False,
    ) -> dict[str, Any]:
        item = self.resolve_media(media, media_type=media_type)
        before_ids = {entry.id for entry in self.list_media()}
        response = self.delete_media(item, prefer_legacy=prefer_legacy)
        after_media = self.list_media()
        after_ids = {entry.id for entry in after_media}
        return {
            "media": item.to_dict(),
            "response": response,
            "present_before": item.id in before_ids,
            "present_after": item.id in after_ids,
            "deleted": item.id in before_ids and item.id not in after_ids,
            "remaining_count": len(after_media),
        }

    def format_sd_start(self) -> Any:
        return self.camera_request_json("GET", "/cmd/format/start")

    def format_sd_result(self) -> Any:
        return self.camera_request_json("GET", "/cmd/format/result")

    @staticmethod
    def _is_terminal_format_result(payload: Any) -> bool:
        if not isinstance(payload, dict):
            return False
        code = payload.get("code")
        if isinstance(code, int) and code == 0:
            desc = str(payload.get("desc", "")).strip().lower()
            status = str(payload.get("status", "")).strip().lower()
            if desc in {"ok", "success", "done", "complete", "completed", "finished"}:
                return True
            if status in {"ok", "success", "done", "complete", "completed", "finished", "idle"}:
                return True
            progress = payload.get("progress")
            if isinstance(progress, int) and progress >= 100:
                return True
        data = payload.get("data")
        if isinstance(data, dict):
            status = str(data.get("status", "")).strip().lower()
            if status in {"ok", "success", "done", "complete", "completed", "finished", "idle"}:
                return True
            progress = data.get("progress")
            if isinstance(progress, int) and progress >= 100:
                return True
        return False

    def format_sd_start_verified(
        self,
        *,
        timeout: int | None = None,
        poll_interval: float = 1.0,
    ) -> dict[str, Any]:
        before = self.format_sd_result()
        response = self.format_sd_start()
        last_result, attempts, elapsed_sec, timed_out = self._poll_until(
            self.format_sd_result,
            predicate=lambda result: self._is_terminal_format_result(result)
            or (result != before and not isinstance(result, dict)),
            timeout=float(timeout or self.control_timeout),
            poll_interval=poll_interval,
        )
        changed = last_result != before
        completed = self._is_terminal_format_result(last_result)

        return {
            "response": response,
            "before": before,
            "after": last_result,
            "changed": changed,
            "completed": completed,
            "poll_attempts": attempts,
            "elapsed_sec": round(elapsed_sec, 2),
            "timed_out": timed_out,
        }

    def standby_now(self) -> Any:
        return self.camera_request_json("GET", "/cmd/standby/now")

    def reboot_camera(self) -> Any:
        return self.camera_request_json("GET", "/cmd/reboot")

    def reset_camera_factory(self) -> Any:
        return self.camera_request_json("GET", "/cmd/resetFact")

    def firmware_upgrade_start(self) -> Any:
        return self.camera_request_json("GET", "/cmd/upgrade/start")

    def firmware_upgrade_result(self) -> Any:
        return self.camera_request_json("GET", "/cmd/upgrade/result")

    def take_picture(self) -> Any:
        return self.camera_request_json("GET", "/media/pic/take")

    def picture_result(self) -> Any:
        return self.camera_request_json("GET", "/media/pic/result")

    @staticmethod
    def _extract_picture_result_id(payload: Any) -> int | None:
        if not isinstance(payload, dict):
            return None
        for key in ("fileIdx", "file_id", "id"):
            value = payload.get(key)
            if isinstance(value, int):
                return value
            if isinstance(value, str) and value.isdigit():
                return int(value)
        data = payload.get("data")
        if isinstance(data, dict):
            for key in ("fileIdx", "file_id", "id"):
                value = data.get(key)
                if isinstance(value, int):
                    return value
                if isinstance(value, str) and value.isdigit():
                    return int(value)
        return None

    def take_picture_verified(self, *, timeout: int | None = None, poll_interval: float = 1.0) -> dict[str, Any]:
        before = self.latest_media(media_type=1)
        trigger = self.take_picture()
        last_result: Any = None
        matched_photo: MediaItem | None = None

        def fetch_picture_state() -> dict[str, Any]:
            nonlocal last_result
            last_result = self.picture_result()
            result_id = self._extract_picture_result_id(last_result)
            latest_photo = self.latest_media(media_type=1)
            return {
                "result_id": result_id,
                "latest_photo": latest_photo,
            }

        def picture_ready(state: dict[str, Any]) -> bool:
            nonlocal matched_photo
            latest_photo = state.get("latest_photo")
            result_id = state.get("result_id")
            if not isinstance(latest_photo, MediaItem):
                return False
            if before is None:
                if result_id is None or latest_photo.id == result_id:
                    matched_photo = latest_photo
                    return True
                return False
            if latest_photo.id != before.id and (result_id is None or latest_photo.id == result_id):
                matched_photo = latest_photo
                return True
            return False

        _, attempts, elapsed_sec, timed_out = self._poll_until(
            fetch_picture_state,
            predicate=picture_ready,
            timeout=float(timeout or self.picture_timeout),
            poll_interval=poll_interval,
        )

        final_photo = matched_photo or self.latest_media(media_type=1)

        return {
            "trigger": trigger,
            "before": before.to_dict() if before else None,
            "picture_result": last_result,
            "after": final_photo.to_dict() if final_photo else None,
            "captured": matched_photo is not None,
            "poll_attempts": attempts,
            "elapsed_sec": round(elapsed_sec, 2),
            "timed_out": timed_out,
        }

    def get_ir_status(self) -> Any:
        return self.camera_request_json("GET", "/media/getIrStatus")

    def set_day_night_mode_path(self, camera_path: str) -> Any:
        return self.camera_request_json("GET", camera_path)

    def set_day_night_mode(self, mode: int) -> Any:
        return self.camera_request_json_body(
            "POST",
            "/media/setDayNightMode",
            {"data": {"DayNightMode": mode}},
        )

    def start_video_recording(self) -> Any:
        return self.camera_request_json("GET", "/media/video/start")

    def stop_video_recording_path(self, camera_path: str = "/media/video/stop") -> Any:
        return self.camera_request_json("GET", camera_path)

    def stop_video_recording_verified(
        self,
        camera_path: str = "/media/video/stop",
        *,
        timeout: int | None = None,
        poll_interval: float = 1.0,
    ) -> dict[str, Any]:
        before = self.latest_media(media_type=2)
        response = self.stop_video_recording_path(camera_path)
        matched_video: MediaItem | None = None

        def fetch_video() -> MediaItem | None:
            return self.latest_media(media_type=2)

        def video_ready(latest_video: Any) -> bool:
            nonlocal matched_video
            if not isinstance(latest_video, MediaItem):
                return False
            if before is None or latest_video.id != before.id:
                matched_video = latest_video
                return True
            return False

        _, attempts, elapsed_sec, timed_out = self._poll_until(
            fetch_video,
            predicate=video_ready,
            timeout=float(timeout or self.picture_timeout),
            poll_interval=poll_interval,
        )

        final_video = matched_video or self.latest_media(media_type=2)
        return {
            "response": response,
            "before": before.to_dict() if before else None,
            "after": final_video.to_dict() if final_video else None,
            "stopped": isinstance(response, dict) and response.get("code") == 0,
            "new_video_observed": matched_video is not None,
            "poll_attempts": attempts,
            "elapsed_sec": round(elapsed_sec, 2),
            "timed_out": timed_out,
        }

    def download_file_to_path(
        self,
        file_path: str,
        dest_path: str,
        *,
        auto_bringup: bool = True,
    ) -> tuple[int, dict[str, str]]:
        return self.download_to_file(self._normalize_file_path(file_path), dest_path, auto_bringup=auto_bringup)

    def download_media_to_path(
        self,
        media: MediaItem | int,
        dest_path: str,
        *,
        media_type: int | None = None,
        auto_bringup: bool = True,
    ) -> tuple[int, dict[str, str]]:
        return self.download_to_file(
            self.media_file_path(media, media_type=media_type),
            dest_path,
            auto_bringup=auto_bringup,
        )

    def download_thumbnail_to_path(
        self,
        media: MediaItem | int,
        dest_path: str,
        *,
        media_type: int | None = None,
        auto_bringup: bool = True,
    ) -> tuple[int, dict[str, str]]:
        return self.download_to_file(
            self.media_thumbnail_path(media, media_type=media_type),
            dest_path,
            auto_bringup=auto_bringup,
        )

    def download_to_file(
        self,
        camera_path: str,
        dest_path: str,
        *,
        auto_bringup: bool = True,
    ) -> tuple[int, dict[str, str]]:
        result = self.download_file_result(camera_path, dest_path, auto_bringup=auto_bringup)
        return int(result["status"]), dict(result["headers"])

    def download_file_result(
        self,
        camera_path: str,
        dest_path: str,
        *,
        auto_bringup: bool = True,
    ) -> dict[str, Any]:
        status, headers, body = self.camera_get(camera_path, auto_bringup=auto_bringup)
        written = False
        bytes_written = 0
        if 200 <= status < 300:
            Path(dest_path).write_bytes(body)
            written = True
            bytes_written = len(body)
        return {
            "camera_path": camera_path,
            "status": status,
            "headers": headers,
            "output": dest_path,
            "written": written,
            "bytes_written": bytes_written,
        }

    def download_media_result(
        self,
        media: MediaItem | int,
        dest_path: str,
        *,
        media_type: int | None = None,
        thumbnail: bool = False,
        auto_bringup: bool = True,
    ) -> dict[str, Any]:
        item = self.resolve_media(media, media_type=media_type)
        camera_path = self.media_thumbnail_path(item) if thumbnail else self.media_file_path(item)
        result = self.download_file_result(camera_path, dest_path, auto_bringup=auto_bringup)
        result["media"] = item.to_dict()
        result["kind"] = "thumbnail" if thumbnail else "file"
        return result
