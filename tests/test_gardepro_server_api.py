import unittest

from gardepro_server_api import GardeProServerAPI, MediaItem


class GardeProServerAPITests(unittest.TestCase):
    def make_api(self) -> GardeProServerAPI:
        api = object.__new__(GardeProServerAPI)
        api.control_timeout = 150
        api.picture_timeout = 30
        api.http_timeout = 30
        api.port = 18080
        api.host = "127.0.0.1"
        api.registration_path = "/tmp/unused"
        return api

    def test_open_session_reports_before_and_after_status(self) -> None:
        api = self.make_api()
        calls: list[tuple[str, object, object]] = []
        status_values = [
            {"wifi_connected": False, "stream_active": False},
            {"wifi_connected": True, "stream_active": False},
        ]

        def status():
            calls.append(("status", None, None))
            return status_values.pop(0)

        def bringup(*, timeout=None, poll_interval=1.0):
            calls.append(("bringup", timeout, poll_interval))
            return {"wifi_connected": True, "control_busy": False}

        api.status = status
        api.bringup = bringup

        result = api.open_session(timeout=12, poll_interval=0.5)

        self.assertTrue(result["session_ready"])
        self.assertEqual(result["before"]["wifi_connected"], False)
        self.assertEqual(result["after"]["wifi_connected"], True)
        self.assertEqual(
            calls,
            [
                ("status", None, None),
                ("bringup", 12, 0.5),
                ("status", None, None),
            ],
        )

    def test_close_session_stops_stream_then_requests_standby(self) -> None:
        api = self.make_api()
        calls: list[tuple[str, object, object]] = []

        def status():
            calls.append(("status", None, None))
            if len([name for name, _, _ in calls if name == "status"]) == 1:
                return {"wifi_connected": True, "stream_active": True}
            return {"wifi_connected": False, "stream_active": False}

        def stream_stop(*, timeout=None, poll_interval=1.0):
            calls.append(("stream_stop", timeout, poll_interval))
            return {"wifi_connected": True, "stream_active": False}

        def standby_now():
            calls.append(("standby_now", None, None))
            return {"code": 0}

        api.status = status
        api.stream_stop = stream_stop
        api.standby_now = standby_now

        result = api.close_session(timeout=9, poll_interval=0.25)

        self.assertTrue(result["session_closed"])
        self.assertTrue(result["standby_requested"])
        self.assertIsNotNone(result["stream_stop"])
        self.assertEqual(result["standby"], {"code": 0})
        self.assertEqual(
            calls,
            [
                ("status", None, None),
                ("stream_stop", 9, 0.25),
                ("standby_now", None, None),
                ("status", None, None),
            ],
        )

    def test_close_session_without_standby_keeps_wifi_requirement_relaxed(self) -> None:
        api = self.make_api()
        calls: list[tuple[str, object, object]] = []

        def status():
            calls.append(("status", None, None))
            if len([name for name, _, _ in calls if name == "status"]) == 1:
                return {"wifi_connected": True, "stream_active": True}
            return {"wifi_connected": True, "stream_active": False}

        def stream_stop(*, timeout=None, poll_interval=1.0):
            calls.append(("stream_stop", timeout, poll_interval))
            return {"wifi_connected": True, "stream_active": False}

        api.status = status
        api.stream_stop = stream_stop
        api.standby_now = lambda: self.fail("standby_now should not be called")

        result = api.close_session(timeout=7, poll_interval=0.75, standby=False)

        self.assertTrue(result["session_closed"])
        self.assertFalse(result["standby_requested"])
        self.assertIsNone(result["standby"])
        self.assertEqual(
            calls,
            [
                ("status", None, None),
                ("stream_stop", 7, 0.75),
                ("status", None, None),
            ],
        )

    def test_close_session_polls_until_standby_disconnects_wifi(self) -> None:
        api = self.make_api()
        calls: list[tuple[str, object, object]] = []
        statuses = [
            {"wifi_connected": True, "stream_active": False},
            {"wifi_connected": True, "stream_active": False},
            {"wifi_connected": False, "stream_active": False},
        ]

        def status():
            calls.append(("status", None, None))
            return statuses.pop(0)

        def standby_now():
            calls.append(("standby_now", None, None))
            return {"code": 0}

        api.status = status
        api.standby_now = standby_now

        result = api.close_session(timeout=2, poll_interval=0)

        self.assertTrue(result["session_closed"])
        self.assertEqual(result["standby"], {"code": 0})
        self.assertEqual(
            calls,
            [
                ("status", None, None),
                ("standby_now", None, None),
                ("status", None, None),
                ("status", None, None),
            ],
        )

    def test_session_context_closes_after_exception(self) -> None:
        api = self.make_api()
        calls: list[tuple[str, object, object, object]] = []

        def open_session(*, timeout=None, poll_interval=1.0):
            calls.append(("open_session", timeout, poll_interval, None))
            return {"session_ready": True}

        def close_session(*, timeout=None, poll_interval=1.0, standby=True):
            calls.append(("close_session", timeout, poll_interval, standby))
            return {"session_closed": True}

        api.open_session = open_session
        api.close_session = close_session

        with self.assertRaisesRegex(RuntimeError, "boom"):
            with api.session(timeout=5, poll_interval=0.2, close_timeout=3, close_poll_interval=0.4):
                raise RuntimeError("boom")

        self.assertEqual(
            calls,
            [
                ("open_session", 5, 0.2, None),
                ("close_session", 3, 0.4, True),
            ],
        )

    def test_run_in_session_wraps_action(self) -> None:
        api = self.make_api()
        calls: list[str] = []

        def open_session(*, timeout=None, poll_interval=1.0):
            calls.append("open")
            return {"session_ready": True}

        def close_session(*, timeout=None, poll_interval=1.0, standby=True):
            calls.append("close")
            return {"session_closed": True}

        api.open_session = open_session
        api.close_session = close_session

        result = api.run_in_session(lambda current_api: calls.append("action") or {"ok": current_api is api})

        self.assertEqual(result, {"result": {"ok": True}})
        self.assertEqual(calls, ["open", "action", "close"])

    def test_camera_request_get_omits_method_query(self) -> None:
        api = self.make_api()
        calls: list[tuple[str, str, object, object, str]] = []

        def request_json(method, path, *, query=None, body=None, content_type=""):
            calls.append((method, path, query, body, content_type))
            return 200, {}, {"code": 0}

        api.request_json = request_json
        api.bringup = lambda: self.fail("bringup should not be called")

        result = api.camera_request_json("GET", "/cmd/getSetting")

        self.assertEqual(result, {"code": 0})
        self.assertEqual(
            calls,
            [
                ("GET", "/camera/request", {"path": "/cmd/getSetting"}, None, ""),
            ],
        )

    def test_camera_request_post_includes_method_and_content_type(self) -> None:
        api = self.make_api()
        calls: list[tuple[str, str, object, object, str]] = []

        def request_json(method, path, *, query=None, body=None, content_type=""):
            calls.append((method, path, query, body, content_type))
            return 200, {}, {"code": 0}

        api.request_json = request_json
        api.bringup = lambda: self.fail("bringup should not be called")

        result = api.camera_request_json_body("POST", "/cmd/setSetting", {"data": {"mode": 1}})

        self.assertEqual(result, {"code": 0})
        self.assertEqual(
            calls,
            [
                (
                    "POST",
                    "/camera/request",
                    {
                        "path": "/cmd/setSetting",
                        "method": "POST",
                        "content_type": "application/json",
                    },
                    b'{"data":{"mode":1}}',
                    "application/json",
                ),
            ],
        )

    def test_list_media_in_session_returns_serialized_media(self) -> None:
        api = self.make_api()
        calls: list[str] = []

        def open_session(*, timeout=None, poll_interval=1.0):
            calls.append("open")
            return {"session_ready": True}

        def close_session(*, timeout=None, poll_interval=1.0, standby=True):
            calls.append("close")
            return {"session_closed": True}

        def list_media():
            calls.append("list_media")
            return [MediaItem(id=126, type=1, date="2026-04-22 10:00:00", size=42, uid="abc")]

        api.open_session = open_session
        api.close_session = close_session
        api.list_media = list_media

        result = api.list_media_in_session()

        self.assertEqual(calls, ["open", "list_media", "close"])
        self.assertEqual(
            result,
            {
                "result": [
                    {
                        "id": 126,
                        "type": 1,
                        "date": "2026-04-22 10:00:00",
                        "size": 42,
                        "uid": "abc",
                        "file_path": "/file/126/JPG",
                        "thumbnail_path": "/thumb/126/JPG",
                        "delete_path": "/cmd/delete/126/JPG",
                        "delete_legacy_path": "/cmd/delete/1/126",
                    }
                ]
            },
        )

    def test_stop_video_recording_verified_accepts_pending_video_item(self) -> None:
        api = self.make_api()
        calls: list[str] = []
        items = [
            MediaItem(id=166, type=2, date="2026-04-23 14:26:04", size=49385892, uid="00000000"),
            MediaItem(id=166, type=2, date="2026-04-23 14:26:04", size=49385892, uid="00000000"),
            MediaItem(id=166, type=2, date="2026-04-23 14:26:04", size=49385892, uid="00000000"),
        ]

        def latest_media(*, media_type=None):
            calls.append(f"latest_media:{media_type}")
            return items.pop(0)

        def stop_video_recording_path(camera_path="/media/video/stop"):
            calls.append(f"stop:{camera_path}")
            return {"code": 0, "desc": "success"}

        api.latest_media = latest_media
        api.stop_video_recording_path = stop_video_recording_path

        result = api.stop_video_recording_verified(timeout=5, poll_interval=0)

        self.assertTrue(result["stopped"])
        self.assertTrue(result["new_video_observed"])
        self.assertFalse(result["timed_out"])
        self.assertEqual(result["before"]["id"], 166)
        self.assertEqual(result["after"]["id"], 166)
        self.assertEqual(
            calls,
            [
                "latest_media:2",
                "stop:/media/video/stop",
                "latest_media:2",
            ],
        )


if __name__ == "__main__":
    unittest.main()
