"""Open the Quest's XRSP interface and print whatever it sends.

Usage: py host/probe.py [seconds] [--reset]
  --reset  USB-reset the headset first so it re-enumerates and starts fresh
"""
import struct
import sys
import time

from usb_link import XrspLink


def main():
    seconds = float(next((a for a in sys.argv[1:] if not a.startswith("--")), 5))
    if "--reset" in sys.argv:
        link = XrspLink()
        link.dev.reset()
        link.close()
        time.sleep(3)
    link = XrspLink()
    print("claimed XRSP interface")
    end = time.time() + seconds
    try:
        while time.time() < end:
            msg = link.recv(timeout_ms=500)
            if msg is None:
                continue
            flags, topic, _, seq = struct.unpack_from("<BBHH", msg)
            inner = f" type=0x{msg[8]:02x} flags=0x{msg[9]:02x}" if topic == 1 else ""
            print(f"{time.time():.3f} topic={topic} seq={seq} flags=0x{flags:02x} len={len(msg)}{inner}")
    finally:
        link.close()


if __name__ == "__main__":
    main()
