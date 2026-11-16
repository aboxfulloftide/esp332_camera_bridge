"""End-to-end tests for desktop_bridge against a fake camera, a fake upload server
and the real GPRT tunnel server (gardepro_tunnel_server.py). No radios needed:
the NetworkManager / iw helpers in tools/pi_radio_test.py are patched.

Run from esp332_camera_bridge/:  .venv/bin/python -m unittest tests.test_desktop_bridge -v
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from desktop_bridge import live as live_mod  # noqa: E402
from desktop_bridge import server as server_mod  # noqa: E402
from desktop_bridge import tunnel  # noqa: E402
from desktop_bridge.__main__ import Bridge  # noqa: E402
from desktop_bridge.config import Config  # noqa: E402
from desktop_bridge.radio import prt  # noqa: E402
from desktop_bridge.server import BridgeHTTPServer  # noqa: E402
from gardepro_tunnel_server import GPRTTunnelServer, TunnelConfig  # noqa: E402


def free_port(kind=socket.SOCK_STREAM) -> int:
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def jpeg(n: int, seed: int) -> bytes:
    body = hashlib.sha256(str(seed).encode()).digest() * (n // 32 + 1)
    return b"\xff\xd8" + body[:n] + b"\xff\xd9"


# ---------------------------------------------------------------------- fake camera + upload server

class FakeWorld:
    def __init__(self):
        self.items: dict[int, dict] = {}
        self.blobs: dict[int, bytes] = {}
        self.uploads: dict[int, dict] = {}
        self.uploaded_blobs: dict[int, bytes] = {}
        self.camera_connections: list[dict] = []
        self.standby_now = 0
        self.lock = threading.Lock()
        for i in range(1, 8):
            self.add_photo(i)

    def add_photo(self, i: int | None = None) -> int:
        with self.lock:
            i = i or (max(self.items, default=0) + 1)
            data = jpeg(3000 + i * 10, i)
            self.blobs[i] = data
            self.items[i] = {"id": i, "type": 1, "size": len(data), "date": f"2026-11-02 10:00:{i:02d}",
                             "uid": f"u{i}"}
            return i


def make_handler(world: FakeWorld):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def _send(self, code, payload, ctype="application/json"):
            data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            p = self.path.split("?")[0]
            if m := re.fullmatch(r"/list/detail/backward/(\d+)/(\d+)", p):
                cur, n = int(m.group(1)), int(m.group(2))
                ids = sorted((i for i in world.items if i < cur), reverse=True)[:n]
                return self._send(200, {"code": 0, "data": [world.items[i] for i in ids]})
            if m := re.fullmatch(r"/file/(\d+)/(JPG|mp4)", p):
                i = int(m.group(1))
                if i not in world.items:
                    return self._send(404, {"code": 1})
                return self._send(200, world.blobs[i], "image/jpeg")
            if m := re.fullmatch(r"/cmd/delete/(\d+)/(JPG|mp4)", p):
                world.items.pop(int(m.group(1)), None)
                return self._send(200, {"code": 0})
            if p == "/cmd/info/3":
                return self._send(200, {"code": 0, "data": {"photo": len(world.items), "video": 0}})
            if p.startswith("/cmd/info/") or p == "/cmd/getParaSetting":
                return self._send(200, {"code": 0, "data": {"path": p}})
            if p == "/cmd/standby/reset":
                return self._send(200, {"code": 0})
            if p == "/cmd/standby/now":
                world.standby_now += 1
                return self._send(200, {"code": 0})
            if p == "/media/pic/take":
                world.add_photo()
                return self._send(200, {"code": 0})
            if p == "/media/pic/result":
                return self._send(200, {"code": 0, "data": {"fileIdx": 0}})   # camera quirk: 0 on success
            return self._send(404, {"code": 404})

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n)
            if self.path.endswith("/api/board/media"):
                meta = json.loads(body)
                mid = len(world.uploads) + 1
                world.uploads[mid] = meta
                return self._send(200, {"ok": True, "media_id": mid,
                                        "upload_url": f"/trail_cam/api/board/media/{mid}/blob"})
            if self.path.endswith("/api/board/camera-connections"):
                req = json.loads(body)
                for row in req["connections"]:
                    world.camera_connections.append(dict(row, scanner_host=req["scanner_host"]))
                return self._send(200, {"ok": True, "saved": len(req["connections"]), "skipped": 0})
            return self._send(200, {"echo": body.decode(errors="replace"), "path": self.path})

        def do_PUT(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n)
            mid = int(self.path.split("/")[-2])
            world.uploaded_blobs[mid] = body
            ok = hashlib.sha256(body).hexdigest() == self.headers.get("X-SHA256")
            return self._send(200, {"ok": ok, "bytes": len(body)})

    return H


class FakeRtsp(threading.Thread):
    """Answers DESCRIBE/SETUP/PLAY/OPTIONS/TEARDOWN and sends RTP to client_port after PLAY."""

    SDP = "v=0\r\ns=cam\r\na=control:*\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:track1\r\n"

    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(2)
        self.port = self.sock.getsockname()[1]
        self.methods: list[str] = []
        self.sending = threading.Event()

    def run(self):
        conn, _ = self.sock.accept()
        buf, client_port = b"", None
        while True:
            try:
                data = conn.recv(4096)
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\r\n\r\n" in buf:
                req, _, buf = buf.partition(b"\r\n\r\n")
                lines = req.decode().split("\r\n")
                method = lines[0].split()[0]
                self.methods.append(method)
                cseq = next(l.split(":")[1].strip() for l in lines if l.lower().startswith("cseq"))
                hdrs, body = [f"CSeq: {cseq}"], ""
                if method == "DESCRIBE":
                    body = self.SDP
                    hdrs += ["Content-Base: rtsp://127.0.0.1/live.sdp/", "Content-Type: application/sdp"]
                elif method == "SETUP":
                    tr = next(l for l in lines if l.lower().startswith("transport"))
                    client_port = int(re.search(r"client_port=(\d+)", tr).group(1))
                    hdrs.append("Session: 12345678;timeout=60")
                elif method == "PLAY":
                    threading.Thread(target=self._send_rtp, args=(client_port,), daemon=True).start()
                elif method == "TEARDOWN":
                    self.sending.clear()
                resp = "RTSP/1.0 200 OK\r\n" + "\r\n".join(hdrs) + f"\r\nContent-Length: {len(body)}\r\n\r\n" + body
                conn.sendall(resp.encode())

    def _send_rtp(self, port):
        self.sending.set()
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        seq = 0
        while self.sending.is_set() and seq < 400:
            frame = seq // 5                                   # 5 packets per frame, IDR every 10 frames
            pkt = rtp(seq, frame * 24000, marker=seq % 5 == 4, nal=0x65 if frame % 10 == 0 else 0x41)
            s.sendto(pkt, ("127.0.0.1", port))
            seq += 1
            time.sleep(0.005)


def rtp(seq, ts, marker=False, nal=0x41, size=200):
    return (bytes([0x80, 96 | (0x80 if marker else 0)]) + (seq & 0xFFFF).to_bytes(2, "big")
            + ts.to_bytes(4, "big") + b"\x00" * 4 + bytes([nal]) + b"x" * size)


def frames(n, start_seq=0, idr_every=10, per_frame=5):
    """n frames as RTP packets; frame 0 (and every idr_every-th) is an IDR."""
    seq, out = start_seq, []
    for f in range(n):
        nal = 0x65 if f % idr_every == 0 else 0x41
        for i in range(per_frame):
            out.append(rtp(seq, f * 24000, marker=i == per_frame - 1, nal=nal))
            seq += 1
    return out


class FrameGateTests(unittest.TestCase):
    def run_gate(self, pkts):
        g = live_mod.FrameGate()
        out = [p for pk in pkts for p in g.push(pk)]
        return g, out

    @staticmethod
    def ts_of(pkts):
        return sorted({int.from_bytes(p[4:8], "big") // 24000 for p in pkts})

    def test_clean_stream_passes_unchanged(self):
        pkts = frames(30)
        g, out = self.run_gate(pkts)
        self.assertEqual(out, pkts)
        self.assertEqual(g.stats["frames_forwarded"], 30)

    def test_waits_for_first_idr(self):
        g, out = self.run_gate(frames(30)[15:])            # join mid-GOP at frame 3
        self.assertEqual(self.ts_of(out), list(range(10, 30)))
        self.assertEqual(g.stats["frames_dropped_waiting_keyframe"], 7)

    def test_loss_drops_until_next_idr_and_renumbers(self):
        pkts = frames(30)
        del pkts[22]                                       # frame 4 loses a middle packet
        g, out = self.run_gate(pkts)
        self.assertEqual(self.ts_of(out), [0, 1, 2, 3] + list(range(10, 30)))
        self.assertEqual(g.stats["frames_dropped_damaged"], 1)
        self.assertEqual(g.stats["frames_dropped_waiting_keyframe"], 5)
        seqs = [int.from_bytes(p[2:4], "big") for p in out]
        self.assertEqual(seqs, list(range(len(out))))      # gap-free for the server

    def test_lost_marker_packet_damages_frame(self):
        pkts = frames(30)
        del pkts[24]                                       # last packet (marker) of frame 4
        g, out = self.run_gate(pkts)
        self.assertNotIn(4, self.ts_of(out))
        self.assertNotIn(5, self.ts_of(out))
        self.assertIn(10, self.ts_of(out))

    def test_seq_wraparound(self):
        pkts = frames(30, start_seq=0xFFF0)
        g, out = self.run_gate(pkts)
        self.assertEqual(len(out), len(pkts))
        self.assertEqual(g.stats["frames_dropped_damaged"], 0)

    def test_idr_detected_in_fu_a_and_stap_a(self):
        fu_a = bytes([0x80, 96]) + b"\x00\x01" + b"\x00" * 8 + bytes([0x7C, 0x85]) + b"x"
        stap = bytes([0x80, 96]) + b"\x00\x01" + b"\x00" * 8 + bytes([0x78, 0, 2, 0x67, 0, 0, 2, 0x65, 0])
        self.assertTrue(live_mod._h264_has_idr(fu_a))
        self.assertTrue(live_mod._h264_has_idr(stap))
        self.assertFalse(live_mod._h264_has_idr(rtp(0, 0, nal=0x41)))


# ---------------------------------------------------------------------- test case

class DesktopBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        cls.world = FakeWorld()
        cls.fake = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(cls.world))
        threading.Thread(target=cls.fake.serve_forever, daemon=True).start()
        fport = cls.fake.server_address[1]

        # Point the camera helpers at the fake camera and stub the radio/OS helpers.
        cls._saved = {k: getattr(prt, k) for k in ("CAMERA_HOST", "CAMERA_PORT", "link_info", "iface_state",
                                                   "run", "scan_for_ssid", "uplink_gateway")}
        prt.CAMERA_HOST, prt.CAMERA_PORT = "127.0.0.1", fport
        prt.link_info = lambda iface: {"connected": True, "signal_dbm": -50}
        prt.iface_state = lambda iface: {"GENERAL.STATE": "100 (connected)", "GENERAL.CONNECTION": prt.CAMERA_NM_PROFILE,
                                         "IP4.ADDRESS[1]": "192.168.8.30/24"}
        prt.run = lambda cmd, timeout=30, check=False: subprocess.CompletedProcess(cmd, 0, "", "")
        prt.scan_for_ssid = lambda iface, ssid: {"signal_pct": 90, "channel": "1", "freq": "2412 MHz"}
        prt.uplink_gateway = lambda iface: None
        server_mod.CAMERA_BASE = f"http://127.0.0.1:{fport}"

        # Real tunnel server
        cls.tun_port = free_port()
        cls.tunnel = GPRTTunnelServer(TunnelConfig(
            bind_host="127.0.0.1", bind_port=cls.tun_port, udp_port_primary=free_port(socket.SOCK_DGRAM),
            udp_port_secondary=free_port(socket.SOCK_DGRAM), sdp_path=str(tmp / "live.sdp"),
            registration_path=str(tmp / "registration.json")))
        threading.Thread(target=cls.tunnel.serve_forever, daemon=True).start()

        # Fake RTSP
        cls.rtsp = FakeRtsp()
        cls.rtsp.start()
        live_mod.RTSP_URL = "rtsp://127.0.0.1/live.sdp"
        live_mod.RTSP_PORT = cls.rtsp.port

        cfg = Config(http_host="127.0.0.1", http_port=0, uplink_iface="lo", camera="e6_original",
                     server=f"http://127.0.0.1:{fport}/trail_cam", tunnel_host="127.0.0.1", tunnel_port=cls.tun_port,
                     register=False, staging_dir=str(tmp / "staging"), state_path=str(tmp / "staging" / "state.json"),
                     upload_sweep_s=1, media_port_primary=free_port(socket.SOCK_DGRAM),
                     media_port_secondary=free_port(socket.SOCK_DGRAM), rtp_stall_s=3, rtp_first_packet_s=5)
        cls.cfg = cfg
        cls.bridge = Bridge(cfg)
        cls.httpd = BridgeHTTPServer(("127.0.0.1", 0), cls.bridge)
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.httpd.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        cls.bridge.live.stop("test_done")
        cls.bridge.jobs.stop_evt.set()
        cls.httpd.shutdown()
        cls.fake.shutdown()
        cls.tunnel.stop()
        for k, v in cls._saved.items():
            setattr(prt, k, v)
        cls.tmp.cleanup()

    def get(self, path, **kw):
        return requests.get(self.base + path, timeout=30, **kw)

    def post(self, path, **kw):
        return requests.post(self.base + path, timeout=30, **kw)

    def wait_control(self, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            s = self.get("/control/summary").json()
            if not s["control_busy"]:
                return s
            time.sleep(0.2)
        self.fail("control action did not finish")

    # ------------------------------------------------------------------ tests
    def test_01_status_and_target(self):
        s = self.get("/status").json()
        self.assertTrue(s["ok"])
        self.assertEqual(s["camera_target_id"], "e6_original")
        t = self.get("/camera/target").json()["target"]
        self.assertEqual(t["active"]["wifi_ssid"], "CAM8Z8_A4C138982635")
        self.assertEqual(self.post("/camera/target?id=nope").status_code, 400)
        r = self.post("/camera/target", json={"id": "e6_plus"})
        self.assertEqual(r.json()["target"]["active"]["wifi_ssid"], "CAM8Z8_A4C138988148")
        self.post("/camera/target?id=e6_original")
        self.assertEqual(self.get("/healthz").status_code, 200)

    def test_02_camera_proxy(self):
        self.assertEqual(self.get("/camera/info/3").json()["data"]["photo"], len(self.world.items))
        g = self.get("/camera/gallery").json()["data"]
        self.assertEqual(len(g), len(self.world.items))
        raw = self.get("/camera/raw", params={"path": "/file/3/JPG"})
        self.assertEqual(raw.content, self.world.blobs[3])
        req = self.get("/camera/request", params={"path": "/cmd/info/2"}).json()
        self.assertEqual(req["data"]["path"], "/cmd/info/2")
        posted = self.post("/camera/request?path=/cmd/setting&method=POST", data=b'{"a":1}').json()
        self.assertEqual(posted["echo"], '{"a":1}')
        latest = self.get("/camera/latest?type=1").json()
        self.assertEqual(latest["data"]["id"], max(self.world.items))
        self.assertEqual(self.get("/camera/request").status_code, 400)

    def test_02b_proxied_standby_now(self):
        # The server's close_session sends /cmd/standby/now after stream_stop; the
        # bridge has already dropped the camera link by then, which must not be an error.
        saved = prt.link_info
        prt.link_info = lambda iface: {"connected": False}
        try:
            r = self.get("/camera/request", params={"path": "/cmd/standby/now"})
            self.assertEqual((r.status_code, r.json()["desc"]), (200, "already_standby"))
            self.assertEqual(self.get("/camera/request", params={"path": "/cmd/info/2"}).status_code, 503)
        finally:
            prt.link_info = saved
        self.post("/session/lease?ttl_ms=60000")
        try:
            r = self.get("/camera/request", params={"path": "/cmd/standby/now"})
            self.assertEqual(r.json()["desc"], "standby_deferred_by_lease")
        finally:
            self.post("/session/release?standby=0")
        n = self.world.standby_now
        r = self.get("/camera/request", params={"path": "/cmd/standby/now"})
        self.assertEqual((r.status_code, r.json()["desc"]), (200, "standby"))
        self.assertEqual(self.world.standby_now, n + 1)

    def test_03_trigger_photo(self):
        before = set(self.world.items)
        r = self.post("/jobs?action=trigger_photo")
        self.assertEqual(r.status_code, 202, r.text)
        s = self.wait_control()
        self.assertTrue(s["control_last_ok"], s)
        new = set(self.world.items) - before
        self.assertEqual(len(new), 1)
        self.assertEqual(s["last_photo"]["media_id"], new.pop())

    def test_04_bringup(self):
        self.assertEqual(self.post("/control/bringup").status_code, 202)
        s = self.wait_control()
        self.assertTrue(s["control_last_ok"])
        self.assertEqual(s["control_message"], "bringup_complete")

    def test_05_durable_job_download_delete_upload(self):
        n_items = len(self.world.items)
        blobs = dict(self.world.blobs)
        r = self.post("/jobs?action=download_and_delete_media")
        self.assertEqual(r.status_code, 202, r.text)
        self.assertEqual(self.post("/jobs?action=download_all_media").status_code, 409)   # one job at a time
        self.assertEqual(self.post("/control/stream_start").status_code, 409)              # camera owned by job
        end, job = time.time() + 90, {}
        while time.time() < end:
            job = self.get("/trail-media/status").json()["job"]
            if job["status"] in ("succeeded", "failed"):
                break
            time.sleep(0.3)
        self.assertEqual(job["status"], "succeeded", job)
        self.assertEqual(job["total"], n_items)
        self.assertEqual(job["deleted"], n_items)
        self.assertEqual(job["uploaded"], n_items)
        self.assertEqual(self.world.items, {})
        # Every upload is byte-identical to the camera original and labelled with the camera.
        by_name = {m["filename"]: (mid, m) for mid, m in self.world.uploads.items()}
        for i, data in blobs.items():
            name = next(n for n in by_name if n.endswith(f"_{i}.jpg"))
            mid, meta = by_name[name]
            self.assertEqual(self.world.uploaded_blobs[mid], data)
            self.assertEqual(meta["metadata"]["camera"], "GardePro E6")
            self.assertEqual(meta["metadata"]["bridge"], "desktop")
        # One camera connection record for the job, sent before the media upload.
        conns = self.world.camera_connections
        self.assertEqual(len(conns), 1, conns)
        c = conns[0]
        self.assertTrue(c["ok"])
        self.assertIsNone(c["failure_stage"])
        self.assertEqual(c["camera"], "e6_original")
        self.assertEqual(c["scanner_host"], "trail_desktop")
        self.assertEqual(c["wifi_rssi_dbm"], -50)
        self.assertEqual(c["wake_method"], "none")       # fake hotspot is always visible: no BLE wake
        self.assertIsNone(c["ble_rssi_dbm"])
        self.assertEqual(c["items_downloaded"], n_items)
        self.assertTrue(c["started_at"].endswith("Z") and c["connected_at"] and c["ended_at"])
        self.assertFalse(any(k.startswith("_") for k in c))
        files = self.get("/trail-media/files").json()["items"]
        self.assertEqual(len(files), n_items)
        self.assertEqual(self.get(files[0]["url"]).content[:2], b"\xff\xd8")
        self.assertEqual(self.get("/trail-media/file/../../etc/passwd").status_code, 404)

    def test_06_live_view_through_tunnel(self):
        r = self.post("/control/stream_start")
        self.assertEqual(r.status_code, 202, r.text)
        s = self.wait_control()
        self.assertTrue(s["control_last_ok"], s)
        self.assertTrue(s["live_view_active"])
        end = time.time() + 10
        while time.time() < end and self.tunnel.stats.rtp_packets < 100:
            time.sleep(0.2)
        st = self.get("/stream/status").json()
        self.assertGreaterEqual(self.tunnel.stats.rtp_packets, 100, st)
        self.assertIn("H264", self.tunnel.stats.last_start_metadata["sdp"])
        self.assertTrue(st["stream_status"]["rtp_keyframe_seen"])
        self.assertEqual(st["stream_status"]["rtp_packets_missing"], 0)
        self.assertEqual(self.post("/control/stream_stop").status_code, 202)
        s = self.wait_control()
        self.assertFalse(s["live_view_active"])
        self.assertIn("TEARDOWN", self.rtsp.methods)
        end = time.time() + 5
        while time.time() < end and self.tunnel.stats.last_stop_metadata is None:
            time.sleep(0.1)
        self.assertEqual(self.tunnel.stats.last_stop_metadata["reason"], "stream_stop")

    def test_07_register_frame(self):
        reg = tunnel.Registrar(self.cfg, lambda m: None)
        self.assertTrue(reg.send_once())
        path = Path(self.tunnel.config.registration_path)
        end = time.time() + 5
        while time.time() < end and not path.exists():
            time.sleep(0.1)
        md = json.loads(path.read_text())
        self.assertEqual(md["type"], "register")
        self.assertEqual(md["halow_ip"], "127.0.0.1")
        self.assertEqual(md["hostname"], "trail_desktop")


if __name__ == "__main__":
    unittest.main()
