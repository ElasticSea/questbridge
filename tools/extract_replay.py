"""Extract what Meta's host sends after the handshake, for replay by our host.

Writes host/replay/<name>.json:
  setup   PC->Quest stream messages (not topic 1, not video/audio) from session
          start until the first video frame, in order: {t, topic, flags, body}
  frame   the raw XRSP messages of one complete video frame on topics 10..14:
          segment table, EncodedSlice header, optional csd, video NALs
  frame_index_offsets  per message, byte offset of EncodedSlice frame.index
          inside the header body (so the host can renumber frames)

Usage: py extract_replay.py <pcap> <device> <session-start-s> <frame-index> [name]
"""
import json
import os
import struct
import sys

from decode_video import VIDEO_TOPICS, parse_header
from xrsp import messages

AUDIO_TOPIC = 6


def main():
    path, dev, start, want = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4])
    name = sys.argv[5] if len(sys.argv) > 5 else f"frame{want}"
    setup, frame, first_video = [], [], None
    pending = {}  # topic -> list of raw msgs for the slice being assembled
    done = set()
    for m in messages(path, dev):
        if m.ts < start or not m.to_quest or m.topic == 1:
            continue
        if m.topic in VIDEO_TOPICS:
            first_video = first_video or m.ts
            if m.topic in done:
                continue
            msgs = pending.setdefault(m.topic, [])
            if len(m.body) == 8 and struct.unpack_from("<I", m.body)[0] == 0:
                pending[m.topic] = [m]  # new slice starts with its segment table
                continue
            if not msgs:
                continue
            msgs.append(m)
            if len(msgs) == 2:
                hdr = parse_header(m.body)
                if hdr["frame"] != want:
                    pending[m.topic] = []
                    continue
                need = hdr["csd_len"] + hdr["video_len"]
                msgs[0].need = need
            else:
                got = sum(len(x.body) - (x.body[-1] if x.flags & 0x08 else 0) for x in msgs[2:])
                if got >= msgs[0].need:
                    frame += msgs
                    done.add(m.topic)
            if len(done) == len(VIDEO_TOPICS):
                break
        elif first_video is None and m.topic != AUDIO_TOPIC:
            setup.append(m)
    if len(done) != len(VIDEO_TOPICS):
        sys.exit(f"frame {want} incomplete: got slices on topics {sorted(done)}")

    def enc(m):
        return {"t": round(m.ts - start, 6), "topic": m.topic, "flags": m.flags, "body": m.body.hex()}

    out = {"source": os.path.basename(path), "session_start": start, "frame_index": want,
           "setup": [enc(m) for m in setup], "frame": [enc(m) for m in frame]}
    dest = os.path.join(os.path.dirname(__file__), "..", "host", "replay")
    os.makedirs(dest, exist_ok=True)
    with open(os.path.join(dest, f"{name}.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    print(f"{name}: {len(setup)} setup msgs (topics {sorted({m.topic for m in setup})}), "
          f"{len(frame)} frame msgs, {sum(len(m.body) for m in frame)} B")


if __name__ == "__main__":
    main()
