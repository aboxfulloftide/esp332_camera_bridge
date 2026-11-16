#!/usr/bin/env python3
"""Read or set the kernel's selected Bluetooth PHYs for an adapter (mgmt API).

The kernel only scans and initiates LE connections on the PHYs selected here.
By default that is LE 1M only, so a BLE 5 adapter never connects over the
long-range LE Coded PHY. `btmgmt phy` does the same job but hangs on this Pi.
The setting is runtime only; it resets when the adapter or bluetoothd restarts.

  tools/ble_phy.py hci1                 # show
  sudo tools/ble_phy.py hci1 --coded    # add LE Coded TX+RX to the selection
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import socket
import struct
import sys

AF_BLUETOOTH, BTPROTO_HCI, HCI_CHANNEL_CONTROL, HCI_DEV_NONE = 31, 1, 3, 0xFFFF
OP_READ_PHY, OP_SET_PHY = 0x0044, 0x0045
EV_CMD_COMPLETE, EV_CMD_STATUS = 0x0001, 0x0002
BITS = ["BR1M1SLOT", "BR1M3SLOT", "BR1M5SLOT", "EDR2M1SLOT", "EDR2M3SLOT", "EDR2M5SLOT",
        "EDR3M1SLOT", "EDR3M3SLOT", "EDR3M5SLOT", "LE1MTX", "LE1MRX", "LE2MTX", "LE2MRX",
        "LECODEDTX", "LECODEDRX"]
LE_CODED = (1 << 13) | (1 << 14)

_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)


class _sockaddr_hci(ctypes.Structure):
    _fields_ = [("family", ctypes.c_ushort), ("dev", ctypes.c_ushort), ("channel", ctypes.c_ushort)]


def names(mask: int) -> str:
    return " ".join(n for i, n in enumerate(BITS) if mask & (1 << i)) or "-"


def mgmt(index: int, op: int, params: bytes = b"") -> bytes:
    s = socket.socket(AF_BLUETOOTH, socket.SOCK_RAW | socket.SOCK_CLOEXEC, BTPROTO_HCI)
    try:
        sa = _sockaddr_hci(AF_BLUETOOTH, HCI_DEV_NONE, HCI_CHANNEL_CONTROL)
        if _libc.bind(s.fileno(), ctypes.byref(sa), ctypes.sizeof(sa)) != 0:
            raise OSError(ctypes.get_errno(), "bind to mgmt channel failed (needs root?)")
        s.send(struct.pack("<HHH", op, index, len(params)) + params)
        s.settimeout(5)
        while True:
            pkt = s.recv(1024)
            ev, idx, _ln = struct.unpack_from("<HHH", pkt)
            if idx != index or ev not in (EV_CMD_COMPLETE, EV_CMD_STATUS):
                continue
            rop, status = struct.unpack_from("<HB", pkt, 6)
            if rop != op:
                continue
            if status:
                raise OSError(f"mgmt op 0x{op:04x} on index {index} failed: status 0x{status:02x}")
            return pkt[9:]
    finally:
        s.close()


def read_phy(index: int) -> tuple[int, int, int]:
    return struct.unpack_from("<III", mgmt(index, OP_READ_PHY))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("adapter", help="e.g. hci1")
    ap.add_argument("--coded", action="store_true", help="select LE Coded TX+RX in addition to the current PHYs")
    args = ap.parse_args()
    index = int(args.adapter.removeprefix("hci"))
    supported, configurable, selected = read_phy(index)
    print(f"{args.adapter} supported:    {names(supported)}")
    print(f"{args.adapter} selected:     {names(selected)}")
    if args.coded:
        if supported & LE_CODED != LE_CODED:
            print(f"{args.adapter} does not support LE Coded", file=sys.stderr)
            return 1
        mgmt(index, OP_SET_PHY, struct.pack("<I", selected | LE_CODED))
        print(f"{args.adapter} now selected: {names(read_phy(index)[2])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
