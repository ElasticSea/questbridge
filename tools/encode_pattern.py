"""Encode a moving test pattern and package it as XRSP video slices for the host.

Encodes with FFmpeg/x264 using the stream format the Quest expects (decoded from
Meta's host): H.264 Constrained Baseline, 4096x2240, all-intra so the host can
loop the clip. Each slice is wrapped like Meta's: segment table, EncodedSlice
header (from the recorded template; index, flags, yOffset, heightInMacroBlocks,
csd/video lengths patched), SPS+PPS, then the slice NAL. Slice i goes on topic 10+i.

Layouts (encoder gate experiments):
  equal5        one picture, 5 slices of 28 MB rows (Meta's layout, default)
  single        one picture, 1 slice
  split5        5 independent 4096x448 streams (own SPS/PPS each), one per topic
  mbs:N         one picture, slices of at most N macroblocks (x264 slice-max-mbs);
                N not a multiple of 256 gives slices that start mid-row
  bytes:N       one picture, slices of at most N bytes (x264 slice-max-size, like
                VideoToolbox's MaxH264SliceBytes)

Writes host/replay/<name>.json with "frames": [[{topic, flags, body}, ...], ...]
and the setup messages copied from the Meta replay file.

Usage: py encode_pattern.py [frames] [name] [crf] [layout]
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
MB_COLS, MB_TOTAL = WIDTH // 16, WIDTH // 16 * HEIGHT // 16
TEMPLATE = os.path.join(ROOT, "host", "replay", "meta-idr.json")


def encode(frames, crf, out, slice_params, height=HEIGHT, crop_y=None):
    vf = f"testsrc2=size={WIDTH}x{HEIGHT}:rate=72,format=yuv420p"
    if crop_y is not None:
        vf += f",crop={WIDTH}:{height}:0:{crop_y}"
    subprocess.run([
        FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", vf,
        "-frames:v", str(frames), "-c:v", "libx264", "-profile:v", "baseline", "-level", "6.0",
        "-preset", "ultrafast", "-tune", "zerolatency", "-g", "1", "-bf", "0", "-crf", str(crf),
        "-x264-params", f"{slice_params}:repeat-headers=1:aud=0",
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


def first_mb(nal):
    """first_mb_in_slice: the first ue(v) after the NAL header (no emulation bytes this early)."""
    bits = int.from_bytes(nal[1:9], "big")
    zeros = 0
    while not bits >> (63 - zeros) & 1:
        zeros += 1
    return (bits >> (63 - 2 * zeros)) - 1


def pictures_of(nals):
    """Group NALs into (sps, pps, [slice NALs]) per picture; a slice at MB 0 starts a picture."""
    pics, sps, pps = [], None, None
    for nal in nals:
        t = nal[0] & 0x1F
        if t == 7:
            sps = nal
        elif t == 8:
            pps = nal
        elif t in (1, 5):
            if first_mb(nal) == 0:
                pics.append((sps, pps, []))
            pics[-1][2].append(nal)
    return pics


def pad4(body):
    """Meta's padding: bodies are 4-byte aligned; flag 0x08 + last byte = pad count, filler 0xDE."""
    n = -len(body) % 4
    if not n:
        return 0x10, body
    return 0x18, body + b"\xde" * (n - 1) + bytes([n])


def csd(sps, pps):
    return b"\x00\x00\x00\x01" + sps + b"\x00\x00\x00\x01" + pps


def es_pictures(path):
    with open(path, "rb") as f:
        return pictures_of(split_nals(f.read()))


