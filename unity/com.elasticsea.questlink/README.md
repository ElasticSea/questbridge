# Quest Link for Mac (Unity package)

`com.elasticsea.questlink` streams a Unity scene to a Quest over USB through the native `libquestlink` (Meta's Link protocol, no headset APK) and brings head and controller poses back as Input System XR devices. Roadmap M6/M7; design in `docs/ROADMAP_REVIEW.md` ("Unity integration on macOS").

Unity 6000.3, Input System 1.19. Works in Editor Play mode. Without the native library the rig uses a mock backend.

## Native plugin (Windows)
`unity\install-native.ps1` builds `questlink.dll` (Release, `native\build.ps1`, C++ tests included) and copies it to `Runtime/Plugins/Windows/x86_64/` with Editor + Win64 import settings. The binary is committed (UPM git installs need it), so run the script and commit `questlink.dll` after a native change the package needs; `-NoBuild` only copies. macOS: `unity/install-native.sh` on the Mac builds `Runtime/Plugins/macOS/libquestlink.dylib` (arm64); copy it back and commit it the same way. Close the Unity Editor first: Windows locks a loaded DLL until the Editor exits.

The Windows backend needs Direct3D11 (the encoder copies the `ID3D11Texture2D`); with another graphics API the rig warns and falls back to the mock. Set `QL_LOG_FILE=<path>` (and `QL_LOG=1` for every protocol message) before starting the Editor to get the native log, because the Editor drops plugin stderr. OVRService holds the XRSP interface while it runs: when the Quest is plugged in, the library stops it through one elevated helper (one UAC prompt per Editor session) that starts it again when the Editor exits (`QL_OVR_STOP=0` before starting the Editor turns this off; then `host\ovr_service.ps1 stop` by hand). **QuestLink > Check Setup** shows what's missing on a machine.

Testers: [QUICKSTART.md](QUICKSTART.md) (git URL, prefab, Play).

## Editor Play mode
- **Drop-in prefab.** `Runtime/Prefabs/QuestLink Rig.prefab` (GameObject → XR → QuestLink Rig): rig + `QuestLinkHands` + `HandJointsVisualizer`, `Head` with the AudioListener and `QuestLinkAudio`, and `Left/Right Controller` on stock TrackedPoseDrivers with a block shown only while tracked (`QuestLinkTrackedVisual`). No cameras in the prefab: the eyes are created at Play, so nothing renders in Edit mode. Regenerate with `BridgeTest/Assets/Editor/BuildRigPrefab.cs`.
- **One native runtime per Editor process.** The Editor never unloads `questlink.dll`; its runtime (USB, handshake, encoder) starts at the first Play and keeps the headset session across Stop/Play and domain reloads. Each Play opens one `ql_session` and closes it on Stop (or on a script reload while playing), so nothing native leaks between Play sessions. `ql_shutdown` is only for handing the headset to another host.
- **While playing** (`headsetFramePacing`): `Application.runInBackground = true`, vSync off and `targetFrameRate` = the headset's refresh rate, so the Editor keeps streaming at 72 Hz when unfocused or minimized and doesn't render frames the encoder drops; all restored on Stop.
- **Main Camera** (`takeOverMainCamera`): eyes copy its settings when `cameraTemplate` is empty, it is disabled (with its AudioListener when the rig has its own) and the left eye is tagged `MainCamera`; restored on Stop.
- **Game view** (`mirror`): left eye (default), both eyes, or nothing; a culling-mask-0 camera clears the display under it.
- **No headset:** `ql_open` and the per-frame calls never block; USB is polled on the runtime's own thread. The rig logs its state changes and, after 15 s without streaming, a hint (OVRService / press Launch). The rig inspector shows the state and setup problems with fixes (Direct3D 11, missing `questlink.dll`, Meta's service running, Input System off).

