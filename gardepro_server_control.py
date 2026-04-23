#!/usr/bin/env python3
"""
CLI wrapper around the server-side GardePro board API.
"""

from __future__ import annotations

import argparse
import json

from gardepro_server_api import GardeProServerAPI


def main() -> int:
    parser = argparse.ArgumentParser(description="Control the registered GardePro ESP32 bridge")
    parser.add_argument(
        "command",
        choices=[
            "status",
            "bringup",
            "session-open",
            "session-close",
            "stream-start",
            "stream-stop",
            "settings",
            "setting-values",
            "setting-keys",
            "setting-get",
            "setting-set",
            "gallery",
            "info",
            "ir-status",
            "take-picture",
            "picture-result",
            "video-start",
            "video-stop",
            "format-start",
            "format-result",
            "reboot-camera",
            "factory-reset",
            "standby-now",
            "camera-request",
            "camera-get",
            "file-download",
            "download",
            "set-setting-json",
            "setting-update-json",
            "set-clock",
            "set-day-night-mode",
            "media-list",
            "media-latest",
            "media-paths",
            "media-download",
            "thumb-download",
            "media-delete",
        ],
    )
    parser.add_argument("--registration-path", default="/tmp/gardepro_board_registration.json")
    parser.add_argument("--host", default="")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--index", type=int, default=1)
    parser.add_argument("--camera-path", default="")
    parser.add_argument("--output", default="")
    parser.add_argument("--method", default="GET")
    parser.add_argument("--json", default="")
    parser.add_argument("--timestamp", default="")
    parser.add_argument("--mode", type=int, default=0)
    parser.add_argument("--media-id", type=int, default=0)
    parser.add_argument("--media-type", type=int, default=0)
    parser.add_argument("--key", default="")
    parser.add_argument("--value-json", default="")
    parser.add_argument("--timeout", type=int, default=0)
    parser.add_argument("--poll-interval", type=float, default=1.0)
    parser.add_argument("--no-auto-bringup", action="store_true")
    parser.add_argument("--no-standby", action="store_true")
    args = parser.parse_args()

    api = GardeProServerAPI(
        host=args.host,
        port=args.port,
        registration_path=args.registration_path,
    )

    if args.command == "status":
        payload = api.status()
        print(json.dumps(payload, indent=2))
        return 0
    if args.command == "bringup":
        payload = api.bringup(timeout=args.timeout or None, poll_interval=args.poll_interval)
        print(json.dumps(payload, indent=2))
        return 0
    if args.command == "session-open":
        payload = api.open_session(timeout=args.timeout or None, poll_interval=args.poll_interval)
        print(json.dumps(payload, indent=2))
        return 0
    if args.command == "session-close":
        payload = api.close_session(
            timeout=args.timeout or None,
            poll_interval=args.poll_interval,
            standby=not args.no_standby,
        )
        print(json.dumps(payload, indent=2))
        return 0
    if args.command == "stream-start":
        payload = api.stream_start(timeout=args.timeout or None, poll_interval=args.poll_interval)
        print(json.dumps(payload, indent=2))
        return 0
    if args.command == "stream-stop":
        payload = api.stream_stop(timeout=args.timeout or None, poll_interval=args.poll_interval)
        print(json.dumps(payload, indent=2))
        return 0
    if args.command == "settings":
        print(json.dumps(api.get_settings(), indent=2))
        return 0
    if args.command == "setting-values":
        print(json.dumps(api.get_setting_values(), indent=2))
        return 0
    if args.command == "setting-keys":
        print(json.dumps(api.list_setting_keys(), indent=2))
        return 0
    if args.command == "setting-get":
        if not args.key:
            raise SystemExit("--key is required for setting-get")
        try:
            value = api.get_setting_value(args.key)
        except KeyError:
            raise SystemExit(f"setting key not found: {args.key}")
        print(json.dumps({args.key: value}, indent=2))
        return 0
    if args.command == "setting-set":
        if not args.key:
            raise SystemExit("--key is required for setting-set")
        if not args.value_json:
            raise SystemExit("--value-json is required for setting-set")
        value = json.loads(args.value_json)
        print(json.dumps(api.update_settings({args.key: value}), indent=2))
        return 0
    if args.command == "gallery":
        print(json.dumps(api.get_gallery(), indent=2))
        return 0
    if args.command == "media-list":
        print(json.dumps([item.to_dict() for item in api.list_media()], indent=2))
        return 0
    if args.command == "media-latest":
        item = api.latest_media(media_type=args.media_type or None)
        print(json.dumps(item.to_dict() if item else None, indent=2))
        return 0
    if args.command == "media-paths":
        if args.media_id:
            if not args.media_type:
                raise SystemExit("--media-type is required when using --media-id")
            payload = api.media_paths(args.media_id, media_type=args.media_type)
        else:
            item = api.latest_media(media_type=args.media_type or None)
            if item is None:
                raise SystemExit("no matching media item found")
            payload = api.media_paths(item)
        print(json.dumps(payload, indent=2))
        return 0
    if args.command == "info":
        print(json.dumps(api.get_info(args.index), indent=2))
        return 0
    if args.command == "ir-status":
        print(json.dumps(api.get_ir_status(), indent=2))
        return 0
    if args.command == "take-picture":
        print(
            json.dumps(
                api.take_picture_verified(
                    timeout=args.timeout or None,
                    poll_interval=args.poll_interval,
                ),
                indent=2,
            )
        )
        return 0
    if args.command == "picture-result":
        print(json.dumps(api.picture_result(), indent=2))
        return 0
    if args.command == "video-start":
        print(json.dumps(api.start_video_recording(), indent=2))
        return 0
    if args.command == "video-stop":
        print(
            json.dumps(
                api.stop_video_recording_verified(
                    args.camera_path or "/media/video/stop",
                    timeout=args.timeout or None,
                    poll_interval=args.poll_interval,
                ),
                indent=2,
            )
        )
        return 0
    if args.command == "format-start":
        print(
            json.dumps(
                api.format_sd_start_verified(
                    timeout=args.timeout or None,
                    poll_interval=args.poll_interval,
                ),
                indent=2,
            )
        )
        return 0
    if args.command == "format-result":
        print(json.dumps(api.format_sd_result(), indent=2))
        return 0
    if args.command == "reboot-camera":
        print(json.dumps(api.reboot_camera(), indent=2))
        return 0
    if args.command == "factory-reset":
        print(json.dumps(api.reset_camera_factory(), indent=2))
        return 0
    if args.command == "standby-now":
        print(json.dumps(api.standby_now(), indent=2))
        return 0
    if args.command == "camera-request":
        if not args.camera_path:
            raise SystemExit("--camera-path is required for camera-request")
        if args.json:
            payload = api.camera_request_json_body(
                args.method.upper(),
                args.camera_path,
                json.loads(args.json),
                auto_bringup=not args.no_auto_bringup,
            )
        else:
            payload = api.camera_request_json(
                args.method.upper(),
                args.camera_path,
                auto_bringup=not args.no_auto_bringup,
            )
        print(json.dumps(payload, indent=2))
        return 0
    if args.command == "camera-get":
        if not args.camera_path:
            raise SystemExit("--camera-path is required for camera-get")
        payload = api.camera_get_result(args.camera_path, auto_bringup=not args.no_auto_bringup)
        print(json.dumps(payload, indent=2))
        return 0 if 200 <= payload["status"] < 300 else 1
    if args.command == "file-download":
        if not args.camera_path or not args.output:
            raise SystemExit("--camera-path and --output are required for file-download")
        payload = api.download_file_result(
            api.normalize_file_path(args.camera_path),
            args.output,
            auto_bringup=not args.no_auto_bringup,
        )
        print(json.dumps(payload, indent=2))
        return 0 if 200 <= payload["status"] < 300 else 1
    if args.command == "download":
        if not args.camera_path or not args.output:
            raise SystemExit("--camera-path and --output are required for download")
        payload = api.download_file_result(
            args.camera_path,
            args.output,
            auto_bringup=not args.no_auto_bringup,
        )
        print(json.dumps(payload, indent=2))
        return 0 if 200 <= payload["status"] < 300 else 1
    if args.command == "media-download":
        if not args.output:
            raise SystemExit("--output is required for media-download")
        if args.media_id:
            if not args.media_type:
                raise SystemExit("--media-type is required when using --media-id")
            payload = api.download_media_result(
                args.media_id,
                args.output,
                media_type=args.media_type,
                auto_bringup=not args.no_auto_bringup,
            )
        else:
            item = api.latest_media(media_type=args.media_type or None)
            if item is None:
                raise SystemExit("no matching media item found")
            payload = api.download_media_result(
                item,
                args.output,
                auto_bringup=not args.no_auto_bringup,
            )
        print(json.dumps(payload, indent=2))
        return 0 if 200 <= payload["status"] < 300 else 1
    if args.command == "thumb-download":
        if not args.output:
            raise SystemExit("--output is required for thumb-download")
        if args.media_id:
            if not args.media_type:
                raise SystemExit("--media-type is required when using --media-id")
            payload = api.download_media_result(
                args.media_id,
                args.output,
                media_type=args.media_type,
                thumbnail=True,
                auto_bringup=not args.no_auto_bringup,
            )
        else:
            item = api.latest_media(media_type=args.media_type or None)
            if item is None:
                raise SystemExit("no matching media item found")
            payload = api.download_media_result(
                item,
                args.output,
                thumbnail=True,
                auto_bringup=not args.no_auto_bringup,
            )
        print(json.dumps(payload, indent=2))
        return 0 if 200 <= payload["status"] < 300 else 1
    if args.command == "media-delete":
        if args.media_id:
            if not args.media_type:
                raise SystemExit("--media-type is required when using --media-id")
            payload = api.delete_media_verified(args.media_id, media_type=args.media_type)
        else:
            item = api.latest_media(media_type=args.media_type or None)
            if item is None:
                raise SystemExit("no matching media item found")
            payload = api.delete_media_verified(item)
        print(json.dumps(payload, indent=2))
        return 0
    if args.command == "set-setting-json":
        if not args.json:
            raise SystemExit("--json is required for set-setting-json")
        print(json.dumps(api.set_setting_json(json.loads(args.json)), indent=2))
        return 0
    if args.command == "setting-update-json":
        if not args.json:
            raise SystemExit("--json is required for setting-update-json")
        patch = json.loads(args.json)
        if not isinstance(patch, dict):
            raise SystemExit("--json must decode to an object for setting-update-json")
        print(json.dumps(api.update_settings(patch), indent=2))
        return 0
    if args.command == "set-clock":
        if not args.timestamp:
            raise SystemExit("--timestamp is required for set-clock")
        print(json.dumps(api.set_clock(args.timestamp, path=args.camera_path or "/cmd/setGmtClock"), indent=2))
        return 0
    if args.command == "set-day-night-mode":
        print(json.dumps(api.set_day_night_mode(args.mode), indent=2))
        return 0

    raise SystemExit(f"unsupported command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
