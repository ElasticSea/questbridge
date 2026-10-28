# Encoders

`encoder.hpp` is the platform-neutral H.264 interface: CPU frame (NV12/BGRA) or GPU frame
(D3D11 texture; later CVPixelBuffer) in, csd (SPS+PPS) plus slice NALs with their index and
`first_mb_in_slice` out. `h264.*` holds the shared Annex-B/SPS/slice-header parsing.

- `mf/`: Windows Media Foundation. Uses the GPU's hardware MFT (async), software MFT as fallback.
- VideoToolbox (macOS): to do, behind the same interface.

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
