# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""Decode the XRSP audio streams (topic 6) and audio control (topic 26).

Topic 6 carries one single-segment Cap'n Proto message per 10 ms block, sent
as two XRSP messages: an 8-byte segment table (flags 0x10), then the segment
(flags 0x18, padded so the whole XRSP message is a multiple of 1024 B; last
body byte = pad count, filler 0xDE). Segment layout:
  0x00 ptr  root struct, 1 data word, 1 pointer (00000000 01000100)
  0x08 u64  timestamp, ns, in the SENDER's clock (PC: QPC ns, Quest: own ns),
            ~= time the block was handed to the transport
  0x10 ptr  list of bytes (Data), offset 0, length n
  0x18 ...  n bytes of PCM

PC->Quest (headphones): n = 3840 = 480 frames of 48 kHz stereo float32,
interleaved L R. Quest->PC (mic): n = 960 = 480 frames of 48 kHz mono int16.
Both 100 blocks/s (200 XRSP messages/s). This is raw PCM: the PC did not
select session feature 6 (Opus) in CONFIG.

Topic 26 AudioControl: 2-word struct, sent once each way at session start.
  Quest->PC 03 00 02 00 01 00 00 00 | 00 00 00 00 00 00 80 3f
  logged as "source = 3, headphones = (0.0, 0), mic = (1.0, 1)"

Usage: py decode_audio.py <capture.pcap> <device> [count] [--wav [outdir]]
"""
import os
import struct
import sys
import wave

from xrsp import encode_message, messages

RATE = 48000
FRAMES = 480  # per 10 ms block
PC_TO_QUEST = {"channels": 2, "width": 4, "format": 3}  # WAVE_FORMAT_IEEE_FLOAT
QUEST_TO_PC = {"channels": 1, "width": 2, "format": 1}  # PCM int16
PAD_BLOCK = 1024


def unpad(flags, body):
    return body[:-body[-1]] if flags & 0x08 else body


def parse_segment(seg):
    """Return (timestamp_ns, pcm bytes) from an audio Cap'n Proto segment."""
    if seg[:8] != bytes.fromhex("0000000001000100"):
        raise ValueError(f"unexpected root pointer {seg[:8].hex()}")
    ts, lo, hi = struct.unpack_from("<QII", seg, 8)
    if lo != 1 or hi & 7 != 2:
        raise ValueError(f"expected byte list pointer, got {seg[16:24].hex()}")
    n = hi >> 3
    return ts, seg[24:24 + n]


def encode_audio(ts, pcm, seq):
    """Build the two raw XRSP messages (segment table, segment) for one block,
    byte-identical to Meta's host. `seq` is the topic-6 seq of the first."""
    seg = (bytes.fromhex("0000000001000100") + struct.pack("<QII", ts, 1, len(pcm) << 3 | 2)
           + pcm + bytes(-len(pcm) % 8))
    table = struct.pack("<II", 0, len(seg) // 8)
    pad = -(8 + len(seg)) % PAD_BLOCK
    flags = 0x10
    if pad:
        flags |= 0x08
        seg += b"\xde" * (pad - 1) + bytes([pad])
    return (encode_message(0x10, 6, seq, table), encode_message(flags, 6, (seq + 1) & 0xFFFF, seg))


def blocks(path, device):
    """Yield (capture_ts, to_quest, seq, timestamp_ns, pcm) per audio block."""
    pending = {}
    for m in messages(path, device):
        if m.topic != 6:
            continue
        body = unpad(m.flags, m.body)
        if len(body) == 8:
            pending[m.to_quest] = struct.unpack("<II", body)
            continue
        table = pending.pop(m.to_quest, None)
        if table is None or table[1] * 8 != len(body):
            continue  # lost segment table (capture start / truncation)
        ts, pcm = parse_segment(body)
        yield m.ts, m.to_quest, m.seq, ts, pcm


def control(path, device):
    """Yield (capture_ts, to_quest, data words) for topic-26 AudioControl."""
    for m in messages(path, device):
        if m.topic == 26 and len(m.body) > 8:
            yield m.ts, m.to_quest, m.body[8:]


def show(path, device, count):
    for t, q, data in control(path, device):
        src, kind, flags, hp, mic = struct.unpack_from("<HHIff", data)
        print(f"t={t:.4f} {'PC->Q' if q else 'Q->PC'} AudioControl {data.hex(' ')}"
              f"  (u16 {src}, u16 {kind}, u32 {flags:#x}, f32 {hp}, f32 {mic})")
    shown = {True: 0, False: 0}
    last = {}
    for t, q, seq, ts, pcm in blocks(path, device):
        if shown[q] >= count:
            continue
        shown[q] += 1
        dt = (ts - last[q]) / 1e6 if q in last else 0
        last[q] = ts
        if q:
            v = struct.unpack(f"<{len(pcm) // 4}f", pcm)
            peak = f"peak {max(map(abs, v)):.2e} (f32 stereo)"
        else:
            v = struct.unpack(f"<{len(pcm) // 2}h", pcm)
            peak = f"peak {max(map(abs, v))} (i16 mono)"
        print(f"t={t:.4f} {'PC->Q' if q else 'Q->PC'} seq={seq} ts={ts / 1e9:.6f}s "
              f"dt={dt:6.2f}ms pcm={len(pcm)}B {peak}")


def export_wav(path, device, outdir):
    os.makedirs(outdir, exist_ok=True)
    data = {True: bytearray(), False: bytearray()}
    for _, q, _, _, pcm in blocks(path, device):
        data[q] += pcm
    base = os.path.splitext(os.path.basename(path))[0]
    for q, fmt, name in ((True, PC_TO_QUEST, "pc-to-quest"), (False, QUEST_TO_PC, "mic")):
        out = os.path.join(outdir, f"{base}-{name}.wav")
        if fmt["format"] == 1:
            with wave.open(out, "wb") as w:
                w.setnchannels(fmt["channels"])
                w.setsampwidth(fmt["width"])
                w.setframerate(RATE)
                w.writeframes(bytes(data[q]))
        else:
            write_float_wav(out, bytes(data[q]), fmt["channels"])
        print(f"{out}: {len(data[q]) / (fmt['channels'] * fmt['width'] * RATE):.2f} s")


def write_float_wav(out, pcm, channels):
    # The wave module only writes integer PCM; write the float header by hand.
    align = channels * 4
    fmt = struct.pack("<HHIIHH", 3, channels, RATE, RATE * align, align, 32)
    with open(out, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(pcm)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)


if __name__ == "__main__":
    args = sys.argv[1:]
    wav = "--wav" in args
    if wav:
        i = args.index("--wav")
        outdir = args[i + 1] if i + 1 < len(args) else os.path.join("capture", "runs")
        args = args[:i]
    path, dev = args[0], int(args[1])
    if wav:
        export_wav(path, dev, outdir)
    else:
        show(path, dev, int(args[2]) if len(args) > 2 else 10)
