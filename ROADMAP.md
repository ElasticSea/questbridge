# Quest Link for Mac — Roadmap

**Target (2026-10-28):** a Unity game on the Mac streams to the Quest over USB via Meta's own Link protocol, and the headset streams its sensors back to Unity: head pose, controllers, hand tracking, and passthrough if Link carries it. Running Windows PC VR games (Wine) is out of scope for now.

## Components
> Approach: speak Meta's own Link protocol so the Quest's built-in Link works — no APK.

- [~] **1. USB transport** — WinUSB on the Windows test bench works; macOS via libusb/IOKit still to do
- [~] **2. Link protocol reverse-engineering** — handshake, video (H.264 slices), head pose, audio decoded; controllers, hand tracking, passthrough being researched
- [ ] **3. Mac streaming host** — handshake, clock sync, VideoToolbox H.264 (Constrained Baseline, 5 slices, 4096×2240), audio, sensors in, pose prediction
- [ ] **4. Unity bridge** — native macOS plugin + C# package: Unity renders both eyes to Metal textures → host encodes; host → Unity: head/controller/hand poses, input, passthrough. Exposed as a Unity XR provider (or simple MonoBehaviour API first)
- [ ] **5. Sensors** — controllers + buttons, hand-tracking joints, passthrough (camera frames or a passthrough layer), eye/face if available
- [ ] **6. Supporting work** — lens distortion/FOV/IPD from the HELLO device description, latency tuning, menu-bar app, firmware-change handling
- [ ] *(later, optional)* OpenXR runtime for non-Unity apps; Wine/GPTK bridge for PC VR games

## Milestones
- [x] M0: USB captures from Windows Link session + protocol spec (handshake, video H.264 slices, pose, audio decoded)
- [~] M1: Link handshake + session from our host — done on Windows (2026-10-28); still to run on a Mac
- [~] M2: Video into the headset — replayed Meta frame shows at 72 fps; own encoded test pattern in progress
- [ ] M3: Host on macOS: same handshake + VideoToolbox encoder, test pattern in the headset
- [ ] M4: Sensors decoded: controllers, hands, passthrough (whatever Link carries)
- [ ] M5: Unity bridge: a Unity scene renders in the headset with head + controller tracking
- [ ] M6: Hands and passthrough delivered into Unity
