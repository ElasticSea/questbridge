# Quest Link for Mac — Roadmap

## Components
> Approach: speak Meta's own Link protocol so the Quest's built-in "Enable Link" works — no APK.

- [ ] **1. USB transport** — raw USB on macOS (IOKit/libusb), matching the interfaces/endpoints the Quest exposes in Link mode
- [ ] **2. Link protocol reverse-engineering** — capture Windows PC ↔ Quest traffic (USBPcap + Wireshark); decode handshake/auth, video, pose, audio, haptics; write a spec; reimplement on Mac
- [ ] **3. Mac streaming server** — VideoToolbox low-latency HEVC (sliced), A/V down, poses up, prediction, clock sync (<50 ms motion-to-photon)
- [ ] **4. macOS OpenXR runtime** — loader-compatible `.dylib` + JSON manifest; HMD/controllers/swapchains → server
- [ ] **5. PC VR game bridge** — Wine/CrossOver/GPTK (D3D→Metal); bridge OpenVR/OpenXR from Wine to native runtime; IOSurface/Metal texture sharing
- [ ] **6. Supporting work** — distortion/IPD/FOV per Quest model, guardian/passthrough, menu-bar app + installer, handling Quest firmware protocol changes

## Milestones
- [~] M0: USB captures from Windows Link session + protocol spec (handshake fully decoded; video/pose layouts next)
- [ ] M1: Mac completes Link handshake; Quest enters Link mode
- [ ] M2: Test pattern on Quest + pose round-trip
- [ ] M3: Native Metal VR demo
- [ ] M4: OpenXR runtime
- [ ] M5: Wine/GPTK bridge for real PC VR games
