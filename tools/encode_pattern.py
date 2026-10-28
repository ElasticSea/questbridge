"""Encode a moving test pattern and package it as XRSP video slices for the host.

Encodes with FFmpeg/x264 using the stream format the Quest expects (decoded from
Meta's host): H.264 Constrained Baseline, 4096x2240, one picture in 5 slices of
28 macroblock rows. Every frame is intra so the host can loop the clip. Each
slice is wrapped like Meta's: segment table, EncodedSlice header (from the
recorded template, lengths and flags patched), SPS+PPS, then the slice NAL.

Writes host/replay/<name>.json with "frames": [[{topic, flags, body}, ...], ...]
and the setup messages copied from the Meta replay file.

Usage: py encode_pattern.py [frames] [name] [crf]
"""
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FFMPEG = os.path.join(os.environ["LOCALAPPDATA"], "Microsoft", "WinGet", "Packages",
                      "Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe", "ffmpeg-9.0.2-full_build", "bin",
                      "ffmpeg.exe")
WIDTH, HEIGHT, SLICES = 4096, 2240, 5
TEMPLATE = os.path.join(ROOT, "host", "replay", "meta-idr.json")


def encode(frames, crf, out):
    subprocess.run([
        FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", f"testsrc2=size={WIDTH}x{HEIGHT}:rate=72,format=yuv420p",
        "-frames:v", str(frames), "-c:v", "libx264", "-profile:v", "baseline", "-level", "6.0",
        "-preset", "ultrafast", "-tune", "zerolatency", "-g", "1", "-bf", "0", "-crf", str(crf),
        "-x264-params", f"slices={SLICES}:threads={SLICES}:repeat-headers=1:aud=0",
        "-f", "h264", out], check=True)


def split_nals(es):
    """Annex-B stream -> list of NAL payloads (start codes stripped)."""
    starts, i = [], 0
    while True:
        i = es.find(b"\x00\x00\x01", i)
        if i < 0:
            break
        starts.append(i + 3)
        i += 3
    nals = []
    for n, s in enumerate(starts):
        end = starts[n + 1] - 3 if n + 1 < len(starts) else len(es)
        nal = es[s:end]
        while nal.endswith(b"\x00"):  # the next 4-byte start code's leading zero
            nal = nal[:-1]
        nals.append(nal)
    return nals


def frames_of(nals):
    """Group NALs into (sps, pps, [5 slice NALs]) per picture."""
    frames, sps, pps, cur = [], None, None, []
    for nal in nals:
        t = nal[0] & 0x1F
        if t == 7:
            sps = nal
        elif t == 8:
            pps = nal
        elif t in (1, 5):
            cur.append(nal)
            if len(cur) == SLICES:
                frames.append((sps, pps, cur))
                cur = []
    return frames


def pad4(body):
    """Meta's padding: bodies are 4-byte aligned; flag 0x08 + last byte = pad count, filler 0xDE."""
    n = -len(body) % 4
    if not n:
        return 0x10, body
    return 0x18, body + b"\xde" * (n - 1) + bytes([n])


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 144
    name = sys.argv[2] if len(sys.argv) > 2 else "pattern"
    crf = int(sys.argv[3]) if len(sys.argv) > 3 else 28
    es_path = os.path.join(ROOT, "capture", "runs", f"{name}.h264")
    os.makedirs(os.path.dirname(es_path), exist_ok=True)
    encode(count, crf, es_path)
    with open(es_path, "rb") as f:
        pictures = frames_of(split_nals(f.read()))

    with open(TEMPLATE, encoding="utf-8") as f:
        meta = json.load(f)
    # Per slice topic: the recorded segment table and EncodedSlice header
    seg_tables, headers = {}, {}
    msgs = meta["frame"]
    for i in range(1, len(msgs)):
        if len(bytes.fromhex(msgs[i - 1]["body"])) == 8 and msgs[i - 1]["topic"] == msgs[i]["topic"]:
            seg_tables[msgs[i]["topic"]] = msgs[i - 1]
            headers[msgs[i]["topic"]] = bytes.fromhex(msgs[i]["body"])

    out_frames = []
    for sps, pps, slices in pictures:
        csd = b"\x00\x00\x00\x01" + sps + b"\x00\x00\x00\x01" + pps
        frame = []
        for i, nal in enumerate(slices):
            topic = 10 + i
            video = b"\x00\x00\x00\x01" + nal
            hdr = bytearray(headers[topic])
            d = 8 + 8 * ((struct.unpack_from("<Q", hdr, 0)[0] & 0xFFFFFFFF) >> 2)
            hdr[d + 48] = i                                  # slice.index
            hdr[d + 49] = 1 | (2 if i == SLICES - 1 else 0)  # isIntraFrame, isLastSliceOfAFrame
            struct.pack_into("<II", hdr, d + 4 * 15, len(csd), len(video))  # csdLength, videoLength
            frame.append(seg_tables[topic])
            frame.append({"topic": topic, "flags": 0x10, "body": bytes(hdr).hex()})
            for payload in (csd, video):
                flags, body = pad4(payload)
                frame.append({"topic": topic, "flags": flags, "body": body.hex()})
        out_frames.append(frame)

    dest = os.path.join(ROOT, "host", "replay", f"{name}.json")
    with open(dest, "w", encoding="utf-8") as f:
        json.dump({"source": f"testsrc2 x264 crf{crf}", "frame_index": 1, "setup": meta["setup"],
                   "frames": out_frames}, f)
    sizes = [sum(len(bytes.fromhex(m["body"])) for m in fr) for fr in out_frames]
    print(f"{name}: {len(out_frames)} frames, avg {sum(sizes) // max(1, len(sizes))} B/frame, "
          f"{sum(sizes) * 72 / max(1, len(sizes)) / 1e6 * 8:.0f} Mbit/s at 72 Hz -> {os.path.relpath(dest, ROOT)}")


if __name__ == "__main__":
    main()
