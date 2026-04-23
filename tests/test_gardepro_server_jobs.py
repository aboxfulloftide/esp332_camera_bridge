import unittest

from gardepro_server_api import MediaItem
from gardepro_server_jobs import GardeProSessionJobs


class GardeProSessionJobsTests(unittest.TestCase):
    def test_fetch_status_does_not_open_session(self) -> None:
        class FakeAPI:
            def __init__(self) -> None:
                self.calls: list[str] = []

            def status(self):
                self.calls.append("status")
                return {"ok": True}

        api = FakeAPI()
        jobs = GardeProSessionJobs(api=api)  # type: ignore[arg-type]

        result = jobs.fetch_status()

        self.assertEqual(result, {"ok": True})
        self.assertEqual(api.calls, ["status"])

    def test_fetch_settings_uses_one_shot_session_helper(self) -> None:
        class FakeAPI:
            def __init__(self) -> None:
                self.calls: list[tuple[object, ...]] = []

            def get_settings_in_session(self, **kwargs):
                self.calls.append(("get_settings_in_session", kwargs))
                return {"result": {"data": {"standby_timeout": 300}}}

        api = FakeAPI()
        jobs = GardeProSessionJobs(api=api)  # type: ignore[arg-type]

        result = jobs.fetch_settings(timeout=10, poll_interval=0.5, standby=False)

        self.assertEqual(result, {"result": {"data": {"standby_timeout": 300}}})
        self.assertEqual(
            api.calls,
            [
                (
                    "get_settings_in_session",
                    {
                        "timeout": 10,
                        "poll_interval": 0.5,
                        "close_timeout": None,
                        "close_poll_interval": None,
                        "standby": False,
                    },
                )
            ],
        )

    def test_download_latest_media_runs_lookup_and_download_inside_session(self) -> None:
        class FakeAPI:
            def __init__(self) -> None:
                self.calls: list[tuple[object, ...]] = []
                self.item = MediaItem(id=55, type=1, date="2026-04-22 12:00:00")

            def run_in_session(self, action, **kwargs):
                self.calls.append(("run_in_session", kwargs))
                return {"result": action(self)}

            def latest_media(self, *, media_type=None):
                self.calls.append(("latest_media", media_type))
                return self.item

            def download_media_result(self, media, output_path, thumbnail=False):
                self.calls.append(("download_media_result", media.id, output_path, thumbnail))
                return {"status": 200, "output_path": output_path}

        api = FakeAPI()
        jobs = GardeProSessionJobs(api=api)  # type: ignore[arg-type]

        result = jobs.download_latest_media(
            "/tmp/photo.jpg",
            media_type=1,
            timeout=22,
            poll_interval=0.75,
            standby=True,
        )

        self.assertEqual(
            result,
            {
                "result": {
                    "media": {
                        "id": 55,
                        "type": 1,
                        "date": "2026-04-22 12:00:00",
                        "size": 0,
                        "uid": "",
                        "file_path": "/file/55/JPG",
                        "thumbnail_path": "/thumb/55/JPG",
                        "delete_path": "/cmd/delete/55/JPG",
                        "delete_legacy_path": "/cmd/delete/1/55",
                    },
                    "download": {"status": 200, "output_path": "/tmp/photo.jpg"},
                }
            },
        )
        self.assertEqual(
            api.calls,
            [
                (
                    "run_in_session",
                    {
                        "timeout": 22,
                        "poll_interval": 0.75,
                        "close_timeout": None,
                        "close_poll_interval": None,
                        "standby": True,
                    },
                ),
                ("latest_media", 1),
                ("download_media_result", 55, "/tmp/photo.jpg", False),
            ],
        )

    def test_download_latest_media_raises_when_no_item_matches(self) -> None:
        class FakeAPI:
            def run_in_session(self, action, **kwargs):
                return {"result": action(self)}

            def latest_media(self, *, media_type=None):
                return None

        jobs = GardeProSessionJobs(api=FakeAPI())  # type: ignore[arg-type]

        with self.assertRaisesRegex(RuntimeError, "no matching media item found"):
            jobs.download_latest_media("/tmp/out.jpg", media_type=1)


if __name__ == "__main__":
    unittest.main()
