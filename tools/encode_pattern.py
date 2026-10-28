# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
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

Writes out/clips/<name>.json with "frames": [[{topic, flags, body}, ...], ...]
and the setup messages copied from the Meta replay file.

codec hevc (split5 only): x265 Main strips with VPS+SPS+PPS as csd, always 72 fps, for the HEVC gate
(CONFIG codec, XRSP_CODEC in tools/pyhost/handshake.py).

Usage: py encode_pattern.py [frames] [name] [crf] [layout] [chunk bytes] [source] [gop] [h264|hevc]
"""
import json
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FFMPEG = os.environ.get("FFMPEG") or shutil.which("ffmpeg") or "ffmpeg"
WIDTH, HEIGHT, SLICES = 4096, 2240, 5
MB_COLS, MB_TOTAL = WIDTH // 16, WIDTH // 16 * HEIGHT // 16
TEMPLATE = os.path.join(ROOT, "tests", "fixtures", "replay", "idr-frame.json")


FONT = "/Windows/Fonts/arialbd.ttf"  # drive-less: a "C:" would need filtergraph escaping


# Per-eye FovPort tangents (left, right, up, down) as in Meta's game frames; left half = left eye
EYE_TANGENTS = ((1.3764, 0.8391, 0.9657, 1.4281), (0.8391, 1.3764, 0.9657, 1.4281))


def eye_centre(eye, w, h):
    """Pixel of the straight-ahead direction in one eye's half (rectilinear projection)."""
    left, right, up, down = EYE_TANGENTS[eye]
    return round(left / (left + right) * w), round(up / (up + down) * h)


