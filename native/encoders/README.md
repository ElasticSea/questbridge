# Encoders

`encoder.hpp` is the platform-neutral H.264 interface: CPU frame (NV12/BGRA/RGBA) or GPU frame
(D3D11 texture, CVPixelBuffer) in, csd (SPS+PPS) plus slice NALs with their index and
`first_mb_in_slice` out. `h264.*` holds the shared Annex-B/SPS/slice-header parsing.

- `select.*`: which backend and layout to use on this machine (platform-neutral, unit-tested
  with fakes in `native/tests/test_encoder_select.cpp`). `strips.*`: `Layout::Strips` out of any
  one-picture encoder (one per strip, encoded in parallel).
- `mf/`: Windows Media Foundation, one specific MFT per encoder (hardware by vendor, or the
  Microsoft software MFT). `d3d11_input.*`: texture split into strips / zero-copy CPU read back.
- `openh264/`: Cisco OpenH264 2.7.0, the software fallback (Windows). Loaded at run time.
- `create_windows.cpp`: the Windows `create_encoder`.
- `vt/`: macOS VideoToolbox. `Layout::Strips`: one session per 4096x448 strip, each its own stream.

## Backend selection (Windows)

`create_encoder` lists the H.264 MFTs (hardware first, vendor from
`MFT_ENUM_HARDWARE_VENDOR_ID_Attribute`) plus OpenH264 if its DLL loads, and tries them in this
order, each with its vendor's settings and layouts:

| Backend (`--backend`, `QB_ENCODER`) | Layouts tried | Settings | Status |
|---|---|---|---|
| `mf-nvidia` (`nvidia`) | slices, strips | defaults as benched | slices bench-verified in the headset; strips pass qbenc + stress |
| `mf-amd` (`amd`) | strips, slices | QualityVsSpeed 33 | untested (no AMD GPU here) |
| `mf-intel` (`intel`) | slices, strips | QualityVsSpeed 33 | untested (no Quick Sync encoder here) |
| other hardware MFT | strips, slices | QualityVsSpeed 33 | untested |
| `openh264` | strips | CAVLC, 1 ref, no scene-change/frame skip | qbenc + stress pass; not yet in the headset |
| `mf-software` (Microsoft MFT) | strips | QualityVsSpeed 0 | rejected by the probe: 4 slices per 4096x448 strip |

Hardware on the adapter of the game's D3D11 device comes first (laptops: the dGPU running the
game, not the iGPU listed first); hardware on another adapter reads the texture back to CPU.
All MFTs get deblocking off, CAVLC, no B frames, 1 ref, level from the picture size (6.0 for
4096x2240, 4.2 for a 4096x448 strip), the slice cap re-encode (`AVEncVideoMinQP`), SPS marked
Constrained Baseline (if an MFT refuses ConstrainedBase and falls back to Baseline) and VUI timing.

Each attempt encodes a gray **probe frame** (forced IDR) and must pass `check_frame`: 5 slices
(slices: equal 28-row bands, `disable_deblocking_filter_idc` != 0; strips: first_mb 0 and own
SPS/PPS each), one NAL each, IDR, Constrained Baseline (profile 66 + constraint_set1), SPS size
matching the config (Quest 3 4096x2240, Pro 4096x2160). Anything else is skipped with the reason
(`Encoder::selection()`), so an encoder that ignores slice control or deblocking can't reach the
headset. The chosen backend's first real frame is an IDR again.

The runtime (`qb_submit_frame_texture`) logs `encoder: <tag>: <name>` and `encoder skipped: ...`
on the first frame and reports the tag + name in `qb_info.encoder` (API version 4);
`QB_ENCODER=nvidia|amd|intel|mf|mf-software|openh264|software` forces a backend.

### OpenH264 (software fallback)

- License: OpenH264 source is **BSD-2-Clause** (`openh264/LICENSE`; the headers in
  `openh264/include/wels` are vendored from tag v2.7.0). x264 was not used: GPL-2, incompatible
  with the planned Apache-2.0 release.
