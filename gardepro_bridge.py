#!/usr/bin/env python3
"""
Minimal GardePro bridge/proxy skeleton.

This is a pragmatic first cut based only on confirmed protocol findings:

- HTTP control plane to camera: 192.168.8.1:8080
- Live media plane from camera UDP ports: 49152 / 49153

The unresolved BLE bootstrap and dynamic UDP destination negotiation are
intentionally left out. This bridge is meant to be the next layer after the
camera has already been activated and the hotspot is reachable.
"""

from __future__ import annotations

import argparse
import http.client
import http.server
import json
import logging
import socket
import socketserver
import threading
import time
import urllib.parse
from dataclasses import dataclass, asdict
from typing import Dict, Optional, Tuple


LOG = logging.getLogger("gardepro_bridge")


@dataclass
class BridgeConfig:
    camera_host: str = "192.168.8.1"
    camera_http_port: int = 8080
    http_bind_host: str = "0.0.0.0"
    http_bind_port: int = 18080
    udp_bind_host: str = "0.0.0.0"
    udp_port_primary: int = 49152
    udp_port_secondary: int = 49153
    upstream_media_host: str = ""
    upstream_media_port_primary: int = 0
    upstream_media_port_secondary: int = 0
    upstream_control_base: str = ""


class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class GardeProProxyHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _proxy(self) -> None:
        server: "GardeProBridge" = self.server.bridge  # type: ignore[attr-defined]
        raw_path = self.path
        body = self._read_body()
        forward_path = self._normalize_path(raw_path)

        conn = http.client.HTTPConnection(
            server.config.camera_host,
            server.config.camera_http_port,
            timeout=15,
        )

        headers = self._forward_headers()
        headers["Host"] = f"{server.config.camera_host}:{server.config.camera_http_port}"

        try:
            LOG.info("HTTP %s %s -> %s", self.command, raw_path, forward_path)
            conn.request(self.command, forward_path, body=body, headers=headers)
            response = conn.getresponse()
            response_body = response.read()

            self.send_response(response.status, response.reason)
            for key, value in response.getheaders():
                lower = key.lower()
                if lower in {"transfer-encoding", "connection", "keep-alive"}:
                    continue
                self.send_header(key, value)
            self.send_header("Content-Length", str(len(response_body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(response_body)
        except Exception as exc:  # pragma: no cover - operational path
            LOG.exception("HTTP proxy failure for %s %s", self.command, raw_path)
            payload = json.dumps(
                {
                    "error": "proxy_failure",
                    "detail": str(exc),
                    "camera": f"{server.config.camera_host}:{server.config.camera_http_port}",
                    "path": forward_path,
                }
            ).encode()
            self.send_response(502, "Bad Gateway")
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        finally:
            conn.close()

    def _normalize_path(self, path: str) -> str:
        parsed = urllib.parse.urlsplit(path)
        candidate = parsed.path or "/"
        if candidate.startswith("/camera/"):
            candidate = candidate[len("/camera") :]
        elif candidate == "/camera":
            candidate = "/"
        query = f"?{parsed.query}" if parsed.query else ""
        return f"{candidate}{query}"

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0") or "0")
        return self.rfile.read(length) if length else b""

    def _forward_headers(self) -> Dict[str, str]:
        headers: Dict[str, str] = {}
        for key, value in self.headers.items():
            lower = key.lower()
            if lower in {
                "host",
                "content-length",
                "connection",
                "proxy-connection",
                "keep-alive",
            }:
                continue
            headers[key] = value
        return headers

    def do_GET(self) -> None:  # noqa: N802
        self._proxy()

    def do_POST(self) -> None:  # noqa: N802
        self._proxy()

    def do_PUT(self) -> None:  # noqa: N802
        self._proxy()

    def do_DELETE(self) -> None:  # noqa: N802
        self._proxy()

    def do_PATCH(self) -> None:  # noqa: N802
        self._proxy()

    def do_HEAD(self) -> None:  # noqa: N802
        self._proxy()

    def log_message(self, fmt: str, *args: object) -> None:
        LOG.info("HTTP: " + fmt, *args)


class UDPForwarder(threading.Thread):
    def __init__(
        self,
        name: str,
        bind_host: str,
        bind_port: int,
        upstream_host: str,
        upstream_port: int,
    ) -> None:
        super().__init__(name=name, daemon=True)
        self.bind_host = bind_host
        self.bind_port = bind_port
        self.upstream_host = upstream_host
        self.upstream_port = upstream_port
        self._sock: Optional[socket.socket] = None
        self._stop = threading.Event()
        self.last_sender: Optional[Tuple[str, int]] = None
        self.packet_count = 0
        self.byte_count = 0

    def stop(self) -> None:
        self._stop.set()
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass

    def run(self) -> None:  # pragma: no cover - operational path
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self.bind_host, self.bind_port))
        sock.settimeout(1.0)
        self._sock = sock

        LOG.info(
            "UDP forwarder listening on %s:%d -> %s:%d",
            self.bind_host,
            self.bind_port,
            self.upstream_host,
            self.upstream_port,
        )

        while not self._stop.is_set():
            try:
                data, addr = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break

            self.last_sender = addr
            self.packet_count += 1
            self.byte_count += len(data)

            if not self.upstream_host or not self.upstream_port:
                continue

            try:
                sock.sendto(data, (self.upstream_host, self.upstream_port))
            except OSError:
                LOG.exception(
                    "UDP forward failure from %s:%d to %s:%d",
                    addr[0],
                    addr[1],
                    self.upstream_host,
                    self.upstream_port,
                )

        LOG.info("UDP forwarder on port %d stopped", self.bind_port)


class GardeProBridge:
    def __init__(self, config: BridgeConfig) -> None:
        self.config = config
        self.httpd = ThreadingHTTPServer(
            (config.http_bind_host, config.http_bind_port),
            GardeProProxyHandler,
        )
        self.httpd.bridge = self  # type: ignore[attr-defined]
        self.http_thread = threading.Thread(
            target=self.httpd.serve_forever,
            name="http-proxy",
            daemon=True,
        )
        self.udp_forwarders = [
            UDPForwarder(
                "udp-primary",
                config.udp_bind_host,
                config.udp_port_primary,
                config.upstream_media_host,
                config.upstream_media_port_primary,
            ),
            UDPForwarder(
                "udp-secondary",
                config.udp_bind_host,
                config.udp_port_secondary,
                config.upstream_media_host,
                config.upstream_media_port_secondary,
            ),
        ]
        self.started_at = time.time()

    def start(self) -> None:
        LOG.info("Bridge config: %s", json.dumps(asdict(self.config), indent=2))
        self.http_thread.start()
        for forwarder in self.udp_forwarders:
            forwarder.start()

    def stop(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()
        for forwarder in self.udp_forwarders:
            forwarder.stop()

    def status(self) -> Dict[str, object]:
        udp = []
        for forwarder in self.udp_forwarders:
            udp.append(
                {
                    "listen_port": forwarder.bind_port,
                    "upstream_host": forwarder.upstream_host,
                    "upstream_port": forwarder.upstream_port,
                    "last_sender": forwarder.last_sender,
                    "packet_count": forwarder.packet_count,
                    "byte_count": forwarder.byte_count,
                }
            )
        return {
            "uptime_sec": round(time.time() - self.started_at, 1),
            "camera_http": f"http://{self.config.camera_host}:{self.config.camera_http_port}",
            "http_proxy": f"http://{self.config.http_bind_host}:{self.config.http_bind_port}/camera/",
            "udp": udp,
        }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Minimal GardePro bridge skeleton")
    parser.add_argument("--camera-host", default="192.168.8.1")
    parser.add_argument("--camera-http-port", type=int, default=8080)
    parser.add_argument("--http-bind-host", default="0.0.0.0")
    parser.add_argument("--http-bind-port", type=int, default=18080)
    parser.add_argument("--udp-bind-host", default="0.0.0.0")
    parser.add_argument("--udp-port-primary", type=int, default=49152)
    parser.add_argument("--udp-port-secondary", type=int, default=49153)
    parser.add_argument("--upstream-media-host", default="")
    parser.add_argument("--upstream-media-port-primary", type=int, default=0)
    parser.add_argument("--upstream-media-port-secondary", type=int, default=0)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    bridge = GardeProBridge(
        BridgeConfig(
            camera_host=args.camera_host,
            camera_http_port=args.camera_http_port,
            http_bind_host=args.http_bind_host,
            http_bind_port=args.http_bind_port,
            udp_bind_host=args.udp_bind_host,
            udp_port_primary=args.udp_port_primary,
            udp_port_secondary=args.udp_port_secondary,
            upstream_media_host=args.upstream_media_host,
            upstream_media_port_primary=args.upstream_media_port_primary,
            upstream_media_port_secondary=args.upstream_media_port_secondary,
        )
    )
    bridge.start()

    print(json.dumps(bridge.status(), indent=2))

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        LOG.info("Shutting down bridge")
        bridge.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
