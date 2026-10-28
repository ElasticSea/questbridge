"""Raw USB access to the Quest's XRSP interface via libusb (portable to macOS).

Interface 0 (class ff/8a/01) exposes bulk OUT 0x01 and bulk IN 0x81. Incoming
bytes are buffered and split into XRSP messages; outgoing messages are packed
into padded transfers exactly like Meta's host does.
"""
import os
import struct
import sys

import libusb_package
import usb.core
import usb.util

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
from xrsp import pack_transfer  # noqa: E402

QUEST_VID = 0x2833
QUEST_PIDS = (0x5010, 0x5011, 0x5012, 0x5013, 0x5017, 0x5018, 0x5019, 0x501A)
XRSP_INTERFACE = 0
EP_OUT, EP_IN = 0x01, 0x81
READ_SIZE = 64 * 1024


class XrspLink:
    def __init__(self):
        backend = libusb_package.get_libusb1_backend()
        self.dev = usb.core.find(idVendor=QUEST_VID, custom_match=lambda d: d.idProduct in QUEST_PIDS,
                                 backend=backend)
        if self.dev is None:
            raise RuntimeError("Quest not found (is it plugged in and awake?)")
        usb.util.claim_interface(self.dev, XRSP_INTERFACE)
        self.buf = b""

    def close(self):
        usb.util.release_interface(self.dev, XRSP_INTERFACE)
        usb.util.dispose_resources(self.dev)

    def send(self, raw_messages):
        """Send one transfer containing the given raw XRSP messages."""
        data = pack_transfer(raw_messages)
        self.dev.write(EP_OUT, data, timeout=1000)

    def recv(self, timeout_ms=1000):
        """Return the next complete non-padding message as raw bytes, or None on timeout."""
        while True:
            msg = self._take()
            if msg is not None:
                if msg[1] != 0:
                    return msg
                continue
            try:
                self.buf += bytes(self.dev.read(EP_IN, READ_SIZE, timeout=timeout_ms))
            except usb.core.USBTimeoutError:
                return None

    def _take(self):
        if len(self.buf) < 8:
            return None
        size = 4 + struct.unpack_from("<H", self.buf, 2)[0] * 4
        if size < 8:
            raise ValueError(f"bad XRSP header {self.buf[:8].hex()}")
        if len(self.buf) < size:
            return None
        msg, self.buf = self.buf[:size], self.buf[size:]
        return msg
