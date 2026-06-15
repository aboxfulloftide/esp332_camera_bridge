#!/usr/bin/env python3
"""Capture onboard ESP32 camera test images over the USB serial console."""

from __future__ import annotations

import argparse
import base64
import os
import select
import termios
import time
from pathlib import Path


DEFAULT_PROFILES = [
    ("uxga_q8_auto", "framesize=UXGA jpeg_quality=8 brightness=1 contrast=0 saturation=0 sharpness=0 awb=1 awb_gain=1 aec=1 aec2=1 agc=1 vflip=1 hmirror=0"),
    ("uxga_q10_less_compression", "framesize=UXGA jpeg_quality=10 brightness=1 contrast=0 saturation=0 sharpness=1 awb=1 awb_gain=1 aec=1 aec2=1 agc=1 vflip=1 hmirror=0"),
    ("sxga_q8_sharp", "framesize=SXGA jpeg_quality=8 brightness=1 contrast=1 saturation=0 sharpness=1 awb=1 awb_gain=1 aec=1 aec2=1 agc=1 vflip=1 hmirror=0"),
    ("uxga_q12_balanced", "framesize=UXGA jpeg_quality=12 brightness=1 contrast=1 saturation=0 sharpness=1 awb=1 awb_gain=1 aec=1 aec2=1 agc=1 vflip=1 hmirror=0"),
]


def configure_serial(fd: int, baud: int) -> list[int]:
    old = termios.tcgetattr(fd)
    new = termios.tcgetattr(fd)
    speed = getattr(termios, f"B{baud}")
    new[0] = 0
    new[1] = 0
    new[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    new[3] = 0
    new[4] = speed
    new[5] = speed
    new[6][termios.VMIN] = 0
    new[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, new)
    return old


class SerialConsole:
    def __init__(self, port: str, baud: int) -> None:
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.old_attrs = configure_serial(self.fd, baud)
        self.buffer = b""

    def close(self) -> None:
        termios.tcsetattr(self.fd, termios.TCSANOW, self.old_attrs)
        os.close(self.fd)

    def write_line(self, line: str) -> None:
        os.write(self.fd, (line + "\n").encode("utf-8"))

    def read_line(self, timeout: float) -> str | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if b"\n" in self.buffer:
                raw, self.buffer = self.buffer.split(b"\n", 1)
                return raw.decode("utf-8", errors="replace").strip()
            remaining = max(0.0, deadline - time.monotonic())
            readable, _, _ = select.select([self.fd], [], [], min(0.25, remaining))
            if readable:
                try:
                    chunk = os.read(self.fd, 4096)
                except BlockingIOError:
                    chunk = b""
                if chunk:
                    self.buffer += chunk
        return None

    def drain(self, seconds: float = 1.0) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            line = self.read_line(0.1)
            if line is None:
                continue

    def command_until(self, command: str, marker: str, timeout: float) -> list[str]:
        self.write_line(command)
        lines: list[str] = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.read_line(0.5)
            if line is None:
                continue
            lines.append(line)
            if marker in line:
                return lines
        raise TimeoutError(f"timed out waiting for {marker!r} after {command!r}")

    def dump_jpeg(self, timeout: float) -> bytes:
        self.write_line("onboard_dump fresh")
        b64_lines: list[str] = []
        in_body = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.read_line(0.5)
            if line is None:
                continue
            if line.startswith("ONBOARD_JPEG_ERROR"):
                raise RuntimeError(line)
            if line.startswith("BEGIN_ONBOARD_JPEG_BASE64"):
                in_body = True
                continue
            if line == "END_ONBOARD_JPEG_BASE64":
                return base64.b64decode("".join(b64_lines), validate=True)
            if in_body:
                b64_lines.append(line)
        raise TimeoutError("timed out waiting for JPEG dump")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out-dir", default="onboard_camera_tests")
    parser.add_argument("--settle-sec", type=float, default=1.5)
    parser.add_argument("--dump-timeout", type=float, default=30.0)
    parser.add_argument("--profile", action="append", default=[], help="name:key=value key=value ...")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    profiles = []
    for item in args.profile:
        name, sep, settings = item.partition(":")
        if not sep or not name or not settings:
            raise SystemExit("--profile must look like name:key=value key=value")
        profiles.append((name, settings))
    if not profiles:
        profiles = DEFAULT_PROFILES

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    serial = SerialConsole(args.port, args.baud)
    try:
        serial.drain()
        for name, settings in profiles:
            print(f"== {name} ==")
            lines = serial.command_until(f"onboard_config {settings}", "[onboard-config] result=", 10)
            for line in lines:
                if "[onboard-config]" in line:
                    print(line)
            time.sleep(args.settle_sec)
            jpeg = serial.dump_jpeg(args.dump_timeout)
            path = out_dir / f"{int(time.time())}_{name}.jpg"
            path.write_bytes(jpeg)
            print(f"saved {path} bytes={len(jpeg)}")
    finally:
        serial.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
