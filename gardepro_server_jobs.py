#!/usr/bin/env python3
"""
Small server-facing job layer built on top of the GardePro session API.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from gardepro_server_api import GardeProServerAPI, MediaItem


@dataclass
class GardeProSessionJobs:
    api: GardeProServerAPI

    def fetch_status(self) -> Any:
        return self.api.status()

    def fetch_settings(
        self,
        *,
        timeout: int | None = None,
        poll_interval: float = 1.0,
        close_timeout: int | None = None,
        close_poll_interval: float | None = None,
        standby: bool = True,
    ) -> dict[str, Any]:
        return self.api.get_settings_in_session(
            timeout=timeout,
            poll_interval=poll_interval,
            close_timeout=close_timeout,
            close_poll_interval=close_poll_interval,
            standby=standby,
        )

    def fetch_setting_values(
        self,
        *,
        timeout: int | None = None,
        poll_interval: float = 1.0,
        close_timeout: int | None = None,
        close_poll_interval: float | None = None,
        standby: bool = True,
    ) -> dict[str, Any]:
        return self.api.get_setting_values_in_session(
            timeout=timeout,
            poll_interval=poll_interval,
            close_timeout=close_timeout,
            close_poll_interval=close_poll_interval,
            standby=standby,
        )

    def list_media(
        self,
        *,
        timeout: int | None = None,
        poll_interval: float = 1.0,
        close_timeout: int | None = None,
        close_poll_interval: float | None = None,
        standby: bool = True,
    ) -> dict[str, Any]:
        return self.api.list_media_in_session(
            timeout=timeout,
            poll_interval=poll_interval,
            close_timeout=close_timeout,
            close_poll_interval=close_poll_interval,
            standby=standby,
        )

    def fetch_ir_status(
        self,
        *,
        timeout: int | None = None,
        poll_interval: float = 1.0,
        close_timeout: int | None = None,
        close_poll_interval: float | None = None,
        standby: bool = True,
    ) -> dict[str, Any]:
        return self.api.get_ir_status_in_session(
            timeout=timeout,
            poll_interval=poll_interval,
            close_timeout=close_timeout,
            close_poll_interval=close_poll_interval,
            standby=standby,
        )

    def download_latest_media(
        self,
        output_path: str,
        *,
        media_type: int | None = None,
        thumbnail: bool = False,
        timeout: int | None = None,
        poll_interval: float = 1.0,
        close_timeout: int | None = None,
        close_poll_interval: float | None = None,
        standby: bool = True,
    ) -> dict[str, Any]:
        def action(api: GardeProServerAPI) -> dict[str, Any]:
            item = api.latest_media(media_type=media_type)
            if item is None:
                raise RuntimeError("no matching media item found")
            result = api.download_media_result(
                item,
                output_path,
                thumbnail=thumbnail,
            )
            return {
                "media": item.to_dict(),
                "download": result,
            }

        return self.api.run_in_session(
            action,
            timeout=timeout,
            poll_interval=poll_interval,
            close_timeout=close_timeout,
            close_poll_interval=close_poll_interval,
            standby=standby,
        )


def create_jobs(**api_kwargs: Any) -> GardeProSessionJobs:
    return GardeProSessionJobs(api=GardeProServerAPI(**api_kwargs))