## Use
1. Add the package: Package Manager → Add package from git URL → `https://github.com/ElasticSea/meta-quest-link-for-mac.git?path=/unity/com.elasticsea.questlink#v0.1.0` (testers: [TESTERS.md](../../TESTERS.md)), or from disk (`package.json`, or `"com.elasticsea.questlink": "file:<path>"` in `manifest.json`).
2. Put `QuestLinkRig` on an empty GameObject at the tracking origin (floor level). It creates `Head/LeftEye` and `Head/RightEye` cameras. Use `cameraTemplate` to copy clear flags, culling mask and similar settings onto both eyes.
3. Bind to the generic XR paths with stock components such as TrackedPoseDriver or XRI: `<XRHMD>/centerEyePosition`, `<XRController>{LeftHand}/devicePosition`, `{RightHand}/{TriggerButton}`, `/primary2DAxis`, …
4. Hands: add `QuestLinkHands` to the rig object; `HandJointsVisualizer` draws each joint as a sphere. With Unity XR Hands (`com.unity.xr.hands` 1.3+) in the project, also add `QuestLinkXRHands` to run an `XRHandSubsystem` fed by QuestLink, so XR Hands visualizers and XRI hand interaction work; parent their visuals to the rig (joint poses are relative to the tracking origin).
5. Sample: Package Manager → Quest Link for Mac → Samples → Basic Scene.

Don't add a TrackedPoseDriver to the rig's `Head`, because the rig drives it directly.

## XR Plug-in Management (unchanged XRI / XR Origin projects)
Instead of a `QuestLinkRig`, an existing XR project can stream as is: Project Settings → XR Plug-in Management → Windows, Mac & Linux → untick OpenXR (or move it below), tick **Quest Link**, press Play. Needs XR Plug-in Management 4+ (XRI projects have it); the loader assembly compiles only then.
- `QuestLinkLoader` starts a hidden `QuestLinkXRDriver` (DontDestroyOnLoad). It streams `Camera.main` (the XR Origin camera): two hidden eye cameras are added under it (copying its settings), render into the same side-by-side texture as the rig and submit with the camera's tracking-space pose, i.e. exactly the pose the frame was rendered from. The camera itself keeps rendering to the Game view as the mirror.
- Head, controllers and haptics are the same Input System devices as with the rig, so XR Origin's `TrackedPoseDriver` (`<XRHMD>/centerEye…`) and XRI's `<XRController>{LeftHand}` bindings work unchanged. The head pose is late-latched inside the Input System's before-render update, so `TrackedPoseDriver` renders with the newest sample.
- Hands: `QuestLinkHands` plus, with XR Hands installed, the `QuestLink-Hands` `XRHandSubsystem`. Audio/microphone components with no rig use the loader's backend.
- Settings (Project Settings → XR Plug-in Management → Quest Link): backend (Auto/Native/Mock), tracking origin (Device: Camera Offset = XR Origin's Camera Y Offset; Floor: 0) and hand tracking.
- Why managed and not a native XR SDK display/input provider: Unity's `XRDisplaySubsystem`/`XRInputSubsystem` can only be implemented natively against the XR SDK headers, which Unity hands out only after an access sign-up (they are not in the Editor's PluginAPI). Everything an XRI project actually consumes (Input System devices, XR Hands, the camera transform) works without them. What is missing: `XRSettings.enabled` stays false, no single-pass instanced rendering (two passes, like the rig), and the mono mirror camera still renders. A native provider can replace the driver later without changing the loader's surface.
- Don't combine it with a `QuestLinkRig` in the same scene (both own the Input System devices).

## How it works
- **Stereo texture.** One encode-sized RT, 4096×2240 on Quest 3, from the HELLO device description, in BGRA32 sRGB. The left eye renders into the left half and the right eye into the right half. Each eye camera gets an off-axis projection built from the per-eye FOV tangents (`QuestLinkMath.Projection`) and sits ±IPD/2 from the head.
  - Left half = left eye, confirmed on the bench (PROTOCOL_NOTES.md, M2b). The native layer sends each frame with rectifyMeshId 1001 and the device's per-eye FOV, which the headset needs to reproject (timewarp) to the frame's renderPose.
  - Unity stores render textures bottom row first where UV v=0 is the top of memory (D3D11, Metal), so the native backend blits a vertically flipped copy and submits that; the encoder reads rows top-down.
