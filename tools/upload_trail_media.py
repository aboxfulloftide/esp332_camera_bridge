#!/usr/bin/env python3
import argparse
import concurrent.futures
import hashlib
import json
import mimetypes
import os
import re
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

import requests


def request_json(url, method, token, body=None, headers=None, timeout=180):
    request_headers = {"Accept": "application/json"}
    if token:
        request_headers["Authorization"] = f"Bearer {token}"
    if headers:
        request_headers.update(headers)
    response = requests.request(method, url, data=body, headers=request_headers, timeout=timeout)
    response.raise_for_status()
    return response.status_code, response.json()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_token(config_path):
    text = config_path.read_text()
    match = re.search(r'^#define\s+UPSTREAM_API_TOKEN\s+"([^"]*)"', text, re.MULTILINE)
    return match.group(1) if match else ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--base-url", default="http://192.168.1.42/trail_cam")
    parser.add_argument("--scanner-host", default="trail_esp32")
    parser.add_argument("--config", type=Path,
                        default=Path("gardepro_dual_radio_bridge/local_config.h"))
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--retries", type=int, default=5)
    parser.add_argument("--state", type=Path, default=Path(".trail_media_upload_state.json"))
    args = parser.parse_args()

    token = load_token(args.config)
    files = sorted((p for p in args.directory.iterdir()
                    if p.is_file() and p.suffix.lower() in {".jpg", ".jpeg", ".mp4"}),
                   key=lambda p: p.name)
    state = {"completed": {}}
    if args.state.exists():
        state = json.loads(args.state.read_text())
    completed = state.setdefault("completed", {})
    lock = threading.Lock()
    started = time.monotonic()
    initial_done = len(completed)

    def save_state():
        temp = args.state.with_suffix(args.state.suffix + ".tmp")
        temp.write_text(json.dumps(state, sort_keys=True, indent=2) + "\n")
        os.replace(temp, args.state)

    def upload(path):
        stat = path.stat()
        existing = completed.get(path.name)
        if existing and existing.get("bytes") == stat.st_size:
            return "skipped", path.name, stat.st_size, None
        content_type = "video/mp4" if path.suffix.lower() == ".mp4" else "image/jpeg"
        digest = sha256_file(path)
        recorded_at = datetime.fromtimestamp(stat.st_mtime, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        trail_id = int(path.stem) if path.stem.isdigit() else None
        metadata = {
            "scanner_host": args.scanner_host,
            "source": "trail_camera",
            "media_type": content_type,
            "filename": path.name,
            "recorded_at": recorded_at,
            "bytes": stat.st_size,
            "sha256": digest,
            "metadata": {"trail_media_id": trail_id},
        }
        last_error = None
        for attempt in range(1, args.retries + 1):
            try:
                _, accepted = request_json(
                    f"{args.base_url}/api/board/media", "POST", token,
                    json.dumps(metadata).encode(), {"Content-Type": "application/json"}, 45)
                upload_url = accepted.get("upload_url")
                if not accepted.get("ok") or not upload_url:
                    raise RuntimeError(f"metadata rejected: {accepted}")
                upload_headers = {"Content-Type": content_type,
                                  "Content-Length": str(stat.st_size),
                                  "X-SHA256": digest,
                                  "Accept": "application/json"}
                if token:
                    upload_headers["Authorization"] = f"Bearer {token}"
                with path.open("rb") as source:
                    response = requests.put(
                        requests.compat.urljoin(args.base_url + "/", upload_url),
                        data=source, headers=upload_headers, timeout=(10, 300))
                    response.raise_for_status()
                    result = response.json()
                if not result.get("ok") or int(result.get("bytes", -1)) != stat.st_size:
                    raise RuntimeError(f"blob verification failed: {result}")
                with lock:
                    completed[path.name] = {
                        "bytes": stat.st_size, "sha256": digest,
                        "media_id": accepted.get("media_id")}
                    save_state()
                return "uploaded", path.name, stat.st_size, None
            except Exception as error:
                last_error = str(error)
                if attempt < args.retries:
                    time.sleep(min(30, 2 ** attempt))
        return "failed", path.name, stat.st_size, last_error

    uploaded_files = uploaded_bytes = skipped_files = failed_files = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(upload, path) for path in files]
        for index, future in enumerate(concurrent.futures.as_completed(futures), 1):
            status, name, size, error = future.result()
            if status == "uploaded":
                uploaded_files += 1
                uploaded_bytes += size
            elif status == "skipped":
                skipped_files += 1
            else:
                failed_files += 1
            print(json.dumps({"progress": index, "total": len(files), "status": status,
                              "file": name, "bytes": size, "error": error,
                              "elapsed_s": round(time.monotonic() - started, 1)}), flush=True)

    print(json.dumps({"complete": failed_files == 0, "total": len(files),
                      "uploaded": uploaded_files, "uploaded_bytes": uploaded_bytes,
                      "skipped": skipped_files, "failed": failed_files,
                      "checkpoint_initial": initial_done,
                      "elapsed_s": round(time.monotonic() - started, 1)}), flush=True)
    raise SystemExit(1 if failed_files else 0)


if __name__ == "__main__":
    main()