def build(count, name, crf, layout):
    """-> per frame a list of slices (csd, nal, yOffset px, heightInMacroBlocks)."""
    es = os.path.join(ROOT, "capture", "runs", f"{name}.h264")
    os.makedirs(os.path.dirname(es), exist_ok=True)
    if layout == "split5":
        h = HEIGHT // SLICES
        streams = []
        for i in range(SLICES):
            part = es.replace(".h264", f"-{i}.h264")
            encode(count, crf, part, "slices=1:threads=1", h, i * h)
            streams.append(es_pictures(part))
        return [[(csd(sps, pps), nals[0], i * h, h // 16) for i, (sps, pps, nals) in enumerate(pics)]
                for pics in zip(*streams)]
    params = {"equal5": f"slices={SLICES}:threads={SLICES}", "single": "slices=1:threads=1"}.get(layout)
    if params is None:
        kind, n = layout.split(":")
        params = {"mbs": "slice-max-mbs", "bytes": "slice-max-size"}[kind] + f"={n}:threads={SLICES}"
    encode(count, crf, es, params)
    out = []
    for sps, pps, nals in es_pictures(es):
        starts = [first_mb(n) for n in nals] + [MB_TOTAL]
        # Rows a slice touches (a mid-row start shares that row with the previous slice)
        out.append([(csd(sps, pps), nal, starts[i] // MB_COLS * 16,
                     -(-starts[i + 1] // MB_COLS) - starts[i] // MB_COLS) for i, nal in enumerate(nals)])
    return out


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 144
    name = sys.argv[2] if len(sys.argv) > 2 else "pattern"
    crf = int(sys.argv[3]) if len(sys.argv) > 3 else 28
    layout = sys.argv[4] if len(sys.argv) > 4 else "equal5"
    pictures = build(count, name, crf, layout)

    with open(TEMPLATE, encoding="utf-8") as f:
        meta = json.load(f)
    # Recorded segment table and EncodedSlice header of slice 0 (the topics differ only in patched fields)
    seg_table, header = meta["frame"][0], bytes.fromhex(meta["frame"][1]["body"])

    out_frames, layouts = [], set()
    for slices in pictures:
        if len(slices) > SLICES:
            raise SystemExit(f"{len(slices)} slices in a frame; only topics 10..{9 + SLICES} exist")
        layouts.add(tuple((y // 16, h) for _, _, y, h in slices))
        frame = []
        for i, (c, nal, y, h_mb) in enumerate(slices):
            topic = 10 + i
            video = b"\x00\x00\x00\x01" + nal
            hdr = bytearray(header)
            d = 8 + 8 * ((struct.unpack_from("<Q", hdr, 0)[0] & 0xFFFFFFFF) >> 2)
            hdr[d + 48] = i                                       # slice.index
            hdr[d + 49] = 1 | (2 if i == len(slices) - 1 else 0)  # isIntraFrame, isLastSliceOfAFrame
            # yOffset (px), heightInMacroBlocks, csdLength, videoLength
            struct.pack_into("<IIII", hdr, d + 4 * 13, y, h_mb, len(c), len(video))
            frame.append({**seg_table, "topic": topic})
            frame.append({"topic": topic, "flags": 0x10, "body": bytes(hdr).hex()})
            for payload in (c, video):
                flags, body = pad4(payload)
                frame.append({"topic": topic, "flags": flags, "body": body.hex()})
        out_frames.append(frame)

    dest = os.path.join(ROOT, "host", "replay", f"{name}.json")
    with open(dest, "w", encoding="utf-8") as f:
        json.dump({"source": f"testsrc2 x264 crf{crf} {layout}", "frame_index": 1, "setup": meta["setup"],
                   "frames": out_frames}, f)
    sizes = [sum(len(bytes.fromhex(m["body"])) for m in fr) for fr in out_frames]
    print(f"{name}: {len(out_frames)} frames, avg {sum(sizes) // max(1, len(sizes))} B/frame, "
          f"{sum(sizes) * 72 / max(1, len(sizes)) / 1e6 * 8:.0f} Mbit/s at 72 Hz -> {os.path.relpath(dest, ROOT)}")
    print(f"slice layouts (first MB row, rows), {len(layouts)} distinct:", sorted(layouts)[:3])


if __name__ == "__main__":
    main()
