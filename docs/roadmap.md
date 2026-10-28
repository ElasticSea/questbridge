# Roadmap

## Working today

- Wired Link protocol: handshake, clock sync, video, head pose, controllers, hands, audio both ways, haptics.
- Native library with a C API on macOS (Apple silicon) and Windows; the `qbhost` CLI.
- Video: VideoToolbox HEVC on macOS (zero-copy from Metal), Media Foundation H.264 per GPU vendor on Windows, OpenH264 fallback.
- Unity package: rig prefab, XR Plug-in Management loader, Input System devices, XR Hands provider, audio and microphone, connection events, Check Setup.
- Connecting without replugs, and recovering inside a running app after disconnects.

## Next

- Hardware coverage: Quest 3S and Quest 2, AMD and Intel GPUs on Windows, more Macs.
- Latency: pose prediction, pacing and bitrate tuning.
- 90 and 120 Hz display modes.
- Passthrough as a composited layer, with a transparent clear in Unity.

## Later

- A native XR SDK display/input provider (single-pass rendering), alongside the managed loader.
- Environment depth and body tracking over the link.
- A small menu-bar app for using QuestBridge without Unity.

## Open / in progress (2026-10-28)
Verified on hardware: Windows Editor Play mode on Quest 3 (head, hands, controllers + haptics, audio, connect without replug, sleep recovery, OpenH264 picture); Mac → Quest Pro video (H.264 and HEVC clips, 72 fps).

- [ ] USB write size (first to fix): the host writes a whole frame as one USB bulk transfer; the headset drops writes over ~256 KiB ("buffer too small"). Meta writes one transfer per slice (max ~214 KB). This caused the Mac Editor → Quest Pro drops and the OpenH264 drops on Quest 3. An unbuilt fix (cap each write at 128 KiB / one slice per transfer) is on branch `claude/compassionate-northcutt-c9782b` (8a9747c): build, test, verify on both headsets.
- [ ] Link quit while streaming: a new HELLO from the headset is ignored, so the host stays "streaming". Restart the handshake on any new HELLO.
- [ ] MF strips encoder (AMD/Intel path): picture quality is weak on Quest 3.
- [ ] Passthrough / environment depth: offered by the runtime, but Meta's AR Foundation plugin fails in a Windows player ("instanceProcAddr is null").
- [ ] Headset media volume can't be read or set over adb; audio checks need an in-headset volume step.
- [ ] Not yet verified on hardware: XR Plug-in Management loader (unchanged XRI project), tester package OVRService handling (UAC), left controller.
- [ ] Final hardware pass after the above: Quest 3 (Windows Editor) and Quest Pro (Mac Editor).
