"""Our own Link host: perform Meta's XRSP handshake with the Quest.

PC-side messages are built from the Cap'n Proto trees Meta's host sent in the
recorded handshake (tests/fixtures/handshake), with session-specific fields
replaced. Each step is logged so a run can be compared with Meta's transcript.

Usage: py host/handshake.py [seconds]
"""
import collections
import copy
import json
import os
import struct
import sys
import threading
import time

from usb_link import XrspLink

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from capnp_codec import decode, encode  # noqa: E402
from xrsp import encode_message  # noqa: E402

TOPIC_CONTROL = 1
HELLO, CONFIG, ACK, CONFIRM, CONFIRM_ACK, START, READY, LINK_REQUEST = 0x81, 0x82, 0x83, 0x89, 0x8A, 0x87, 0x80, 0x85
CLOCK_PING, CLOCK_PONG = 0x06, 0x16
TOPIC_HEAD_POSE, HEAD_POSE_SEGMENT = 34, 272


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
        self.send_lock = threading.Lock()
        self.stream_seq = collections.defaultdict(int)
        self.rx_counts = collections.Counter()
        self.quest_offset_ns = None
        self.frame_start = None
        self.skip_setup = False
        self.head_pose = None  # (quat xyzw, position, sample ns) from topic 34
        # Render pose written into each frame: "fixed" = first head pose (panel stays put in
        # the world via headset reprojection), "live" = latest pose, "template" = as recorded
        self.pose_mode = os.environ.get("XRSP_POSE", "fixed")

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
        with self.send_lock:
            self.link.send([encode_message(0x10, TOPIC_CONTROL, self.seq, body)])
            self.log("PC->Q", label, self.seq)
            self.seq += 1

    def send_stream(self, msgs):
        """Send (topic, flags, body) stream messages in one transfer, numbering seq per topic."""
        raw = []
        with self.send_lock:
            for topic, flags, body in msgs:
                raw.append(encode_message(flags, topic, self.stream_seq[topic] & 0xFFFF, body))
                self.stream_seq[topic] += 1
            self.link.send(raw)

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
            # Quest transmit time vs our receive time: offset good to ~1 ms (one-way USB latency)
            self.quest_offset_ns = stamps[2] - now
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
        if os.environ.get("XRSP_ATTACH"):
            # Headset session is still up from an earlier run of ours: skip the handshake
            self.seq = int(os.environ.get("XRSP_CONTROL_SEQ", "16"))
            # The headset remembers per-topic seq and frame index; continue past earlier runs
            for t in range(10, 15):
                self.stream_seq[t] = int(os.environ.get("XRSP_VIDEO_SEQ", "0"))
            self.frame_start = int(os.environ.get("XRSP_FRAME_START", "0")) or None
            self.skip_setup = True
            self.log("info", "attaching to existing session", None)
            self.stream(seconds)
            return
        hello = self.expect(HELLO, timeout=float(os.environ.get("XRSP_HELLO_WAIT", "300")))
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
        self.stream(seconds)

    def stream(self, seconds):
        replay = os.environ.get("XRSP_REPLAY")
        if not replay:
            end = time.time() + 5
            while time.time() < end:
                try:
                    self.expect(-1, timeout=end - time.time())
                except TimeoutError:
                    break
            return
        stop = threading.Event()
        reader = threading.Thread(target=self.read_loop, args=(stop,), daemon=True)
        reader.start()
        try:
            self.replay(replay, seconds)
        finally:
            stop.set()
            reader.join(timeout=2)

    # --- streaming -----------------------------------------------------------
    def read_loop(self, stop):
        """Drain Quest->PC traffic in the background: answer clock pings, count the rest."""
        last = time.time()
        while not stop.is_set():
            msg = self.link.recv(timeout_ms=100)
            if msg is not None:
                topic = msg[1]
                if topic == TOPIC_CONTROL and msg[9] == 0x80 and msg[8] in (CLOCK_PING, CLOCK_PONG):
                    self.on_clock(msg[8], msg[8:])
                elif topic == TOPIC_CONTROL:
                    self.log("Q->PC", f"control 0x{msg[8]:02x}", struct.unpack_from("<H", msg, 4)[0])
                else:
                    self.rx_counts[topic] += 1
                    if topic == TOPIC_HEAD_POSE and len(msg) == 8 + HEAD_POSE_SEGMENT:
                        # Capnp segment of a head pose: quat xyzw @0xe8, position @0xf8, sample ns @0x108
                        self.head_pose = (struct.unpack_from("<4f", msg, 8 + 0xE8),
                                          struct.unpack_from("<3f", msg, 8 + 0xF8),
                                          struct.unpack_from("<Q", msg, 8 + 0x108)[0])
            if time.time() - last >= 2:
                self.log("info", "rx " + " ".join(f"t{t}:{n}" for t, n in sorted(self.rx_counts.items())), None)
                self.rx_counts.clear()
                last = time.time()

    def replay(self, path, seconds):
        """Replay Meta's post-handshake setup, then loop one recorded frame at 72 Hz."""
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        prev_t = None
        for m in [] if self.skip_setup else data["setup"]:
            if prev_t is not None:
                time.sleep(min(m["t"] - prev_t, 0.5))
            prev_t = m["t"]
            self.send_stream([(m["topic"], m["flags"], bytes.fromhex(m["body"]))])
        clip = data.get("frames") or [data["frame"]]
        self.log("info", f"sent {len(data['setup'])} setup msgs; looping {len(clip)} frame(s) at 72 Hz", None)
        # EncodedSlice timing words in the Quest clock: renderPose time (5), pcPresentBegin (9),
        # targetVsync (11), targetDeadline (12), estimatedGpuEnd (13), latency marker (14)
        timing_words = (5, 9, 11, 12, 13, 14)
        prepared = []
        for msgs in clip:
            frame = [(m["topic"], m["flags"], bytearray.fromhex(m["body"])) for m in msgs]
            headers = []
            for i, (t, fl, body) in enumerate(frame):
                if i > 0 and len(frame[i - 1][2]) == 8 and frame[i - 1][0] == t:
                    d = 8 + 8 * ((struct.unpack_from("<Q", body, 0)[0] & 0xFFFFFFFF) >> 2)
                    headers.append((body, d, {w: struct.unpack_from("<Q", body, d + 8 * w)[0]
                                              for w in timing_words}))
            prepared.append((frame, headers))
        period, index = 1 / 72, self.frame_start or data.get("frame_index", 1)
        while self.quest_offset_ns is None:
            time.sleep(0.05)  # need one clock ping to map our clock onto the Quest's
        if self.pose_mode != "template":
            while self.head_pose is None:
                time.sleep(0.01)
        fixed_pose = self.head_pose
        nxt, end, sent = time.perf_counter(), time.time() + seconds, 0
        while time.time() < end:
            frame, headers = prepared[sent % len(prepared)]
            quest_now = time.perf_counter_ns() + self.quest_offset_ns
            pose = fixed_pose if self.pose_mode == "fixed" else self.head_pose
            for body, d, orig in headers:
                if pose and self.pose_mode != "template":
                    # renderPose: orientation xyzw at data u32 slots 3..6, translation at 7..9
                    struct.pack_into("<4f3f", body, d + 12, *pose[0], *pose[1])
                struct.pack_into("<Q", body, d, index)  # frame.index
                # Shift all timestamps together so the frame targets a vsync ~20 ms out
                shift = quest_now + 20_000_000 - orig[11]
                for w, v in orig.items():
                    if v:
                        struct.pack_into("<Q", body, d + 8 * w, v + shift)
            self.send_stream([(t, fl, bytes(b)) for t, fl, b in frame])
            index += 1
            sent += 1
            nxt += period
            time.sleep(max(0.0, nxt - time.perf_counter()))
        self.log("info", f"streamed {sent} frames", None)


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30
    t0 = time.time()

    def log(direction, what, seq):
        print(f"{time.time() - t0:8.3f} {direction:6} {what}" + (f" seq={seq}" if seq is not None else ""), flush=True)

    # The Quest sends HELLO once, ~1.5 s after it enumerates, and drops it if no host
    # is reading. Like Meta's hotplug watcher, wait for the interface and open it at once.
    wait = float(os.environ.get("XRSP_WAIT", "600"))
    log("info", f"waiting up to {wait:.0f}s for the Quest to be plugged in", None)
    end = time.time() + wait
    while True:
        link = None
        while link is None:
            try:
                link = XrspLink()
            except (RuntimeError, OSError):
                if time.time() > end:
                    raise
                time.sleep(0.02)
        log("info", "XRSP interface opened", None)
        try:
            Host(link, log).run(seconds)
            return
        except OSError as e:
            # Unplugged/replugged mid-session: wait for it to come back and start over
            log("info", f"transport lost ({e}); waiting for reconnect", None)
        finally:
            try:
                link.close()
            except OSError:
                pass


if __name__ == "__main__":
    main()
