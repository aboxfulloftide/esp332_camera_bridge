"""Direct LE connection to a public BLE address without seeing an advertisement.

The E6+ can advertise so rarely that a scan never catches it (see the
direct-connect fallback in gardepro_dual_radio_bridge.ino). The ESP32 handles
this by letting its controller initiate a connection straight to the camera's
public address. This module does the same on Linux: it opens a kernel L2CAP
ATT socket (CID 4) to the address, which makes the controller initiate the
connection. While the socket is open, BlueZ registers the device, and bleak can
attach to it through its D-Bus path for GATT access.

No BlueZ configuration change is needed. Python's socket module cannot express
LE socket addresses, so connect/bind go through libc via ctypes.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import errno
import os
import socket
import time

AF_BLUETOOTH = 31
BTPROTO_L2CAP = 0
ATT_CID = 4
BDADDR_LE_PUBLIC = 1
SOL_BLUETOOTH = 274
BT_SECURITY = 4

_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)


class _sockaddr_l2(ctypes.Structure):
    _fields_ = [
        ("l2_family", ctypes.c_ushort),
        ("l2_psm", ctypes.c_ushort),
        ("l2_bdaddr", ctypes.c_ubyte * 6),
        ("l2_cid", ctypes.c_ushort),
        ("l2_bdaddr_type", ctypes.c_ubyte),
    ]


def _addr(mac: str, addr_type: int) -> _sockaddr_l2:
    sa = _sockaddr_l2()
    sa.l2_family = AF_BLUETOOTH
    sa.l2_psm = 0
    sa.l2_bdaddr[:] = list(reversed(bytes.fromhex(mac.replace(":", ""))))
    sa.l2_cid = ATT_CID
    sa.l2_bdaddr_type = addr_type
    return sa


def adapter_address(adapter: str) -> str:
    with open(f"/sys/class/bluetooth/{adapter}/address") as f:  # noqa: PTH123
        return f.read().strip().upper()


def _sysfs_address(adapter: str) -> str | None:
    try:
        return adapter_address(adapter)
    except OSError:
        return None


def direct_connect(mac: str, adapter: str = "hci1", timeout: float = 40.0) -> socket.socket:
    """Open an LE ATT socket to `mac` (public address) from `adapter`.

    Returns the connected socket; keep it open to hold the link. Raises
    TimeoutError/OSError if the controller could not connect.
    """
    local = _sysfs_address(adapter)
    if local is None:
        # /sys/class/bluetooth/<hci>/address is not always present; ask hciconfig.
        import subprocess

        out = subprocess.run(["hciconfig", adapter], capture_output=True, text=True).stdout
        local = out.split("BD Address:")[1].split()[0] if "BD Address:" in out else None
    if local is None:
        raise OSError(f"cannot determine address of {adapter}")

    sock = socket.socket(AF_BLUETOOTH, socket.SOCK_SEQPACKET, BTPROTO_L2CAP)
    fd = sock.fileno()
    la = _addr(local, BDADDR_LE_PUBLIC)
    if _libc.bind(fd, ctypes.byref(la), ctypes.sizeof(la)) != 0:
        e = ctypes.get_errno()
        sock.close()
        raise OSError(e, f"bind to {adapter} ({local}) failed: {os.strerror(e)}")
    ra = _addr(mac, BDADDR_LE_PUBLIC)
    sock.settimeout(None)
    t0 = time.time()
    # Blocking connect: the kernel initiates the LE connection and returns when
    # the link is up or the controller's connection attempt times out.
    rc = _libc.connect(fd, ctypes.byref(ra), ctypes.sizeof(ra))
    if rc != 0:
        e = ctypes.get_errno()
        sock.close()
        if e in (errno.ETIMEDOUT, errno.ECONNREFUSED, errno.EHOSTDOWN, errno.EINPROGRESS, errno.EBUSY,
                 errno.EALREADY):
            raise TimeoutError(f"direct connect to {mac} failed after {time.time() - t0:.0f}s: {os.strerror(e)}")
        raise OSError(e, f"direct connect to {mac} failed: {os.strerror(e)}")
    return sock


def bluez_device_path(mac: str, adapter: str) -> str:
    return f"/org/bluez/{adapter}/dev_{mac.upper().replace(':', '_')}"


# --------------------------------------------------------------------------- minimal ATT client

ATT_ERROR_RSP = 0x01
ATT_FIND_INFO_REQ, ATT_FIND_INFO_RSP = 0x04, 0x05
ATT_READ_BY_TYPE_REQ, ATT_READ_BY_TYPE_RSP = 0x08, 0x09
ATT_WRITE_REQ, ATT_WRITE_RSP = 0x12, 0x13
ATT_HANDLE_VALUE_NTF, ATT_HANDLE_VALUE_IND, ATT_HANDLE_VALUE_CFM = 0x1B, 0x1D, 0x1E
GATT_CHAR_DECL = 0x2803
GATT_CCCD = 0x2902


def _uuid_from_le(b: bytes) -> str:
    if len(b) == 2:
        return f"0000{int.from_bytes(b, 'little'):04x}-0000-1000-8000-00805f9b34fb"
    h = b[::-1].hex()
    return f"{h[:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:]}"


class ATTClient:
    """Just enough ATT to discover characteristics, enable notify and write."""

    def __init__(self, sock: socket.socket, log=print):
        self.sock = sock
        self.log = log
        self.notifications: list[tuple[int, bytes]] = []

    def _recv(self, timeout: float) -> bytes | None:
        self.sock.settimeout(timeout)
        try:
            return self.sock.recv(1024)
        except (TimeoutError, socket.timeout):
            return None

    def _request(self, pdu: bytes, expect: int, timeout: float = 10.0) -> bytes:
        self.sock.send(pdu)
        end = time.time() + timeout
        while time.time() < end:
            rsp = self._recv(max(0.1, end - time.time()))
            if rsp is None:
                break
            op = rsp[0]
            if op == ATT_HANDLE_VALUE_NTF:
                self.notifications.append((int.from_bytes(rsp[1:3], "little"), rsp[3:]))
                continue
            if op == ATT_HANDLE_VALUE_IND:
                self.notifications.append((int.from_bytes(rsp[1:3], "little"), rsp[3:]))
                self.sock.send(bytes([ATT_HANDLE_VALUE_CFM]))
                continue
            if op == expect or op == ATT_ERROR_RSP:
                return rsp
            # Peer-initiated request we do not serve: reply "request not supported".
            if op & 0x01 == 0 and op not in (ATT_WRITE_RSP,):
                self.sock.send(bytes([ATT_ERROR_RSP, op, 0, 0, 0x06]))
        raise TimeoutError(f"ATT 0x{pdu[0]:02x}: no response")

    def characteristics(self) -> list[dict]:
        chars, start = [], 0x0001
        while start <= 0xFFFF:
            rsp = self._request(bytes([ATT_READ_BY_TYPE_REQ]) + start.to_bytes(2, "little")
                                + (0xFFFF).to_bytes(2, "little") + GATT_CHAR_DECL.to_bytes(2, "little"),
                                ATT_READ_BY_TYPE_RSP)
            if rsp[0] == ATT_ERROR_RSP:
                break
            n = rsp[1]
            last = start
            for i in range(2, len(rsp), n):
                rec = rsp[i:i + n]
                decl = int.from_bytes(rec[0:2], "little")
                chars.append({"decl": decl, "props": rec[2], "handle": int.from_bytes(rec[3:5], "little"),
                              "uuid": _uuid_from_le(rec[5:])})
                last = decl
            start = last + 1
        for i, c in enumerate(chars):
            c["end"] = chars[i + 1]["decl"] - 1 if i + 1 < len(chars) else 0xFFFF
        return chars

    def find_cccd(self, char: dict) -> int | None:
        start, end = char["handle"] + 1, char["end"]
        while start <= end:
            rsp = self._request(bytes([ATT_FIND_INFO_REQ]) + start.to_bytes(2, "little") + end.to_bytes(2, "little"),
                                ATT_FIND_INFO_RSP)
            if rsp[0] == ATT_ERROR_RSP:
                return None
            step = 4 if rsp[1] == 1 else 18
            last = start
            for i in range(2, len(rsp), step):
                h = int.from_bytes(rsp[i:i + 2], "little")
                if rsp[1] == 1 and int.from_bytes(rsp[i + 2:i + 4], "little") == GATT_CCCD:
                    return h
                last = h
            start = last + 1
        return None

    def write(self, handle: int, value: bytes, timeout: float = 10.0) -> bool:
        rsp = self._request(bytes([ATT_WRITE_REQ]) + handle.to_bytes(2, "little") + value, ATT_WRITE_RSP, timeout)
        return rsp[0] == ATT_WRITE_RSP

    def drain(self, seconds: float) -> None:
        end = time.time() + seconds
        while time.time() < end:
            rsp = self._recv(max(0.05, end - time.time()))
            if rsp is None:
                return
            if rsp[0] in (ATT_HANDLE_VALUE_NTF, ATT_HANDLE_VALUE_IND):
                self.notifications.append((int.from_bytes(rsp[1:3], "little"), rsp[3:]))
                if rsp[0] == ATT_HANDLE_VALUE_IND:
                    self.sock.send(bytes([ATT_HANDLE_VALUE_CFM]))


WAKE_CHAR = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
NOTIFY_CHAR = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
WAKE_CMD = b"AT+WAKEPULSE=10\r\n"


def direct_wake(mac: str, adapter: str = "hci1", connect_timeout: float = 40.0,
                send_wake: bool = True, log=print) -> dict:
    """Direct-connect to `mac`, then (optionally) send the GardePro wake pulse.

    Mirrors the firmware: subscribe to 6e400003/6e400004, write the wake
    command to 6e400004 three times, and collect notifications ("OK").
    """
    res: dict = {"connected": False, "ok_notify": False, "notifications": []}
    t0 = time.time()
    sock = direct_connect(mac, adapter, connect_timeout)
    res["connected"] = True
    res["connect_s"] = round(time.time() - t0, 1)
    log(f"BLE direct: link up to {mac} on {adapter} in {res['connect_s']}s")
    try:
        att = ATTClient(sock, log)
        chars = att.characteristics()
        res["characteristics"] = [f"{c['uuid'][:8]}@{c['handle']:#06x}" for c in chars]
        by_uuid = {c["uuid"]: c for c in chars}
        wake = by_uuid.get(WAKE_CHAR)
        if wake is None:
            raise RuntimeError(f"wake characteristic not found; have {res['characteristics']}")
        for u in (NOTIFY_CHAR, WAKE_CHAR):
            c = by_uuid.get(u)
            if c and c["props"] & 0x30:  # notify / indicate
                cccd = att.find_cccd(c)
                if cccd:
                    att.write(cccd, b"\x01\x00" if c["props"] & 0x10 else b"\x02\x00")
        if send_wake:
            time.sleep(0.15)
            for i in range(1, 4):
                log(f"BLE direct: wake pulse {i}/3")
                att.write(wake["handle"], WAKE_CMD)
                att.drain(0.35)
            att.drain(2.0)
        res["notifications"] = [v.decode(errors="replace").strip() for _, v in att.notifications]
        res["ok_notify"] = "OK" in res["notifications"]
        if send_wake:
            log(f"BLE direct: wake {'acknowledged (OK)' if res['ok_notify'] else 'sent, no OK notify'}")
    finally:
        sock.close()
    return res
