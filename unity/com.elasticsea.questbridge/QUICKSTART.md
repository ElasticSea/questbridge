# QuestBridge for Unity: scripting quick start

Play your own Unity scene on a Quest over USB, straight from the Editor's Play button. No app on the headset. Installing the package and first run: [Getting started](../../docs/getting-started.md); this page is the rig and scripting part.

## 1. Drop the rig in your scene
1. **GameObject → XR → QuestBridge Rig** (or drag `Packages/QuestBridge/Runtime/Prefabs/QuestBridge Rig`).
2. Put it where the player should stand, on the floor. Leave your Main Camera where it is: while playing the rig copies its settings (clear color, culling mask) and turns it off, and `Camera.main` points at the rig's left eye.
3. Leave **QuestBridge** unticked in Project Settings → XR Plug-in Management (that's the other way to stream, without the rig; both at once don't work).
4. Select the rig. If the Inspector shows a red box (e.g. "streaming needs Direct3D 11"), press its button; Unity restarts once.

## 2. Play
1. Plug the Quest in.
2. Press **Play** in Unity.
3. Windows asks once (UAC) to pause Meta's Link service; allow it. It starts again when Unity exits.
4. Put the headset on. In the headset: **Quick Settings → Link → Launch**. Your scene appears within a few seconds.

The Game view shows the left eye. You can switch away from Unity or minimize it; the headset keeps running at 72 Hz. Stop and Play again as often as you like; you stay in Link.

## Use the headset in your scripts
- **Head:** the rig moves its `Head` child; anything parented to `Head` follows it. Input System path `<XRHMD>/centerEyePosition`.
- **Controllers:** `Left Controller` / `Right Controller` children follow the controllers (stock `TrackedPoseDriver`). Buttons: `<XRController>{RightHand}/triggerPressed`, `/primaryButton`, `/primary2DAxis`, … (any XRI or Input System action on generic XR paths works).
- **Hands:** the rig's `QuestBridgeHands` component has 26 joints per hand and pinch strength; `HandJointsVisualizer` draws them as spheres. With Unity XR Hands installed, add `QuestBridgeXRHands` to the rig.
- **Audio:** everything your AudioListener hears plays in the headset (the listener is on the rig's `Head`).

## If something's off
| You see | Do this |
|---|---|
| Console: "No Quest on USB yet" | Check the cable; allow the UAC prompt (Meta's Link service holds the headset otherwise). |
| Headset: Link dialog without a **Launch** button, or "Attempting to connect" for more than ~10 s | Stop and Play again. |
| Headset: your scene doesn't appear after Launch | Quit Link in the headset, press Launch again. |
| Console: "libquestbridge not found, using the mock backend" | The package came without `questbridge.dll`; tell us which package version you installed. |

Report problems with the Unity Console log and, for connection trouble, a native log: set the environment variables `QB_LOG=1` and `QB_LOG_FILE=C:\temp\questbridge.log` before starting Unity Hub.