- Patents: H.264 royalties are covered only for **Cisco's prebuilt binary downloaded by the end
  user** (http://www.openh264.org/BINARY_LICENSE.txt), so the DLL is never shipped with our
  builds. `native\encoders\openh264\fetch.ps1` downloads `openh264-2.7.0-win64.dll` from
  ciscobinary.openh264.org, checks Cisco's SHA-256 and puts it in
  `%LOCALAPPDATA%\QuestBridge\openh264\`. Search order: `QB_OPENH264` (full path), next to
  `questbridge.dll` / `qbenc.exe`, then that folder; the version must match the headers (2.7).
  (Store Python's child processes can't see that AppData folder, so for `slice_cap_stress.py`
  copy the DLL next to qbenc.)
- Layout: 5 encoders of 4096x448, one per strip, on 5 threads; rate control per strip at
  bitrate / 5. IDRs come from our frame count, so all strips switch on the same frame.
- Slice cap: OpenH264's rate control never goes above QP 42, and noise at QP 42 is ~1.4 MB per
  strip. An oversized strip is re-encoded as an IDR at a fixed QP (extra encoders without rate
  control) and **stays on that QP floor** (P frames) until slices are under a quarter of the cap
  for half a second, then hands back to rate control with an IDR. Without the sticky floor,
  every noise/motion frame needed a re-encode (36-41 ms/frame).
- Speed (this PC, 5 threads): tiles 2.9 ms/frame (NV12), 5.2 ms (BGRA), 10.3 ms from a D3D11
  texture incl. read back and the test's upload; stress patterns 7-17 ms mean. 72 Hz holds on
  normal content; on worst-case noise/motion it drops toward ~60 fps (the runtime drops the
  oldest frame when the encoder is behind). No size reduction needed on this CPU.

## Build and run (Windows)

```
native\encoders\build.ps1
native\encoders\build\qbenc.exe --frames 144 --name qbenc
```

Writes `out/runs/<name>.h264` and `out/clips/<name>.json` (same clip format as
`tools/encode_pattern.py`; stream it with `XRSP_REPLAY=out/clips/<name>.json`).
Options: `--mbps 100`, `--vbr`, `--gop 72` (1 = all intra, 0 = first frame only),
`--input nv12|bgra|d3d11|d3d11-rgba`, `--software`, `--csd every|idr` (default every, like the x264
clip), `--backend` and `--layout auto|slices|strips` (see below; default auto).

## Results on the bench (RTX 5060, driver 591.59, 2026-10-28)

NVIDIA H.264 Encoder MFT, 4096x2240 @ 72, Constrained Baseline (constraint flags 0xc0), level 6.0:

- `CODECAPI_AVEncSliceControlMode=2` (MB rows), size 28 gives exactly 5 slices,
  `first_mb_in_slice` 0/7168/14336/21504/28672 on every frame (IDR and P).
- Encode, submit to output: ~7.5 ms/frame (P), ~6.9 ms (all intra). Whole `encode()` call:
  NV12 CPU 8.8 ms, BGRA CPU (parallel convert) 8.7 ms, D3D11 BGRA texture (video processor to
  NV12) 8.4 ms in the MFT, 10.7 ms including the test's 36 MB texture upload.
- CBR 100 Mbit/s on the test pattern: ~3.3 Mbit/s with P frames, 26.6 Mbit/s all intra (no filler).
- In-loop deblocking off (`CODECAPI_AVEnableInLoopDeblockFilter=0`, slices get
  disable_deblocking_filter_idc 1): the Quest decodes each slice on its own decoder. SPS
  fixed_frame_rate_flag is rewritten to 1 (`h264::set_fixed_frame_rate`). Compare headers with
  `py tools\h264_fields.py a.h264 b.h264`.
- Microsoft software MFT ignores slice control: 16 slices of uneven size, so no replay clip.

## Results on the Mac (M5 Pro, macOS 26.7.1, 2026-10-28)

Build and run: [docs/building.md](../../docs/building.md). `qbenc` defaults to `--layout strips`
on macOS; options as above plus `--input cvpixelbuffer`, `--low-latency`, `--max-slice-bytes`.

- **Layout:** 5 sessions of 4096x448 (`Layout::Strips`), Constrained Baseline (0xc0), level 4.2,
  one IDR/P slice per strip, `first_mb_in_slice` 0, own SPS/PPS each. Keyframes forced by the
  backend on the same frame in all strips. ffprobe decodes all 5 streams (144 frames) cleanly.
- **One full-size session is no alternative:** `kVTCompressionPropertyKey_MaxH264SliceBytes` is
  rejected by both H.264 encoders (always 1 slice), and there is no slice-count control.
- **Encoder:** default `com.apple.videotoolbox.videoencoder.ave.avc` (hardware). Low-latency RC
  (`--low-latency`) switches to `h264.rtvc`: same speed, but long-term refs, MMCO and ref list
  reordering in every P slice, so not the default. Neither writes a VUI: `h264::add_vui` adds
  timing 1000/144000 fixed and bitstream_restriction (reorder 0, dec buffering 1). Remaining
  header differences vs the working x264 strips: POC type 0 (x264 2), deblocking idc 0 (fine:
  no slice edge inside a strip picture), nal_ref_idc 1.
- **Speed (the blocker): one hardware encode engine, ~570 Mpx/s for H.264.** 5 strips, encode()
  per frame: NV12 16.5–17.5 ms, BGRA 18–19.5 ms, CVPixelBuffer BGRA 18.9 ms (locked row copy
  ~1.2 ms; VTPixelTransferSession crops cost ~12 ms). One 4096x2240 session: 15 ms. Pipelined
  (frames in flight, `vt/vtprobe.cpp`): 62 fps max at 4096x2240; two processes split it (31 + 31 fps).
  `PrioritizeEncodingSpeedOverQuality`, Quality 0, Constrained High: no gain. **72 Hz at
  4096x2240 is out of reach with H.264**; 3840x1920 as strips reaches ~74 fps, 1920x1088 217 fps.
  **HEVC is faster:** 4096x2240 72.8 fps (1 session), 5 strips 87.6 fps.
- **Real-time options (`qbenc`, encode() per frame, NV12):** `--codec hevc` (ave.hevc, Main, 5 x
  4096x448, one slice each, IDR 19/20) **10.9 ms**; `--size 2880x1600` H.264 (5 x 2880x320)
  **9.5 ms**, ~92 fps pipelined. Throughput varies ~20% with the Mac's desktop load (Aerial
  wallpaper, Unity open): 5 x 4096x448 H.264 measured 62 fps first, 50 fps later.
- **HEVC strips in the native core (2026-10-28):** the session sends the codec in CONFIG v3
  (`qb_config.codec` / `QB_CODEC`, auto = HEVC on macOS unless the HELLO v3 lists codecs without HEVC; Quest 3/Pro
  list nothing and the Pro decodes HEVC, so HEVC on the Mac, H.264 on Windows), and the Metal texture encoder follows it. ave.hevc strip headers:
  VPS + SPS + PPS per strip (csd), one slice segment per strip, IDR_N_LP (20) on forced keys in
  every strip at once, no reordering (max_num_reorder 0, dec buffering 5), CTB 32, level 5.2,
  POC lsb 11 bits, temporal MVP on, SAO on, **WPP on (entropy_coding_sync, entry points in the
  slice header)**, loop filter across slices off, no VUI. 432 px strips (Quest Pro) encode without
  a conformance window issue. `questbridge_vt_tests`: HEVC 4096x2240 11.6 ms/frame, 4096x2160 11.8,
  H.264 17.4 (sequential); every strip decodes on its own decoder incl. P frames. Noise at
  400 Mbit/s: first HEVC try up to 1.7 MB per strip, refit lands under 120,000 B in 2 rounds
  (slice QP isn't parsed for HEVC, so the refit starts from QP 20); even QP 51 noise is ~100 KB.
- Strip sizes: `DataRateLimits` caps each strip at 120,000 B per frame duration; on the test
  pattern at 100 Mbit/s CBR the largest strip was 12 KB (P), 8.5 KB (IDR).

## Slice cap (both backends, 2026-10-28)

Every slice NAL must fit one XRSP message (`EncoderConfig::max_slice_bytes`, 120,000 B); the
headset ends the session on larger messages. Rate control doesn't guarantee it, so `encode()`
re-encodes a frame (MF) or strip (VT) with a slice over the cap as an IDR at a higher QP
(+6 QP per halving needed, +2), and fails rather than returning an oversized slice.
`EncodedFrame::refits` / `first_try_bytes` and `Slice::qp` report it; the runtime logs each one.

- MF: `CODECAPI_AVEncVideoMinQP` for the re-encode, then back to 0.
- VT: `MinAllowedFrameQP` is ignored and ave.avc has no `BaseFrameQP`, so each strip has 3 extra
  sessions without rate control at `Quality` for QP 39/45/51 (Quality is read only up to a
  session's first frame; one gray frame each warms them up in init, else the first use costs
  ~40 ms). ~35 hardware sessions per process is the limit (5 main + 15 refit = 20).

`py tools/slice_cap_stress.py [-- --codec hevc | --size 2880x1600]` (qbenc `--pattern
noise|motion|cuts`, gop 1 and 72, 72 frames; also in the full `tools/test_all.ps1` run):

| Backend | Largest slice | Rejected (first try) | Re-encoded frames | QP normal → re-encoded | encode() max |
|---|---|---|---|---|---|
| MF NVIDIA, 4096x2240 H.264 | 96,662 B | up to 1,793,380 B | 1–4 of 72 | cuts: 21–33 → 45–50 | 28 ms (normal 8–12) |
| VT M5 Pro, 5 x 4096x448 H.264 | 118,787 B | up to 1,465,087 B | 4–11 of 72 | cuts: 26–37 → 46–47 | 39 ms (normal 17–18) |
| VT, HEVC 5 x 4096x448 | 119,065 B | up to 1,689,673 B | 5–11 of 72 (up to 17 encodes) | (QP not parsed) | 40 ms (normal 11–15) |
| VT, H.264 5 x 2880x320 | 112,435 B | up to 845,521 B | 8–11 of 72 | cuts: 21–31 → 45 | 30 ms (normal 10–12) |
| MF NVIDIA strips, 5 x 4096x448 | 96,658 B | up to 1,122,799 B | 1–4 of 72 | cuts: 21–33 → 45–50 | 27 ms (normal 7–11) |
| OpenH264 strips, 5 x 4096x448 | 84,887 B | up to 1,744,659 B | 1 of 72 (then QP floor) | cuts: 39–41 (incl. floor frames) → 46 | 61 ms (normal 7–17) |

Quality cost: only the re-encoded frame, one IDR at QP ~45–51 (blocky on detail) instead of
dropping it, plus one extra encode pass (MF +10–20 ms, VT +8–18 ms per pass). On noise/motion
content QP is already ~50 at 100 Mbit/s, so there is no visible difference. Pure noise at QP 51
is ~93–119 KB per 4096x448 strip, close to the cap: past that `encode()` reports an error.
