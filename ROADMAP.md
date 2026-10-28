# Quest Link for Mac — Roadmap

**Target:** a Unity game on the Mac (Editor Play mode and standalone) streams to the Quest over USB via Meta's own Link protocol, and the headset sends its sensors back to Unity: head pose, controllers and input, hand tracking, passthrough. Windows PC VR games (Wine) are out of scope for now. Rationale and alternatives: [docs/ROADMAP_REVIEW.md](docs/ROADMAP_REVIEW.md).

## Components
> Main path: Meta's Link protocol, no headset install. Fallback if a gate fails: our own Quest APK (needs the user's decision).

- [~] **1. USB transport** — WinUSB on the Windows bench works; macOS via libusb still to do
- [~] **2. Link protocol** — handshake, video (H.264 slices), head pose, audio decoded; encoder format gate (which slice layouts the headset accepts); controllers, hands, passthrough being researched
- [ ] **3. Mac host** — native dylib (libusb, XRSP, handshake, clock sync, pacing, VideoToolbox) with a small C API (`ql_get_pose`, `ql_submit_frame`, …) plus a CLI
- [~] **4. Unity bridge** — C# package `unity/com.elasticsea.questlink` done against an assumed C API (mock backend, tests pass); native hookup pending. C# two-eye rig → one RenderTexture → Metal/IOSurface → VideoToolbox; poses and input back as Input System XRHMD/XRController devices (TrackedPoseDriver/XRI work unchanged). An XR SDK display provider comes later.
- [ ] **5. Sensors** — controllers + buttons + haptics, hand-tracking joints, passthrough as a headset-composited layer (alpha blend). Raw camera pixels are APK-only. Eye/face: no Quest 3 hardware.
- [ ] **6. Supporting work** — FOV/IPD from the HELLO device description, latency tuning (pose prediction, pacing, bitrate), firmware-version pinning, menu-bar app

## Milestones
- [x] M0: USB captures + protocol spec (handshake, video slices, pose, audio)
- [x] M1: own host completes handshake + session (Windows bench; Mac in M4)
- [x] M2: video into the headset: own x264 test pattern at 72 fps, 0 glitches
- [x] M2a: encoder gate (bench): headset accepts a VideoToolbox-compatible stream: 5 independent 4096×448 streams (one decoder per strip) at 72 fps incl. P frames; each slice message ≤ ~128 KiB; <5 slices not shown with CONFIG slices=5 (2026-10-28)
- [x] M2b: timewarp check: constant renderPose stays world-locked, needs rectifyMeshId=1001 + per-eye FOV in EncodedSlice (2026-10-28)
- [ ] M2c: sensor gate (bench): hands + controller input + haptics found on the wire via Meta-Link captures with the Unity sensor-probe app; feature bits identified
       → if M2a or M2c fails: decide on the own-APK path before M3
- [ ] M3: Mac encoder bench: VideoToolbox 4096×2240 (or chosen size) in real time, slice layout from M2a
- [ ] M4: Mac host CLI: libusb transport, handshake, clock sync, VideoToolbox test pattern in the headset
- [ ] M5: native dylib with C API (pose out, frame in) + CLI on top; fixtures/tests reuse it
- [ ] M6: Unity package v0: two-eye rig → RT → Metal/IOSurface → VideoToolbox; head pose via Input System; scene visible in headset from Editor Play mode
- [ ] M7: controllers: poses + buttons/sticks as XRController devices, haptics; works with XRI
- [ ] M8: hand tracking: joints decoded → C# hands API (XR Hands subsystem if cheap)
- [ ] M9: passthrough: alpha-blend frames + passthrough layer; Unity transparent clear
- [ ] M10: standalone macOS player build; latency tuning
- [ ] later: environment depth / body tracking over Link; XR SDK display provider; raw camera pixels via a small APK; 90/120 Hz modes; menu-bar app
