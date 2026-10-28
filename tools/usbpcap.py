"""Minimal reader for USBPcap .pcap files (linktype 249).

Yields one Packet per URB event. `inbound` is True for completions coming
back from the device (where IN data lives); OUT data rides on submissions.
"""
import struct
from dataclasses import dataclass

TRANSFER = {0: "iso", 1: "int", 2: "ctrl", 3: "bulk"}


@dataclass
class Packet:
    index: int
    ts: float
    irp: int
    status: int
    function: int
    inbound: bool
    bus: int
    device: int
    endpoint: int
    transfer: int
    data: bytes
    ctrl_stage: int = -1
    truncated: bool = False


def read(path):
    with open(path, "rb") as f:
        gh = f.read(24)
        magic = struct.unpack_from("<I", gh)[0]
        if magic != 0xA1B2C3D4:
            raise ValueError("unsupported pcap magic")
        linktype = struct.unpack_from("<I", gh, 20)[0]
        if linktype != 249:
            raise ValueError(f"not a USBPcap file (linktype {linktype})")
        t0 = None
        i = 0
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            sec, usec, incl, _ = struct.unpack("<IIII", rh)
            body = f.read(incl)
            ts = sec + usec / 1e6
            if t0 is None:
                t0 = ts
            hlen, irp, status, func, info, bus, dev, ep, xfer, dlen = struct.unpack_from("<HQIHBHHBBI", body)
            stage = body[27] if xfer == 2 and hlen >= 28 else -1
            i += 1
            yield Packet(i, ts - t0, irp, status, func, bool(info & 1), bus, dev, ep, xfer,
                         body[hlen:hlen + dlen], stage, hlen + dlen > incl)
