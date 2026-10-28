# QuestBridge (Unity package)

`com.elasticsea.questbridge` streams a Unity scene to a Quest over USB through the native `libquestbridge` (Meta's Link protocol, no headset APK) and brings head and controller poses back as Input System XR devices.

Unity 6000.3, Input System 1.19. Works in Editor Play mode. Without the native library the rig uses a mock backend.

## Native plugin (Windows)
`unity\install-native.ps1` builds `questbridge.dll` (Release, `native\build.ps1`, C++ tests included) and copies it to `Runtime/Plugins/Windows/x86_64/` with Editor + Win64 import settings. The binary is git-ignored (releases ship it as a download); `-NoBuild` only copies. macOS: `unity/install-native.sh` on the Mac builds `Runtime/Plugins/macOS/libquestbridge.dylib` (arm64) the same way. Close the Unity Editor first: Windows locks a loaded DLL until the Editor exits.

The Windows backend needs Direct3D11 (the encoder copies the `ID3D11Texture2D`); with another graphics API the rig warns and falls back to the mock. Set `QB_LOG_FILE=<path>` (and `QB_LOG=1` for every protocol message) before starting the Editor to get the native log, because the Editor drops plugin stderr. OVRService holds the XRSP interface while it runs: when the Quest is plugged in, the library stops it through one elevated helper (one UAC prompt per Editor session) that starts it again when the Editor exits (`QB_OVR_STOP=0` before starting the Editor turns this off; then `tools\windows\ovr_service.ps1 stop` by hand). **QuestBridge > Check Setup** shows what's missing on a machine.

First run: [Getting started](../../docs/getting-started.md). Scripting: [QUICKSTART.md](QUICKSTART.md).

## Editor Play mode
- **Drop-in prefab.** `Runtime/Prefabs/QuestBridge Rig.prefab` (GameObject → XR → QuestBridge Rig): rig + `QuestBridgeHands` + `HandJointsVisualizer`, `Head` with the AudioListener and `QuestBridgeAudio`, and `Left/Right Controller` on stock TrackedPoseDrivers with a block shown only while tracked (`QuestBridgeTrackedVisual`). No cameras in the prefab: the eyes are created at Play, so nothing renders in Edit mode. Regenerate with `BridgeTest/Assets/Editor/BuildRigPrefab.cs`.
- **One native runtime per Editor process.** The Editor never unloads `questbridge.dll`; its runtime (USB, handshake, encoder) starts at the first Play and keeps the headset session across Stop/Play and domain reloads. Each Play opens one `qb_session` and closes it on Stop (or on a script reload while playing), so nothing native leaks between Play sessions. `qb_shutdown` is only for handing the headset to another host.
- **While playing** (rig `headsetFramePacing`, and always for the XR loader's `QuestBridgeXRDriver`; `QuestBridgePacing`): `Application.runInBackground = true`, vSync off and `targetFrameRate` = the headset's refresh rate, so the Editor keeps streaming at 72 Hz when unfocused or minimized and doesn't render frames the encoder drops; all restored on Stop.
- **Main Camera** (`takeOverMainCamera`): eyes copy its settings when `cameraTemplate` is empty, it is disabled (with its AudioListener when the rig has its own) and the left eye is tagged `MainCamera`; restored on Stop.
- **Game view** (`mirror`): left eye (default), both eyes, or nothing; a culling-mask-0 camera clears the display under it.
- **No headset:** `qb_open` and the per-frame calls never block; USB is polled on the runtime's own thread. The rig logs its state changes and, after 15 s without streaming, a hint (OVRService / press Launch). The rig inspector shows the state and setup problems with fixes (Direct3D 11, missing `questbridge.dll`, Input System off).

## Use
1. Add the package: Package Manager → Add package from git URL → `https://github.com/ElasticSea/questbridge.git?path=/unity/com.elasticsea.questbridge#v0.2.0` (testers: [docs/getting-started.md](../../docs/getting-started.md)), or from disk (`package.json`, or `"com.elasticsea.questbridge": "file:<path>"` in `manifest.json`).
2. Put `QuestBridgeRig` on an empty GameObject at the tracking origin (floor level). It creates `Head/LeftEye` and `Head/RightEye` cameras. Use `cameraTemplate` to copy clear flags, culling mask and similar settings onto both eyes.
3. Bind to the generic XR paths with stock components such as TrackedPoseDriver or XRI: `<XRHMD>/centerEyePosition`, `<XRController>{LeftHand}/devicePosition`, `{RightHand}/{TriggerButton}`, `/primary2DAxis`, …
4. Hands: add `QuestBridgeHands` to the rig object; `HandJointsVisualizer` draws each joint as a sphere. With Unity XR Hands (`com.unity.xr.hands` 1.3+) in the project, also add `QuestBridgeXRHands` to run an `XRHandSubsystem` fed by QuestBridge, so XR Hands visualizers and XRI hand interaction work; parent their visuals to the rig (joint poses are relative to the tracking origin).
5. Sample: Package Manager → QuestBridge → Samples → Basic Scene.
6. Disconnects: keep playing. libquestbridge recovers by itself (USB glitch: resumes the stream; Link quit, cable, headset sleep or reboot: new handshake, the user presses Launch). Add `QuestBridgeConnectionStatus` next to the rig for `Connected` / `Disconnected(reason)` / `Reconnecting(status)` events and a Game-view banner while the link is down.

Don't add a TrackedPoseDriver to the rig's `Head`, because the rig drives it directly.

## XR Plug-in Management (unchanged XRI / XR Origin projects)
Instead of a `QuestBridgeRig`, an existing XR project can stream as is: Project Settings → XR Plug-in Management → Windows, Mac & Linux → untick OpenXR (or move it below), tick **QuestBridge**, press Play. Needs XR Plug-in Management 4+ (XRI projects have it); the loader assembly compiles only then.
- `QuestBridgeLoader` starts a hidden `QuestBridgeXRDriver` (DontDestroyOnLoad). It streams `Camera.main` (the XR Origin camera): two hidden eye cameras are added under it (copying its settings), render into the same side-by-side texture as the rig and submit with the camera's tracking-space pose, i.e. exactly the pose the frame was rendered from. The camera itself keeps rendering to the Game view as the mirror.
- Head, controllers and haptics are the same Input System devices as with the rig, so XR Origin's `TrackedPoseDriver` (`<XRHMD>/centerEye…`) and XRI's `<XRController>{LeftHand}` bindings work unchanged. The head pose is late-latched inside the Input System's before-render update, so `TrackedPoseDriver` renders with the newest sample.
- Hands: `QuestBridgeHands` plus, with XR Hands installed, the `QuestBridge-Hands` `XRHandSubsystem`. Audio/microphone components with no rig use the loader's backend.
- Settings (Project Settings → XR Plug-in Management → Quest Link): backend (Auto/Native/Mock), tracking origin (Device: Camera Offset = XR Origin's Camera Y Offset; Floor: 0) and hand tracking.
- Why managed and not a native XR SDK display/input provider: Unity's `XRDisplaySubsystem`/`XRInputSubsystem` can only be implemented natively against the XR SDK headers, which Unity hands out only after an access sign-up (they are not in the Editor's PluginAPI). Everything an XRI project actually consumes (Input System devices, XR Hands, the camera transform) works without them. What is missing: `XRSettings.enabled` stays false, no single-pass instanced rendering (two passes, like the rig), and the mono mirror camera still renders. A native provider can replace the driver later without changing the loader's surface.
- Don't combine it with a `QuestBridgeRig` in the same scene (both own the Input System devices).

## How it works
- **Stereo texture.** One encode-sized RT, 4096×2240 on Quest 3, from the HELLO device description, in BGRA32 sRGB. The left eye renders into the left half and the right eye into the right half. Each eye camera gets an off-axis projection built from the per-eye FOV tangents (`QuestBridgeMath.Projection`) and sits ±IPD/2 from the head.
  - Left half = left eye, confirmed on the bench (docs/protocol/spec.md). The native layer sends each frame with rectifyMeshId 1001 and the device's per-eye FOV, which the headset needs to reproject (timewarp) to the frame's renderPose.
  - Unity stores render textures bottom row first where UV v=0 is the top of memory (D3D11, Metal), so the native backend blits a vertically flipped copy and submits that; the encoder reads rows top-down.
- **Late latch.** In `Application.onBeforeRender` the rig reads the newest head pose, moves `Head`, and stores that exact pose with an increasing `FrameIndex`.
- **Submit.** Once both eye cameras have rendered (`Camera.onPostRender` in the built-in pipeline, end of the SRP context otherwise), the rig calls `SubmitFrame(rt, renderPose, frameIndex)` once. The native backend passes the (flipped) texture's `GetNativeTexturePtr()` (`ID3D11Texture2D*` on D3D11, `id<MTLTexture>` on Metal) to the render thread through `CommandBuffer.IssuePluginEventAndData`.
- **Input.** `QuestBridgeHMD : XRHMD` and two `QuestBridgeController : XRControllerWithRumble`, with `LeftHand`/`RightHand` usages. Control names follow Unity's Oculus Touch layout, and usages follow XR feature usages (`TriggerButton`, `GripButton`, `Primary2DAxis`, `PrimaryButton`, `MenuButton`, …). `SendImpulse` and any `SendHapticImpulseCommand` go to the backend's `SendHaptic`.
- **Hands.** libquestbridge turns hand tracking on once streaming (InputControl on topic 28; `QB_HANDS=0` before starting the Editor keeps it off) and parses topic 8. `QuestBridgeHands` reads both hands in `Update` and again before rendering: 26 OpenXR joints (pose + radius; Unity XR Hands axes, +Z toward the fingertip, +Y back of the hand), confidence, pinch strength per finger, aim ray, dominant hand. The mock backend animates two hands curling their fingers through a C# port of the native forward kinematics (`QuestBridgeHandSkeleton`).
- **Defaults.** Quest 3 values from docs/protocol/spec.md: FOV in up/down/left/right degrees is 44/55/54/40 for the left eye and mirrored for the right, IPD is 63.5 mm, and the stream runs at 72 Hz. The native layer overrides them when it reports device info.

## Audio
- **Game audio → headset.** Add `QuestBridgeAudio` next to the scene's AudioListener (ideally on the rig's `Head`). It reads the final mix in `OnAudioFilterRead`, downmixes to stereo and calls `qb_submit_audio`; the native side resamples to 48 kHz and sends a 10 ms block every 10 ms (silence when the queue runs dry). `muteLocal` (default on) silences the PC speakers.
- **Headset mic → Unity.** `QuestBridgeMicrophone` pulls the Quest mic (48 kHz mono) every frame into a 1 s buffer: `Read(samples, offset, count)`, `Level` (RMS), `Clear()`. `playThrough` plays it live through an AudioSource on the same GameObject (40 ms jitter cushion); since that also reaches the headset through `QuestBridgeAudio`, keep the gain low or expect feedback.
- FirstLight (`audioTest`, on by default) runs `AudioBench`: beeps every 2 s, records 4 s of mic, plays it back, with instructions and a mic level bar in the headset.

## Coordinates
The C API uses raw Link convention: right-handed, +Y up, −Z forward, metres, and Quest-clock nanoseconds. C# converts by mirroring Z: position `(x, y, −z)`, quaternion `(−x, −y, z, w)` (`QuestBridgeMath.LinkToUnity`). It converts back the same way for the submitted renderPose.

## Native API
The C# side calls the C API in `native/include/questbridge.h` through `Runtime/Native/QuestBridgeNative.cs`, the only file that declares native entry points. If an entry point is missing, the rig logs a warning and falls back to the mock backend in `Auto` mode.

## Tests
`unity/BridgeTest` references this package locally. From the repo root:

```
powershell -ExecutionPolicy Bypass -File tools\unity_hidden.ps1 -ArgLine "-batchmode -nographics -projectPath unity/BridgeTest -runTests -testPlatform EditMode -testFilter ElasticSea.QuestBridge.Tests -testResults results.xml"
```

The Unity Editor is only ever started through `tools\unity_hidden.ps1` (hidden desktop, so it can't take focus); `tools\test_all.ps1` does this for every suite. Players that stream to the headset go through `tools\start_player.ps1` (minimized, not activated, focus guard) or `unity\run-firstlight.ps1`. Use `-testPlatform PlayMode` for the smoke test. Unity runs get `QB_USB=0`, so a loaded `questbridge.dll` never opens the headset; `EditorPlayTests` then plays, stops and plays a native rig twice (with and without domain reload) on Direct3D 11, and `test_all` repeats the PlayMode suite in a real, minimized Editor (`SubmitsAtHeadsetRateWhileMinimized`). Live Editor check with the headset: `unity\run-editorplay.ps1` (two Play rounds with a Stop between, questions on the headset board; `-Mock` for a dry run).

## First live scene
`unity/BridgeTest` scene `Assets/FirstLight/FirstLight.unity` (`FirstLightScene`): sky colour, 1 m floor grid, cubes 1–3 m away and instructions, on the native backend. The first head pose recentres the rig (instructions straight ahead; floor 1.6 m below the head if the Link origin is at eye level). The Console logs the backend, the first head pose and every 720 frames the state and frame count.

Hands-off player: `tools\unity_hidden.ps1 -ArgLine "-batchmode -nographics -quit -projectPath unity/BridgeTest -executeMethod ElasticSea.QuestBridge.BridgeTest.Editor.BuildFirstLight.Build"` builds `unity/BridgeTest/Build/FirstLight/FirstLight.exe` (git-ignored, questbridge.dll included). Run it with `unity\run-firstlight.ps1` (sets `QB_LOG_FILE`, launches via `tools\start_player.ps1`); it quits after N seconds and so frees USB. Batchmode doesn't render, so that test drives a frame by hand there. EditMode covers projection math against FOV tangents, the RT layout and per-eye viewports, IPD offsets, late-latch/submit pose consistency, coordinate conversion, device registration, XR binding paths, controller state and haptics.
