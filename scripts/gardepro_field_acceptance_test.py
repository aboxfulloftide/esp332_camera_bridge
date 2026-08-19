#!/usr/bin/env python3
"""Field acceptance test for the ESP32 GardePro bridge.

This intentionally does not:
- run Wi-Fi/BLE scanner tests,
- enable scheduled scanners,
- call onboard delete-all,
- delete trail-camera media.

It does:
- verify all status endpoints return data,
- test onboard capture/download/delete-one-created-photo,
- attempt trail-camera bringup,
- run trail-camera HTTP/media/picture/live-view tests only if camera Wi-Fi connects.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


STATUS_ENDPOINTS = [
    "/status",
    "/system/status",
    "/halow/status",
    "/wifi/status",
    "/camera/status",
    "/timing/status",
    "/stream/status",
    "/ble/status",
    "/control/status",
    "/battery/status",
    "/onboard/status",
    "/upload/status",
    "/session/status",
    "/sd/status",
    "/scanner/config",
    "/firmware/status",
]


def now_stamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


class FieldTester:
    def __init__(self, base_url: str, out_dir: Path, timeout: float, pause: float) -> None:
        self.base_url = base_url.rstrip("/")
        self.out_dir = out_dir
        self.timeout = timeout
        self.pause = pause
        self.results: list[dict[str, Any]] = []
        self.out_dir.mkdir(parents=True, exist_ok=True)

    def record(self, phase: str, name: str, ok: bool, **data: Any) -> dict[str, Any]:
        item = {"time": now_stamp(), "phase": phase, "name": name, "ok": ok, **data}
        self.results.append(item)
        status = "PASS" if ok else "FAIL"
        detail = data.get("summary") or data.get("error") or ""
        print(f"[{status}] {phase}: {name} {detail}".rstrip(), flush=True)
        return item

    def request(self, method: str, path: str, timeout: float | None = None) -> tuple[bool, int | None, bytes, str]:
        url = self.base_url + path
        req = urllib.request.Request(url, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as resp:
                return True, resp.status, resp.read(), ""
        except urllib.error.HTTPError as exc:
            return False, exc.code, exc.read(), str(exc)
        except Exception as exc:
            return False, None, b"", str(exc)

    def json_request(self, phase: str, name: str, method: str, path: str, timeout: float | None = None) -> dict[str, Any] | None:
        ok, status, body, error = self.request(method, path, timeout)
        if not ok:
            self.record(phase, name, False, status=status, error=error, path=path)
            time.sleep(self.pause)
            return None
        try:
            data = json.loads(body.decode("utf-8", "replace"))
        except Exception as exc:
            self.record(phase, name, False, status=status, error=f"invalid json: {exc}", body=body[:200].decode("utf-8", "replace"), path=path)
            time.sleep(self.pause)
            return None
        self.record(phase, name, True, status=status, path=path, data=data)
        time.sleep(self.pause)
        return data

    def download(self, phase: str, name: str, path: str, dest: Path, timeout: float | None = None) -> bool:
        ok, status, body, error = self.request("GET", path, timeout)
        if not ok:
            self.record(phase, name, False, status=status, error=error, path=path)
            time.sleep(self.pause)
            return False
        dest.write_bytes(body)
        jpeg = body.startswith(b"\xff\xd8")
        self.record(phase, name, jpeg and len(body) > 1000, status=status, path=path, output=str(dest), bytes=len(body), jpeg=jpeg)
        time.sleep(self.pause)
        return jpeg and len(body) > 1000

    def ping(self, host: str, count: int) -> None:
        proc = subprocess.run(["ping", "-c", str(count), host], text=True, capture_output=True)
        output = proc.stdout + proc.stderr
        self.record("link", "ping", proc.returncode == 0, summary=output.strip().splitlines()[-1] if output.strip() else "", output=output)

    def phase_status(self) -> None:
        for path in STATUS_ENDPOINTS:
            self.json_request("status", path, "GET", path)

    def phase_onboard(self) -> None:
        before = self.json_request("onboard", "status_before", "GET", "/onboard/status")
        self.json_request("onboard", "media_before", "GET", "/onboard/media?limit=20")
        capture = self.json_request("onboard", "capture", "POST", "/onboard/capture", timeout=90)
        media_id = None
        if capture:
            media = capture.get("media") or {}
            media_id = media.get("id")
        if media_id:
            self.download("onboard", "latest_download", "/onboard/latest.jpg", self.out_dir / "onboard_latest.jpg", timeout=90)
            self.download("onboard", "media_download", f"/onboard/media/{media_id}", self.out_dir / f"onboard_{media_id}.jpg", timeout=90)
            self.json_request("onboard", "delete_created_media", "DELETE", f"/onboard/media/{media_id}")
        else:
            self.record("onboard", "delete_created_media", False, error="skipped; no captured media id")
        after = self.json_request("onboard", "status_after", "GET", "/onboard/status")
        if before and after:
            self.record(
                "onboard",
                "storage_count_check",
                True,
                summary=f"before={before.get('stored_photo_count')} after={after.get('stored_photo_count')}",
            )

    def phase_bringup(self) -> bool:
        self.json_request("trail_camera", "bringup_request", "POST", "/control/bringup", timeout=60)
        final_control = None
        final_wifi = None
        final_ble = None
        for idx in range(1, 13):
            final_control = self.json_request("trail_camera", f"control_poll_{idx}", "GET", "/control/status")
            final_ble = self.json_request("trail_camera", f"ble_poll_{idx}", "GET", "/ble/status")
            final_wifi = self.json_request("trail_camera", f"wifi_poll_{idx}", "GET", "/wifi/status")
            if final_wifi and final_wifi.get("wifi_connected") is True:
                self.record("trail_camera", "bringup_gate", True, summary="wifi_connected=true")
                return True
            if final_control and final_control.get("control_busy") is False and idx >= 2:
                break
            time.sleep(5)
        self.json_request("trail_camera", "timing_after_bringup", "GET", "/timing/status")
        self.record(
            "trail_camera",
            "bringup_gate",
            False,
            summary=f"wifi_connected={None if final_wifi is None else final_wifi.get('wifi_connected')} "
                    f"control_last={None if final_control is None else final_control.get('control_last_message')} "
                    f"ble_stage={None if final_ble is None else final_ble.get('ble_stage')}",
        )
        return False

    def camera_get(self, label: str, camera_path: str, timeout: float = 45) -> dict[str, Any] | None:
        encoded = urllib.parse.quote(camera_path, safe="")
        return self.json_request("trail_camera", label, "GET", f"/camera/request?method=GET&path={encoded}", timeout=timeout)

    def phase_trail_camera(self) -> None:
        for path in ["/camera/info/1", "/camera/info/2", "/camera/getParaSetting", "/camera/standby/reset"]:
            self.json_request("trail_camera", path, "GET", path, timeout=45)
        self.json_request("trail_camera", "gallery", "GET", "/camera/gallery", timeout=60)
        latest = self.json_request("trail_camera", "latest_metadata", "GET", "/camera/latest", timeout=60)
        latest_id = None
        if latest:
            data = latest.get("data")
            if isinstance(data, dict):
                latest_id = data.get("id")
        if latest_id:
            self.download(
                "trail_camera",
                "latest_file_download",
                f"/camera/raw?path=/file/{latest_id}/JPG",
                self.out_dir / f"trail_latest_{latest_id}.jpg",
                timeout=180,
            )

        self.camera_get("picture_take", "/media/pic/take", timeout=60)
        created_id = None
        for idx in range(1, 11):
            result = self.camera_get(f"picture_result_{idx}", "/media/pic/result", timeout=45)
            if result is not None and result.get("code") == 0:
                data = result.get("data")
                if isinstance(data, dict):
                    created_id = data.get("fileIdx") or data.get("id")
                if created_id:
                    break
            time.sleep(2)
        self.json_request("trail_camera", "gallery_after_picture", "GET", "/camera/gallery", timeout=60)
        if created_id:
            self.download(
                "trail_camera",
                "created_picture_download",
                f"/camera/raw?path=/file/{created_id}/JPG",
                self.out_dir / f"trail_created_{created_id}.jpg",
                timeout=180,
            )
        else:
            self.record("trail_camera", "created_picture_download", False, error="skipped; no fileIdx from picture result")

    def phase_live_view(self) -> None:
        self.json_request("live_view", "stream_start", "POST", "/control/stream_start", timeout=60)
        saw_active = False
        saw_tunnel = False
        max_primary_packets = 0
        for idx in range(1, 9):
            self.json_request("live_view", f"control_poll_{idx}", "GET", "/control/status")
            stream = self.json_request("live_view", f"stream_poll_{idx}", "GET", "/stream/status")
            if stream and stream.get("stream_active") is True:
                saw_active = True
            if stream and stream.get("tunnel_connected") is True:
                saw_tunnel = True
            if stream:
                try:
                    max_primary_packets = max(max_primary_packets, int(stream.get("media_primary_packets") or 0))
                except Exception:
                    pass
            time.sleep(5)
        self.record(
            "live_view",
            "stream_active_gate",
            saw_active and saw_tunnel and max_primary_packets > 0,
            summary=f"active={saw_active} tunnel={saw_tunnel} media_primary_packets={max_primary_packets}",
        )
        self.json_request("live_view", "stream_stop", "POST", "/control/stream_stop", timeout=45)
        self.json_request("live_view", "stream_after_stop", "GET", "/stream/status")

    def save(self) -> None:
        report = self.out_dir / "field_acceptance_report.json"
        report.write_text(json.dumps(self.results, indent=2))
        print(f"Report: {report}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://192.168.1.160:18080")
    parser.add_argument("--out-dir", default="/tmp/gardepro_field_test")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--pause", type=float, default=1)
    parser.add_argument("--ping-count", type=int, default=50)
    parser.add_argument("--skip-ping", action="store_true")
    args = parser.parse_args()

    tester = FieldTester(args.base_url, Path(args.out_dir), args.timeout, args.pause)
    host = urllib.parse.urlparse(args.base_url).hostname or "192.168.1.160"

    if not args.skip_ping:
        tester.ping(host, args.ping_count)
    tester.phase_status()
    tester.phase_onboard()
    if tester.phase_bringup():
        tester.phase_trail_camera()
        tester.phase_live_view()
    else:
        tester.record("trail_camera", "dependent_tests", False, summary="skipped camera HTTP/media/picture/live-view because camera Wi-Fi did not connect")
    tester.save()

    failures = [item for item in tester.results if not item["ok"]]
    print(f"Total checks: {len(tester.results)} failures/skips: {len(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
