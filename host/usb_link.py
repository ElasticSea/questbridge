"""Raw USB access to the Quest's XRSP interface.

Interface 0 (class ff/8a/01) exposes bulk OUT 0x01 and bulk IN 0x81. Incoming
bytes are buffered and split into XRSP messages; outgoing messages are packed
into padded transfers exactly like Meta's host does.

Backends: WinUSB on Windows (opens interface 0 only, so adb can keep the ADB
interface), libusb elsewhere (macOS).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
from xrsp import pack_transfer  # noqa: E402

QUEST_VID = 0x2833
QUEST_PIDS = (0x5010, 0x5011, 0x5012, 0x5013, 0x5017, 0x5018, 0x5019, 0x501A)
XRSP_INTERFACE = 0
EP_OUT, EP_IN = 0x01, 0x81
READ_SIZE = 1024  # Quest pads transfers to 1 KiB and sends no short packets; larger reads stall and lose data on timeout


class LibusbPipe:
    def __init__(self):
        import libusb_package
        import usb.core
        import usb.util
        self._usb = usb
        self.dev = usb.core.find(idVendor=QUEST_VID, custom_match=lambda d: d.idProduct in QUEST_PIDS,
                                 backend=libusb_package.get_libusb1_backend())
        if self.dev is None:
            raise RuntimeError("Quest not found (is it plugged in and awake?)")
        usb.util.claim_interface(self.dev, XRSP_INTERFACE)

    def close(self):
        self._usb.util.release_interface(self.dev, XRSP_INTERFACE)
        self._usb.util.dispose_resources(self.dev)

    def write(self, data, timeout_ms=1000):
        return self.dev.write(EP_OUT, data, timeout=timeout_ms)

    def read(self, size=READ_SIZE, timeout_ms=1000):
        try:
            return bytes(self.dev.read(EP_IN, size, timeout=timeout_ms))
        except self._usb.core.USBTimeoutError:
            return None


def open_pipe():
    if sys.platform == "win32":
        from winusb_link import WinUsbPipe
        return WinUsbPipe()
    return LibusbPipe()


class XrspLink:
    def __init__(self):
        self.pipe = open_pipe()
        self.buf = b""

    def close(self):
        self.pipe.close()

    def send(self, raw_messages):
        """Send one transfer containing the given raw XRSP messages."""
        self.pipe.write(pack_transfer(raw_messages))

    def recv(self, timeout_ms=1000):
        """Return the next complete non-padding message as raw bytes, or None on timeout."""
        while True:
            msg = self._take()
            if msg is not None:
                if msg[1] != 0:
                    return msg
                continue
            data = self.pipe.read(READ_SIZE, timeout_ms)
            if data is None:
                return None
            self.buf += data

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
