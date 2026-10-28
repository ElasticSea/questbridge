# Encoders

`encoder.hpp` is the platform-neutral H.264 interface: CPU frame (NV12/BGRA) or GPU frame
(D3D11 texture; later CVPixelBuffer) in, csd (SPS+PPS) plus slice NALs with their index and
`first_mb_in_slice` out. `h264.*` holds the shared Annex-B/SPS/slice-header parsing.

- `mf/`: Windows Media Foundation. Uses the GPU's hardware MFT (async), software MFT as fallback.
- `vt/`: macOS VideoToolbox. `Layout::Strips`: one session per 4096x448 strip, each its own stream.

## Build and run (Windows)

```
native\encoders\build.ps1
native\encoders\build\qlenc.exe --frames 144 --name qlenc
```

Writes `capture/runs/<name>.h264` and `host/replay/<name>.json` (same clip format as
`tools/encode_pattern.py`; stream it with `XRSP_REPLAY=host/replay/<name>.json`).
Options: `--mbps 100`, `--vbr`, `--gop 72` (1 = all intra, 0 = first frame only),
`--input nv12|bgra|d3d11`, `--software`, `--csd every|idr` (default every, like the x264 clip).

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

Build and run: [docs/MAC_SETUP.md](../../docs/MAC_SETUP.md). `qlenc` defaults to `--layout strips`
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
- Strip sizes: `DataRateLimits` caps each strip at 120,000 B per frame duration; on the test
  pattern at 100 Mbit/s CBR the largest strip was 12 KB (P), 8.5 KB (IDR).
