# Quest Link for Mac (Unity package)

`com.elasticsea.questlink` streams a Unity scene to a Quest over USB through the native `libquestlink` (Meta's Link protocol, no headset APK) and brings head and controller poses back as Input System XR devices. Roadmap M6/M7; design in `docs/ROADMAP_REVIEW.md` ("Unity integration on macOS").

Unity 6000.3, Input System 1.19. Works in Editor Play mode. Without the native library the rig uses a mock backend.

## Use
1. Add the package (Package Manager → Add package from disk → `package.json`, or `"com.elasticsea.questlink": "file:<path>"` in `manifest.json`).
2. Put `QuestLinkRig` on an empty GameObject at the tracking origin (floor level). It creates `Head/LeftEye` and `Head/RightEye` cameras. Use `cameraTemplate` to copy clear flags, culling mask and similar settings onto both eyes.
3. Bind to the generic XR paths with stock components such as TrackedPoseDriver or XRI: `<XRHMD>/centerEyePosition`, `<XRController>{LeftHand}/devicePosition`, `{RightHand}/{TriggerButton}`, `/primary2DAxis`, …
4. Sample: Package Manager → Quest Link for Mac → Samples → Basic Scene.

Don't add a TrackedPoseDriver to the rig's `Head`, because the rig drives it directly.

## How it works
- **Stereo texture.** One encode-sized RT, 4096×2240 on Quest 3, from the HELLO device description, in BGRA32 sRGB. The left eye renders into the left half and the right eye into the right half. Each eye camera gets an off-axis projection built from the per-eye FOV tangents (`QuestLinkMath.Projection`) and sits ±IPD/2 from the head.
  - The half-per-eye layout is an **assumption**: Meta's IDR frame we replay leaves the EncodedSlice FovPorts at zero, and no capture has shown which half is which eye yet. To flip it, change `QuestLinkMath.EyeViewport`.
- **Late latch.** In `Application.onBeforeRender` the rig reads the newest head pose, moves `Head`, and stores that exact pose with an increasing `FrameIndex`.
- **Submit.** Once both eye cameras have rendered (`Camera.onPostRender` in the built-in pipeline, end of the SRP context otherwise), the rig calls `SubmitFrame(rt, renderPose, frameIndex)` once. The native backend passes `GetNativeTexturePtr()` (an `id<MTLTexture>` on Metal) to the render thread through `CommandBuffer.IssuePluginEventAndData`.
- **Input.** `QuestLinkHMD : XRHMD` and two `QuestLinkController : XRControllerWithRumble`, with `LeftHand`/`RightHand` usages. Control names follow Unity's Oculus Touch layout, and usages follow XR feature usages (`TriggerButton`, `GripButton`, `Primary2DAxis`, `PrimaryButton`, `MenuButton`, …). `SendImpulse` and any `SendHapticImpulseCommand` go to the backend's `SendHaptic`.
- **Defaults.** Quest 3 values from PROTOCOL_NOTES.md: FOV in up/down/left/right degrees is 44/55/54/40 for the left eye and mirrored for the right, IPD is 63.5 mm, and the stream runs at 72 Hz. The native layer overrides them when it reports device info.

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
"C:\Program Files\Unity\Hub\Editor\6000.3.11f1\Editor\Unity.exe" -batchmode -projectPath unity/BridgeTest -runTests -testPlatform EditMode -testFilter ElasticSea.QuestLink.Tests -testResults results.xml
```

Use `-testPlatform PlayMode` for the smoke test. Batchmode doesn't render, so that test drives a frame by hand there. EditMode covers projection math against FOV tangents, the RT layout and per-eye viewports, IPD offsets, late-latch/submit pose consistency, coordinate conversion, device registration, XR binding paths, controller state and haptics.