def source(src, rate=None):
    """lavfi source:
      testsrc2                         moving pattern, differs per eye
      grid:<colour>                    static grid with a centre cross, same pixels in both eyes
      look:<colour>:<secs>:<l1>|<l2>   grid plus instruction lines and a countdown
      text:<colour>:<secs>:<l1>|<l2>   instruction lines only
      world:<colour>:<secs>:<l1>|<l2>  like look, but drawn per eye through its own projection
                                       (cross straight ahead, grid every 0.25 tan) so the eyes fuse
    Text sits inside one slice band (448 px each), never on a slice edge.
    Lines must not contain ':' ',' or quotes."""
    if src == "testsrc2":
        return f"testsrc2=size={WIDTH}x{HEIGHT}:rate=72,format=yuv420p"
    kind, c, *rest = src.split(":", 3)
    w, h = WIDTH // 2, HEIGHT
    # look/text/world clips change once a second: 1 frame/s, stream with XRSP_HOLD=72
    rate = rate or (72 if kind == "grid" else 1)
    eyes = []
    for eye in range(2):
        cx, cy = eye_centre(eye, w, h) if kind == "world" else (w // 2, h // 2)
        vf = f"color=c={c}:s={w}x{h}:r={rate}"
        if kind == "world":
            # 0.25 tan steps: the horizontal and vertical tangent spans differ, so cells are not square
            gx, gy = round(0.25 * w / sum(EYE_TANGENTS[eye][:2])), round(0.25 * h / sum(EYE_TANGENTS[eye][2:]))
            vf += f",drawgrid=x={cx % gx}:y={cy % gy}:w={gx}:h={gy}:t=6:c=white"
        elif kind in ("grid", "look"):
            vf += ",drawgrid=w=128:h=128:t=6:c=white"
        if kind != "text":
            vf += (f",drawbox=x={cx - 20}:y={cy - 160}:w=40:h=320:c=black:t=fill,"
                   f"drawbox=x={cx - 160}:y={cy - 20}:w=320:h=40:c=black:t=fill")
        if kind in ("look", "text", "world"):
            secs, lines = int(rest[0]), [line for line in rest[1].split("|") if line]
            box = "box=1:boxcolor=black:boxborderw=14"
            # world: the centre cross sits near band 2's top edge, so text goes to band 3, countdown to 4
            text_y, count_y = (1366, 1840) if kind == "world" else (470, 1480)
            for i, line in enumerate(lines[:4]):
                vf += (f",drawtext=fontfile={FONT}:text='{line}':fontsize=64:fontcolor=white:{box}"
                       f":x={cx}-text_w/2:y={text_y + 105 * i}")
            vf += (rf",drawtext=fontfile={FONT}:text='%{{eif\:max(0\,{secs}-t)\:d}}':fontsize=150"
                   f":fontcolor=yellow:{box}:x={cx}-text_w/2:y={count_y}")
        eyes.append(vf + f"[e{eye}]")
    return ";".join(eyes) + ";[e0][e1]hstack,format=yuv420p"


def encode(frames, crf, out, slice_params, height=HEIGHT, crop_y=None, src="testsrc2", gop=1, codec="h264"):
    # HEVC: x265 makes all-intra streams Main Intra (RExt), so look/text clips run at 72 fps with P frames
    vf = source(src, 72 if codec == "hevc" else None)
    if crop_y is not None:
        vf += f",crop={WIDTH}:{height}:0:{crop_y}"
    if codec == "hevc":
        subprocess.run([
            FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", vf,
            "-frames:v", str(frames), "-c:v", "libx265", "-profile:v", "main", "-preset", "ultrafast",
            "-tune", "zerolatency", "-crf", str(crf),
            "-x265-params", f"keyint={gop}:min-keyint={gop}:bframes=0:repeat-headers=1:aud=0:slices=1:log-level=error",
            "-f", "hevc", out], check=True)
        return
    subprocess.run([
        FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", vf,
        "-frames:v", str(frames), "-c:v", "libx264", "-profile:v", "baseline", "-level", "6.0",
        "-preset", "ultrafast", "-tune", "zerolatency", "-g", str(gop), "-bf", "0", "-crf", str(crf),
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
    bits = int.from_bytes(nal[1:9].ljust(8, b"\0"), "big")  # short (all-skip) slices: pad
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


def hevc_pictures(path):
    """HEVC stream with one slice per picture -> [(csd, slice NAL)]; csd = VPS+SPS+PPS in effect."""
    with open(path, "rb") as f:
        nals = split_nals(f.read())
    pics, ps = [], {}
    for nal in nals:
        t = nal[0] >> 1 & 0x3F
        if t in (32, 33, 34):
            ps[t] = nal
        elif t < 32:
            pics.append((b"".join(b"\x00\x00\x00\x01" + ps[k] for k in (32, 33, 34)), nal))
    return pics


def build(count, name, crf, layout, src, gop, codec="h264"):
    """-> per frame a list of slices (csd, nal, yOffset px, heightInMacroBlocks)."""
    es = os.path.join(ROOT, "capture", "runs", f"{name}.h264")
    os.makedirs(os.path.dirname(es), exist_ok=True)
    if codec == "hevc":
        if layout != "split5":
            raise SystemExit("hevc: only the split5 layout")
        h = HEIGHT // SLICES
        streams = []
        for i in range(SLICES):
            part = es.replace(".h264", f"-{i}.h265")
            encode(count, crf, part, "", h, i * h, src, gop, "hevc")
            streams.append(hevc_pictures(part))
        return [[(c, nal, i * h, h // 16) for i, (c, nal) in enumerate(pics)] for pics in zip(*streams)]
    if layout == "split5":
        h = HEIGHT // SLICES
        streams = []
        for i in range(SLICES):
            part = es.replace(".h264", f"-{i}.h264")
            encode(count, crf, part, "slices=1:threads=1", h, i * h, src, gop)
            streams.append(es_pictures(part))
        return [[(csd(sps, pps), nals[0], i * h, h // 16) for i, (sps, pps, nals) in enumerate(pics)]
                for pics in zip(*streams)]
    params = {"equal5": f"slices={SLICES}:threads={SLICES}", "single": "slices=1:threads=1"}.get(layout)
    if params is None:
        kind, n = layout.split(":")
        params = {"mbs": "slice-max-mbs", "bytes": "slice-max-size"}[kind] + f"={n}:threads=1"  # sliced threads would force >= threads slices
    encode(count, crf, es, params, src=src, gop=gop)
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
    # Split each payload over XRSP messages of at most this many bytes (0 = one message each)
    chunk = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    src = sys.argv[6] if len(sys.argv) > 6 else "testsrc2"
    gop = int(sys.argv[7]) if len(sys.argv) > 7 else 1  # 1 = all intra; else IDR every gop frames + P frames
    codec = sys.argv[8] if len(sys.argv) > 8 else "h264"
    pictures = build(count, name, crf, layout, src, gop, codec)

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
            intra = (nal[0] >> 1 & 0x3F) in (19, 20) if codec == "hevc" else nal[0] & 0x1F == 5
            hdr[d + 49] = intra | (2 if i == len(slices) - 1 else 0)  # isIntraFrame, isLastSliceOfAFrame
            # yOffset (px), heightInMacroBlocks, csdLength, videoLength
            struct.pack_into("<IIII", hdr, d + 4 * 13, y, h_mb, len(c), len(video))
            frame.append({**seg_table, "topic": topic})
            frame.append({"topic": topic, "flags": 0x10, "body": bytes(hdr).hex()})
            for payload in (c, video):
                step = chunk or len(payload) or 1
                for o in range(0, max(1, len(payload)), step):
                    flags, body = pad4(payload[o:o + step])
                    frame.append({"topic": topic, "flags": flags, "body": body.hex()})
        out_frames.append(frame)

    dest = os.path.join(ROOT, "out", "clips", f"{name}.json")
    with open(dest, "w", encoding="utf-8") as f:
        json.dump({"source": f"{src} {'x265' if codec == 'hevc' else 'x264'} crf{crf} {layout} gop{gop}",
                   "frame_index": 1, "setup": meta["setup"],
                   "frames": out_frames}, f)
    sizes = [sum(len(bytes.fromhex(m["body"])) for m in fr) for fr in out_frames]
    print(f"{name}: {len(out_frames)} frames, avg {sum(sizes) // max(1, len(sizes))} B/frame, "
          f"{sum(sizes) * 72 / max(1, len(sizes)) / 1e6 * 8:.0f} Mbit/s at 72 Hz -> {os.path.relpath(dest, ROOT)}")
    print(f"largest message {max(len(m['body']) // 2 for fr in out_frames for m in fr)} B")
    print(f"slice layouts (first MB row, rows), {len(layouts)} distinct:", sorted(layouts)[:3])


if __name__ == "__main__":
    main()
