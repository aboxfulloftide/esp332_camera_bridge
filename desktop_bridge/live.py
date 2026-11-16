"""Live view: RTSP session with the camera, RTP/RTCP received on UDP and forwarded
to the server over the GPRT tunnel (firmware runRtspLiveSequence / stream pipeline).
"""

from __future__ import annotations

import re
import select
import socket
import threading
import time

import requests

from . import tunnel
from .radio import log, prt

RTSP_URL = "rtsp://192.168.8.1/live.sdp"
RTSP_PORT = 554


class RtspError(RuntimeError):
    pass


class RtspClient:
    def __init__(self, host: str, port: int = 554, timeout: float = 10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.cseq = 0
        self.buf = b""
        self.lock = threading.Lock()

    def request(self, method: str, url: str, headers: dict | None = None) -> tuple[int, dict, str]:
        with self.lock:
            self.cseq += 1
            lines = [f"{method} {url} RTSP/1.0", f"CSeq: {self.cseq}", "User-Agent: gardepro-desktop-bridge"]
            lines += [f"{k}: {v}" for k, v in (headers or {}).items()]
            self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
            while b"\r\n\r\n" not in self.buf:
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise RtspError(f"{method}: connection closed")
                self.buf += chunk
            head, _, rest = self.buf.partition(b"\r\n\r\n")
            text = head.decode(errors="replace").split("\r\n")
            m = re.match(r"RTSP/1\.\d (\d+)", text[0])
            if not m:
                raise RtspError(f"{method}: bad status line {text[0]!r}")
            hdrs = {}
            for line in text[1:]:
                k, _, v = line.partition(":")
                hdrs[k.strip().lower()] = v.strip()
            n = int(hdrs.get("content-length", "0") or 0)
            while len(rest) < n:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                rest += chunk
            body, self.buf = rest[:n], rest[n:]
            return int(m.group(1)), hdrs, body.decode(errors="replace")

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def _h264_has_idr(pkt: bytes) -> bool:
    """True if this RTP packet carries (part of) an H.264 IDR slice (NAL 5)."""
    if len(pkt) < 13:
        return False
    off = 12 + 4 * (pkt[0] & 0x0F)
    if pkt[0] & 0x10 and len(pkt) >= off + 4:                     # header extension
        off += 4 + 4 * int.from_bytes(pkt[off + 2:off + 4], "big")
    if len(pkt) <= off:
        return False
    nal = pkt[off] & 0x1F
    if nal == 5:
        return True
    if nal in (28, 29) and len(pkt) > off + 1:                     # FU-A / FU-B
        return (pkt[off + 1] & 0x1F) == 5
    if nal == 24:                                                  # STAP-A
        i = off + 1
        while i + 2 < len(pkt):
            size = int.from_bytes(pkt[i:i + 2], "big")
            if size and (pkt[i + 2] & 0x1F) == 5:
                return True
            i += 2 + size
    return False


class FrameGate:
    """Forward only complete H.264 frames; after any loss, wait for the next IDR.

    The camera's link loses ~0.5-1% of RTP packets in short bursts and sends an
    IDR every 4 s, so a single lost packet smears the picture until the next
    keyframe. Packets are held per frame (same RTP timestamp, ended by the marker
    bit or by the next timestamp) and released only when the frame arrived
    without a sequence gap. A damaged frame, and every frame after it up to the
    next IDR, is dropped: the viewer sees a short freeze instead of corruption.
    Forwarded packets are renumbered so the server sees a gap-free sequence.
    """

    def __init__(self) -> None:
        self.buf: list[bytes] = []
        self.ts: int | None = None
        self.damaged = False
        self.waiting_idr = True            # start cleanly on the first keyframe
        self.next_seq: int | None = None
        self.out_seq: int | None = None
        self.stats = {"frames_forwarded": 0, "frames_dropped_damaged": 0,
                      "frames_dropped_waiting_keyframe": 0, "packets_dropped": 0}

    def push(self, pkt: bytes) -> list[bytes]:
        if len(pkt) < 12:
            return []
        seq = int.from_bytes(pkt[2:4], "big")
        ts = int.from_bytes(pkt[4:8], "big")
        out: list[bytes] = []
        if self.next_seq is not None:
            delta = (seq - self.next_seq) & 0xFFFF
            if delta > 0x8000:                         # late / duplicate: its frame is already gone
                self.stats["packets_dropped"] += 1
                return []
            if delta:                                  # lost packets: they may belong to either frame
                self.damaged = True
                if self.buf and ts != self.ts:
                    out += self._finish()
                    self.damaged = True
        self.next_seq = (seq + 1) & 0xFFFF
        if self.buf and ts != self.ts:                 # previous frame's marker never came
            out += self._finish()
        self.ts = ts
        self.buf.append(pkt)
        if pkt[1] & 0x80:                              # marker: last packet of the frame
            out += self._finish()
        return out

    def _finish(self) -> list[bytes]:
        pkts, damaged = self.buf, self.damaged
        self.buf, self.damaged = [], False
        if not pkts:
            return []
        if damaged:
            self.waiting_idr = True
            self.stats["frames_dropped_damaged"] += 1
        elif self.waiting_idr and not any(_h264_has_idr(p) for p in pkts):
            self.stats["frames_dropped_waiting_keyframe"] += 1
        else:
            self.waiting_idr = False
            self.stats["frames_forwarded"] += 1
            out = []
            for p in pkts:
                if self.out_seq is None:
                    self.out_seq = int.from_bytes(p[2:4], "big")
                out.append(p[:2] + self.out_seq.to_bytes(2, "big") + p[4:])
                self.out_seq = (self.out_seq + 1) & 0xFFFF
            return out
        self.stats["packets_dropped"] += len(pkts)
        return []


def parse_sdp_controls(base: str, sdp: str, content_base: str | None) -> tuple[str, str]:
    """Return (aggregate_url, media_url) from an SDP (video track control)."""
    base = content_base or base
    aggregate, media, seen_media, in_video = base, "", False, False
    for line in sdp.splitlines():
        line = line.strip()
        if line.startswith("m="):
            seen_media, in_video = True, line.startswith("m=video")
        elif line.startswith("a=control:"):
            ctl = line[len("a=control:"):]
            url = ctl if ctl.startswith("rtsp://") else (base.rstrip("/") + "/" + ctl if ctl != "*" else base)
            if not seen_media:
                aggregate = url          # session-level control
            elif in_video and not media:
                media = url
    return aggregate, media or base


class LiveView:
    def __init__(self, cfg, on_stall=None):
        self.cfg = cfg
        self.on_stall = on_stall
        self.lock = threading.Lock()
        self.active = False
        self.rtsp: RtspClient | None = None
        self.session = ""
        self.play_url = ""
        self.sdp = ""
        self.udp: list[socket.socket] = []
        self.tun: socket.socket | None = None
        self.tun_lock = threading.Lock()
        self.stop_evt = threading.Event()
        self.threads: list[threading.Thread] = []
        self.session_id = 0
        self.stats = self._new_stats()
        self.gate: FrameGate | None = None
        self.totals = {"rtp_packets_received_total": 0, "rtp_packets_forwarded_total": 0, "stall_count": 0,
                       "sessions": 0}
        self.last_stage = "idle"
        self.last_message: str | None = None

    @staticmethod
    def _new_stats() -> dict:
        return {"rtp_packets_received": 0, "rtcp_packets_received": 0, "rtp_packets_forwarded": 0,
                "rtcp_packets_forwarded": 0, "rtp_bytes": 0, "tunnel_send_failures": 0,
                "tunnel_reconnects": 0, "rtp_sequence_gaps": 0, "rtp_packets_missing": 0, "rtp_out_of_order": 0,
                "last_rtp_sequence": None, "rtp_keyframes_observed": 0, "first_packet_at": None,
                "last_packet_at": None, "started_at": None, "rtsp_keepalive_count": 0,
                "rtsp_keepalive_failures": 0, "http_keepalive_failures": 0}

    # ------------------------------------------------------------------ tunnel
    def _start_metadata(self, ident: dict) -> dict:
        return {"type": "start", "codec": "H264", "camera_ip": prt.CAMERA_HOST,
                "halow_ip": ident["ip"], "halow_mac": ident["mac"], "halow_bssid": "", "halow_rssi": 0,
                "primary_port": self.cfg.media_port_primary, "secondary_port": self.cfg.media_port_secondary,
                "rtsp_session": self.session, "sdp": self.sdp.replace("\r", "")}

    def _tunnel_connect(self) -> bool:
        try:
            s = socket.create_connection((self.cfg.tunnel_host, self.cfg.tunnel_port), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(5)
            s.sendall(tunnel.control_frame(tunnel.CONTROL_START,
                                           self._start_metadata(tunnel.iface_identity(self.cfg.uplink_iface))))
            with self.tun_lock:
                self.tun = s
            return True
        except OSError as e:
            log(f"Live: tunnel connect failed: {e}")
            self.last_message = "camera_stream_active_tunnel_connect_failed"
            return False

    def _tunnel_close(self, send_stop: bool, reason: str) -> None:
        with self.tun_lock:
            s, self.tun = self.tun, None
        if s is None:
            return
        if send_stop:
            try:
                s.sendall(tunnel.control_frame(tunnel.CONTROL_STOP, {"type": "stop", "reason": reason,
                                                                      "rtsp_session": self.session}))
            except OSError:
                pass
        try:
            s.close()
        except OSError:
            pass

    def _forward(self, stream_id: int, payload: bytes) -> bool:
        with self.tun_lock:
            s = self.tun
            if s is None:
                return False
            try:
                s.sendall(tunnel.frame(stream_id, 0, payload))
                return True
            except OSError:
                self.stats["tunnel_send_failures"] += 1
                try:
                    s.close()
                except OSError:
                    pass
                self.tun = None
                return False

    # ------------------------------------------------------------------ start / stop
    def start(self) -> None:
        """Run DESCRIBE/SETUP/PLAY and start forwarding. Camera Wi-Fi must be up."""
        with self.lock:
            if self.active:
                return
            self.stats = self._new_stats()
            self.gate = FrameGate() if self.cfg.rtp_keyframe_gate else None
            self.stop_evt.clear()
            self.session_id += 1
            self.totals["sessions"] += 1
            try:
                self._open_udp()
                self.last_stage = "rtsp_describe"
                self.rtsp = RtspClient(prt.CAMERA_HOST, RTSP_PORT)
                code, hdrs, body = self.rtsp.request("DESCRIBE", RTSP_URL, {"Accept": "application/sdp"})
                if code != 200:
                    raise RtspError(f"rtsp_describe_status_{code}")
                self.sdp = body
                aggregate, media = parse_sdp_controls(RTSP_URL, body, hdrs.get("content-base"))
                self.last_stage = "rtsp_setup"
                code, hdrs, _ = self.rtsp.request("SETUP", media, {
                    "Transport": f"RTP/AVP;unicast;client_port={self.cfg.media_port_primary}-{self.cfg.media_port_secondary}"})
                if code != 200:
                    raise RtspError(f"rtsp_setup_status_{code}")
                self.session = hdrs.get("session", "").split(";")[0].strip()
                if not self.session:
                    raise RtspError("rtsp_setup_missing_session")
                # Tunnel before PLAY so the first SPS/PPS/IDR packets are not lost (firmware does the same).
                self.last_stage = "tunnel_connect_before_play"
                self._tunnel_connect()
                self.last_stage = "rtsp_play"
                play_hdrs = {"Session": self.session, "Range": "npt=0.000-"}
                code, _, _ = self.rtsp.request("PLAY", aggregate, play_hdrs)
                self.play_url = aggregate
                if code == 455 and media != aggregate:
                    code, _, _ = self.rtsp.request("PLAY", media, play_hdrs)
                    self.play_url = media
                if code != 200:
                    raise RtspError(f"rtsp_play_status_{code}")
            except (OSError, RtspError) as e:
                self.last_stage = "failed"
                self.last_message = str(e) if isinstance(e, RtspError) else f"rtsp_error: {e}"
                self._teardown(send_stop=True, reason="start_failed")
                raise RtspError(self.last_message) from e
            self.active = True
            self.last_stage = "streaming"
            self.last_message = "camera_stream_active" if self.tun else self.last_message
            self.stats["started_at"] = time.time()
            for target, name in ((self._rx_loop, "live-rx"), (self._keepalive_loop, "live-keepalive")):
                t = threading.Thread(target=target, daemon=True, name=name)
                t.start()
                self.threads.append(t)
            log(f"Live: streaming (session {self.session}, tunnel {'up' if self.tun else 'down'})")

    def stop(self, reason: str = "stop") -> None:
        with self.lock:
            if not self.active and self.rtsp is None:
                return
            self.stop_evt.set()
            self._teardown(send_stop=True, reason=reason)
            for t in self.threads:
                if t is not threading.current_thread():
                    t.join(timeout=5)
            self.threads = []
            self.active = False
            self.last_stage = "stopped"
            log(f"Live: stopped ({reason})")

    def _open_udp(self) -> None:
        self._close_udp()
        for port in (self.cfg.media_port_primary, self.cfg.media_port_secondary):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
            s.bind(("0.0.0.0", port))
            self.udp.append(s)

    def _close_udp(self) -> None:
        for s in self.udp:
            try:
                s.close()
            except OSError:
                pass
        self.udp = []

    def _teardown(self, send_stop: bool, reason: str) -> None:
        if self.rtsp is not None:
            if self.session and self.play_url:
                try:
                    self.rtsp.sock.settimeout(3)
                    self.rtsp.request("TEARDOWN", self.play_url, {"Session": self.session})
                except (OSError, RtspError):
                    pass
            self.rtsp.close()
            self.rtsp = None
        self._tunnel_close(send_stop, reason)
        self._close_udp()
        self.session = self.play_url = ""

    # ------------------------------------------------------------------ loops
    def _rx_loop(self) -> None:
        primary = self.udp[0] if self.udp else None
        while not self.stop_evt.is_set():
            try:
                ready, _, _ = select.select(self.udp, [], [], 0.5)
            except (OSError, ValueError):
                break
            for s in ready:
                try:
                    data = s.recv(2048)
                except OSError:
                    continue
                now = time.time()
                is_rtp = s is primary
                st = self.stats
                if st["first_packet_at"] is None:
                    st["first_packet_at"] = now
                st["last_packet_at"] = now
                if is_rtp:
                    st["rtp_packets_received"] += 1
                    st["rtp_bytes"] += len(data)
                    self.totals["rtp_packets_received_total"] += 1
                    self._track_rtp(data)
                else:
                    st["rtcp_packets_received"] += 1
                if self.tun is None and not self.stop_evt.is_set():
                    if self._tunnel_connect():
                        st["tunnel_reconnects"] += 1
                if not is_rtp:
                    if self._forward(tunnel.STREAM_RTCP, data):
                        st["rtcp_packets_forwarded"] += 1
                    continue
                for pkt in (self.gate.push(data) if self.gate else [data]):
                    if self._forward(tunnel.STREAM_RTP, pkt):
                        st["rtp_packets_forwarded"] += 1
                        self.totals["rtp_packets_forwarded_total"] += 1
            self._check_stall()

    def _track_rtp(self, data: bytes) -> None:
        if len(data) < 13:
            return
        seq = int.from_bytes(data[2:4], "big")
        last = self.stats["last_rtp_sequence"]
        if last is not None:
            delta = (seq - last) & 0xFFFF
            if delta == 0 or delta > 0x8000:
                self.stats["rtp_out_of_order"] += 1
                return
            if delta > 1:
                self.stats["rtp_sequence_gaps"] += 1
                self.stats["rtp_packets_missing"] += delta - 1
        self.stats["last_rtp_sequence"] = seq
        cc = data[0] & 0x0F
        off = 12 + 4 * cc
        if len(data) > off:
            nal = data[off] & 0x1F
            if nal == 5 or (nal == 28 and len(data) > off + 1 and (data[off + 1] & 0x1F) == 5
                            and data[off + 1] & 0x80):
                self.stats["rtp_keyframes_observed"] += 1

    def _check_stall(self) -> None:
        st, now = self.stats, time.time()
        if st["started_at"] is None:
            return
        if st["last_packet_at"] is None:
            stalled = now - st["started_at"] > self.cfg.rtp_first_packet_s
        else:
            stalled = now - st["last_packet_at"] > self.cfg.rtp_stall_s
        if stalled and not self.stop_evt.is_set():
            self.totals["stall_count"] += 1
            self.last_message = "stream_rtp_stalled"
            log("Live: RTP stalled; stopping session")
            threading.Thread(target=self._stall_stop, daemon=True).start()
            self.stop_evt.set()

    def _stall_stop(self) -> None:
        self.stop("rtp_stalled")
        self.last_stage = "rtp_stalled"
        self.last_message = "stream_rtp_stalled"
        if self.on_stall:
            self.on_stall()

    def _keepalive_loop(self) -> None:
        cam_ka_at = time.time() + prt.HTTP_KEEPALIVE_S
        while not self.stop_evt.wait(self.cfg.rtsp_keepalive_s):
            ok = False
            for method in ("OPTIONS", "GET_PARAMETER"):   # firmware order
                try:
                    if self.rtsp is None:
                        break
                    code, _, _ = self.rtsp.request(method, self.play_url or RTSP_URL, {"Session": self.session})
                    if code == 200:
                        ok = True
                        break
                except (OSError, RtspError):
                    pass
            self.stats["rtsp_keepalive_count" if ok else "rtsp_keepalive_failures"] += 1
            if time.time() >= cam_ka_at:
                cam_ka_at = time.time() + prt.HTTP_KEEPALIVE_S
                try:
                    requests.get(f"http://{prt.CAMERA_HOST}:{prt.CAMERA_PORT}/cmd/standby/reset", timeout=(3, 5))
                except requests.RequestException:
                    self.stats["http_keepalive_failures"] += 1

    # ------------------------------------------------------------------ status
    def status(self) -> dict:
        st, now = self.stats, time.time()
        recv, fwd, missing = st["rtp_packets_received"], st["rtp_packets_forwarded"], st["rtp_packets_missing"]
        loss = round(100.0 * missing / (recv + missing), 2) if recv + missing else 0.0
        last_age = int((now - st["last_packet_at"]) * 1000) if st["last_packet_at"] else None
        receiving = self.active and last_age is not None and last_age < 3000
        stalled = self.last_message == "stream_rtp_stalled" and not self.active
        return {
            "stream_active": self.active,
            "tunnel_connected": self.active and self.tun is not None,
            "stream_status": {
                "stream_session_id": self.session_id,
                "stage": self.last_stage, "message": self.last_message,
                "rtp_receiving": receiving,
                "rtp_flowing": receiving and self.tun is not None and loss <= 5.0,
                "rtp_quality_ok": loss <= 5.0, "rtp_quality_limit_pct": 5.0,
                "rtp_stalled": stalled, "stall_fail_ms": int(self.cfg.rtp_stall_s * 1000),
                "stall_count": self.totals["stall_count"],
                "rtp_packets_received": recv, "rtp_packets_forwarded": fwd,
                "rtcp_packets_received": st["rtcp_packets_received"],
                "rtp_sequence_gaps": st["rtp_sequence_gaps"], "rtp_packets_missing": missing,
                "rtp_out_of_order": st["rtp_out_of_order"], "rtp_forward_failures": st["tunnel_send_failures"],
                "tunnel_packets_sent": fwd + st["rtcp_packets_forwarded"],
                "tunnel_reconnects": st["tunnel_reconnects"],
                "last_rtp_sequence": st["last_rtp_sequence"], "rtp_loss_pct": loss,
                "rtp_keyframe_seen": st["rtp_keyframes_observed"] > 0,
                "rtp_keyframes_observed": st["rtp_keyframes_observed"],
                "udp_primary": {"port": self.cfg.media_port_primary, "last_packet_age_ms": last_age},
                "rtp_keyframe_gate": self.gate is not None,
                **(self.gate.stats if self.gate else {}),
                "rtsp_session": self.session, "rtsp_keepalive_count": st["rtsp_keepalive_count"],
                "rtsp_keepalive_failures": st["rtsp_keepalive_failures"],
                **self.totals,
            },
            "media_primary_packets": recv,
            "media_primary_bytes": st["rtp_bytes"],
            "media_secondary_packets": st["rtcp_packets_received"],
            "http_keepalive_failures": st["http_keepalive_failures"],
            "sdp": self.sdp if self.active else None,
        }