- **Late latch.** In `Application.onBeforeRender` the rig reads the newest head pose, moves `Head`, and stores that exact pose with an increasing `FrameIndex`.
- **Submit.** Once both eye cameras have rendered (`Camera.onPostRender` in the built-in pipeline, end of the SRP context otherwise), the rig calls `SubmitFrame(rt, renderPose, frameIndex)` once. The native backend passes the (flipped) texture's `GetNativeTexturePtr()` (`ID3D11Texture2D*` on D3D11, `id<MTLTexture>` on Metal) to the render thread through `CommandBuffer.IssuePluginEventAndData`.
- **Input.** `QuestLinkHMD : XRHMD` and two `QuestLinkController : XRControllerWithRumble`, with `LeftHand`/`RightHand` usages. Control names follow Unity's Oculus Touch layout, and usages follow XR feature usages (`TriggerButton`, `GripButton`, `Primary2DAxis`, `PrimaryButton`, `MenuButton`, …). `SendImpulse` and any `SendHapticImpulseCommand` go to the backend's `SendHaptic`.
- **Hands.** libquestlink turns hand tracking on once streaming (InputControl on topic 28; `QL_HANDS=0` before starting the Editor keeps it off) and parses topic 8. `QuestLinkHands` reads both hands in `Update` and again before rendering: 26 OpenXR joints (pose + radius; Unity XR Hands axes, +Z toward the fingertip, +Y back of the hand), confidence, pinch strength per finger, aim ray, dominant hand. The mock backend animates two hands curling their fingers through a C# port of the native forward kinematics (`QuestLinkHandSkeleton`).
- **Defaults.** Quest 3 values from PROTOCOL_NOTES.md: FOV in up/down/left/right degrees is 44/55/54/40 for the left eye and mirrored for the right, IPD is 63.5 mm, and the stream runs at 72 Hz. The native layer overrides them when it reports device info.

## Audio
- **Game audio → headset.** Add `QuestLinkAudio` next to the scene's AudioListener (ideally on the rig's `Head`). It reads the final mix in `OnAudioFilterRead`, downmixes to stereo and calls `ql_submit_audio`; the native side resamples to 48 kHz and sends a 10 ms block every 10 ms (silence when the queue runs dry). `muteLocal` (default on) silences the PC speakers.
- **Headset mic → Unity.** `QuestLinkMicrophone` pulls the Quest mic (48 kHz mono) every frame into a 1 s buffer: `Read(samples, offset, count)`, `Level` (RMS), `Clear()`. `playThrough` plays it live through an AudioSource on the same GameObject (40 ms jitter cushion); since that also reaches the headset through `QuestLinkAudio`, keep the gain low or expect feedback.
- FirstLight (`audioTest`, on by default) runs `AudioBench`: beeps every 2 s, records 4 s of mic, plays it back, with instructions and a mic level bar in the headset.

## Coordinates
The C API uses raw Link convention: right-handed, +Y up, −Z forward, metres, and Quest-clock nanoseconds. C# converts by mirroring Z: position `(x, y, −z)`, quaternion `(−x, −y, z, w)` (`QuestLinkMath.LinkToUnity`). It converts back the same way for the submitted renderPose.

## Assumed C API (libquestlink)
`native/include/questlink.h` didn't exist when this package was written, so the C# side targets the API below. All of it lives in `Runtime/Native/QuestLinkNative.cs`, the only file to change if the header ends up different. The library name is `questlink` (`libquestlink.dylib`). Return value `0` means success unless noted.

