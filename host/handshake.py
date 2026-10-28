"""Our own Link host: perform Meta's XRSP handshake with the Quest.

PC-side messages are built from the Cap'n Proto trees Meta's host sent in the
recorded handshake (tests/fixtures/handshake), with session-specific fields
replaced. Each step is logged so a run can be compared with Meta's transcript.

Usage: py host/handshake.py [seconds]
"""
import copy
import json
import os
import struct
import sys
import time

from usb_link import XrspLink

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from capnp_codec import decode, encode  # noqa: E402
from xrsp import encode_message  # noqa: E402

TOPIC_CONTROL = 1
HELLO, CONFIG, ACK, CONFIRM, CONFIRM_ACK, START, READY, LINK_REQUEST = 0x81, 0x82, 0x83, 0x89, 0x8A, 0x87, 0x80, 0x85
CLOCK_PING, CLOCK_PONG = 0x06, 0x16


def load_templates():
    with open(os.path.join(ROOT, "tests", "fixtures", "handshake", "control.json"), encoding="utf-8") as f:
        control = json.load(f)
    templates = {}
    for e in control:
        if e["dir"] != "pc_to_quest" or "capnp" not in e:
            continue
        version = e["capnp"]["tree"]["s"][0] & 0xFFFF
        templates.setdefault((e["inner"]["type"], version), e)
    return templates


class Host:
    def __init__(self, link, log):
        self.link = link
        self.log = log
        self.templates = load_templates()
        self.seq = 0
        self.clock_count = 0
        self.session_id = 0

    # --- sending -------------------------------------------------------------
    def send_control(self, typ, version, patch=None):
        t = self.templates[(typ, version)]
        tree = copy.deepcopy(t["capnp"]["tree"])
        if patch:
            patch(tree)
        capnp = encode(tree, t["capnp"]["words"], t["capnp"]["order"])
        inner = struct.pack("<BBHI", typ, t["inner"]["flags"], -(-len(capnp) // 16), 1)
        self._send(inner + capnp, f"0x{typ:02x} v{version}")

    def send_clock(self, typ, stamps):
        self.clock_count += 1
        inner = struct.pack("<BBHI", typ, 0x80, 2, self.clock_count)
        self._send(inner + struct.pack("<4Q", *stamps), f"clock 0x{typ:02x}")

    def _send(self, body, label):
        self.link.send([encode_message(0x10, TOPIC_CONTROL, self.seq, body)])
        self.log("PC->Q", label, self.seq)
        self.seq += 1

    # --- receiving -----------------------------------------------------------
    def expect(self, wanted, timeout=3.0):
        """Wait for a control message of type `wanted`, answering clock pings meanwhile."""
        end = time.time() + timeout
        while time.time() < end:
            msg = self.link.recv(timeout_ms=200)
            if msg is None:
                continue
            topic = msg[1]
            if topic != TOPIC_CONTROL:
                self.log("Q->PC", f"topic {topic} len={len(msg)}", struct.unpack_from("<H", msg, 4)[0])
                continue
            body = msg[8:]
            typ, flags = body[0], body[1]
            seq = struct.unpack_from("<H", msg, 4)[0]
            if flags == 0x80 and typ in (CLOCK_PING, CLOCK_PONG):
                self.on_clock(typ, body)
                continue
            tree, _ = decode(body[8:])
            self.log("Q->PC", f"0x{typ:02x} v{tree['s'][0] & 0xFFFF}", seq)
            if typ == wanted:
                return tree
        raise TimeoutError(f"no 0x{wanted:02x} within {timeout}s")

    def on_clock(self, typ, body):
        now = time.perf_counter_ns()
        stamps = struct.unpack_from("<4Q", body, 8)
        self.log("Q->PC", f"clock 0x{typ:02x} {stamps[2]}", None)
        if typ == CLOCK_PING:
            # NTP-style reply: echo their transmit time, our receive and transmit times
            self.send_clock(CLOCK_PONG, (stamps[2], now, time.perf_counter_ns(), 0))

    # --- handshake -----------------------------------------------------------
    def phase(self, hello, version):
        session = self.session_id

        def config_patch(tree):
            if version == 1:
                tree["s"][10] = session

        def session_patch(tree):
            if version == 1:
                tree["s"][1] = session

        self.send_control(CONFIG, version, config_patch)
        self.expect(ACK)
        self.send_control(CONFIRM, version, session_patch)
        self.expect(CONFIRM_ACK)
        self.send_control(START, version, session_patch)
        self.expect(READY)

    def run(self, seconds):
        hello = self.expect(HELLO, timeout=5)
        self.session_id = hello["s"][6]
        version = hello["s"][0] & 0xFFFF
        if version == 1:
            self.phase(hello, 1)
            self.log("info", "phase 1 done; waiting for Enable Link (0x85)", None)
            self.expect(LINK_REQUEST, timeout=seconds)
            self.seq = 0
            hello = self.expect(HELLO, timeout=10)
        self.phase(hello, 3)
        self.log("info", "phase 2 done; starting clock sync", None)
        self.send_clock(CLOCK_PING, (0, 0, time.perf_counter_ns(), 0))
        end = time.time() + 5
        while time.time() < end:
            try:
                self.expect(-1, timeout=end - time.time())
            except TimeoutError:
                break


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30
    t0 = time.time()

    def log(direction, what, seq):
        print(f"{time.time() - t0:8.3f} {direction:6} {what}" + (f" seq={seq}" if seq is not None else ""), flush=True)

    # The Quest sends HELLO once per connection: reset so it re-enumerates and greets us
    link = XrspLink()
    link.dev.reset()
    link.close()
    time.sleep(3)
    link = XrspLink()
    try:
        Host(link, log).run(seconds)
    finally:
        link.close()


if __name__ == "__main__":
    main()
