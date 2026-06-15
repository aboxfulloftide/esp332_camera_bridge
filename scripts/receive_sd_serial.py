#!/usr/bin/env python3
"""Receive files streamed by sd_serial_transfer.ino."""
from __future__ import annotations

import argparse
import os
import re
import sys
import time
from pathlib import Path

import serial


FILE_BEGIN_RE = re.compile(rb"^FILE_BEGIN:(.*):([0-9]+)\r?\n$")


def unescape_path(raw: bytes) -> str:
    text = raw.decode("utf-8", errors="replace")
    out = []
    i = 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt == "n":
                out.append("\n")
            elif nxt == "r":
                out.append("\r")
            elif nxt == "\\":
                out.append("\\")
            else:
                out.append(nxt)
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def safe_output_path(root: Path, sd_path: str) -> Path:
    relative = sd_path.lstrip("/")
    parts = [part for part in Path(relative).parts if part not in ("", ".", "..")]
    if not parts:
        raise ValueError(f"invalid SD path: {sd_path!r}")
    return root.joinpath(*parts)


def read_line(ser: serial.Serial) -> bytes:
    line = ser.readline()
    if not line:
        raise TimeoutError("timed out waiting for serial line")
    return line


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    with serial.Serial(
        args.port,
        args.baud,
        timeout=args.timeout,
        dsrdtr=False,
        rtscts=False,
        xonxoff=False,
    ) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.2)

        files = 0
        bytes_written = 0
        while True:
            line = read_line(ser)
            sys.stdout.write(line.decode("utf-8", errors="replace"))
            sys.stdout.flush()

            if line.startswith(b"SD_TRANSFER_DONE"):
                print(f"received_files={files} received_bytes={bytes_written}")
                return 0

            match = FILE_BEGIN_RE.match(line)
            if not match:
                continue

            sd_path = unescape_path(match.group(1))
            size = int(match.group(2))
            destination = safe_output_path(output_dir, sd_path)
            destination.parent.mkdir(parents=True, exist_ok=True)

            remaining = size
            with destination.open("wb") as handle:
                while remaining:
                    chunk = ser.read(min(65536, remaining))
                    if not chunk:
                        raise TimeoutError(f"timed out receiving {sd_path!r}")
                    handle.write(chunk)
                    remaining -= len(chunk)

            end_line = read_line(ser)
            while end_line in (b"\n", b"\r\n"):
                end_line = read_line(ser)
            sys.stdout.write(end_line.decode("utf-8", errors="replace"))
            sys.stdout.flush()
            if not end_line.startswith(b"FILE_END:"):
                raise RuntimeError(f"expected FILE_END after {sd_path!r}, got {end_line!r}")

            files += 1
            bytes_written += size
            print(f"SAVED:{destination}:{size}")


if __name__ == "__main__":
    raise SystemExit(main())