```c
typedef struct ql_session ql_session;
typedef struct { float qx, qy, qz, qw; float px, py, pz; uint64_t time_ns; } ql_pose;  // Link convention, Quest clock
typedef struct { float up, down, left, right; } ql_fov;                                 // positive tangents
typedef struct { uint32_t encode_width, encode_height; float refresh_hz; float ipd_m;
                 ql_fov fov_left, fov_right; } ql_device_info;
typedef struct { ql_pose pose; uint32_t buttons, touches; float trigger, grip, stick_x, stick_y;
                 uint32_t flags; } ql_controller;
// buttons/touches: 1 A/X, 2 B/Y, 4 thumbstick, 8 menu, 16 trigger (touches only)
// flags: 1 connected, 2 position tracked, 4 rotation tracked
typedef struct { ql_session* session; void* native_texture; ql_pose render_pose; uint64_t frame_index; } ql_frame_submit;

ql_session* ql_open(void);              // non-blocking; NULL on failure. USB/handshake run in the background
void ql_close(ql_session*);             // must tolerate submit events for it still queued on the render thread
int  ql_poll(ql_session*);              // 0 idle, 1 connecting, 2 streaming
int  ql_get_device_info(ql_session*, ql_device_info*);   // nonzero until HELLO arrived
int  ql_get_head_pose(ql_session*, ql_pose*);            // newest (ideally predicted) head pose
int  ql_get_controller(ql_session*, int hand /*0 L,1 R*/, ql_controller*);
int  ql_get_hands(ql_session*, ql_hands*);               // 26 OpenXR joints per hand, see questlink.h "Hand tracking"
int  ql_haptic(ql_session*, int hand, float amplitude, float duration_s);
int  ql_submit_frame_texture(ql_session*, void* native_texture, const ql_pose* render_pose, uint64_t frame_index);
// UnityRenderingEventAndData; event 1 = submit, data = ql_frame_submit* (valid until the event runs)
void* ql_get_render_event_func(void);
```

Notes for the native side:
- `frame_index` restarts at 1 on every Play-mode entry. The headset needs `frame.index` to increase across the whole session, which outlives Play mode, so the native side should map it to its own counter.
- The render-thread callback has to copy the texture before returning (IOSurface/`CVPixelBuffer` from the VideoToolbox pool), because Unity reuses the RT next frame.
- If any entry point is missing, the rig logs a warning and falls back to the mock backend in `Auto` mode.

## Tests
`unity/BridgeTest` references this package locally. From the repo root:

```
powershell -ExecutionPolicy Bypass -File tools\unity_hidden.ps1 -ArgLine "-batchmode -nographics -projectPath unity/BridgeTest -runTests -testPlatform EditMode -testFilter ElasticSea.QuestLink.Tests -testResults results.xml"
```

Unity (Editor or player) is only ever started through `tools\unity_hidden.ps1` (hidden desktop, so it can't take focus); `tools\test_all.ps1` does this for every suite. Use `-testPlatform PlayMode` for the smoke test. Unity runs get `QL_USB=0`, so a loaded `questlink.dll` never opens the headset; `EditorPlayTests` then plays, stops and plays a native rig twice (with and without domain reload) on Direct3D 11, and `test_all` repeats the PlayMode suite in a real, minimized Editor (`SubmitsAtHeadsetRateWhileMinimized`). Live Editor check with the headset: `unity\run-editorplay.ps1` (two Play rounds with a Stop between, questions on the headset board; `-Mock` for a dry run).

## First live scene
`unity/BridgeTest` scene `Assets/FirstLight/FirstLight.unity` (`FirstLightScene`): sky colour, 1 m floor grid, cubes 1–3 m away and instructions, on the native backend. The first head pose recentres the rig (instructions straight ahead; floor 1.6 m below the head if the Link origin is at eye level). The Console logs the backend, the first head pose and every 720 frames the state and frame count.

Hands-off player: `tools\unity_hidden.ps1 -ArgLine "-batchmode -nographics -quit -projectPath unity/BridgeTest -executeMethod ElasticSea.QuestLink.BridgeTest.Editor.BuildFirstLight.Build"` builds `unity/BridgeTest/Build/FirstLight/FirstLight.exe` (git-ignored, questlink.dll included). Run it as `tools\unity_hidden.ps1 -Exe unity\BridgeTest\Build\FirstLight\FirstLight.exe -ArgLine "-seconds 90 -logFile <player.log>"` with `QL_LOG_FILE` set in the environment; it quits after N seconds and so frees USB. Batchmode doesn't render, so that test drives a frame by hand there. EditMode covers projection math against FOV tangents, the RT layout and per-eye viewports, IPD offsets, late-latch/submit pose consistency, coordinate conversion, device registration, XR binding paths, controller state and haptics.
