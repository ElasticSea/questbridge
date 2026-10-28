# Troubleshooting

Start with **QuestBridge → Check Setup** in Unity (or `qbhost --check` from a native build). It checks the OS, the USB driver, whether the Quest is on USB and its Link interface is free, Meta's Link service, the GPU encoder and the Editor's graphics API, and says how to fix each problem.

## Common problems

| You see | Do this |
|---|---|
| Check Setup: "Quest on USB: no Quest found" | Check the cable (a data cable, no hub) and wake the headset. |
| Check Setup: "Link interface … no driver" (Windows) | Install the Meta Quest Link app ([Getting started](getting-started.md), step 1), then unplug and replug the headset. |
| Check Setup: "Link interface free: another program has it open" | Close other Unity Editors in Play mode, QuestBridge players and the Meta Quest Link app. |
| You pressed **No** on the Windows admin prompt | Press **Stop OVRService now** in Check Setup, or restart Unity and press Play again. To never see the prompt, stop "Oculus VR Runtime Service" in `services.msc` yourself, or run `tools\windows\ovr_service.ps1 stop`. |
| Headset: "Attempting to connect" for more than ~10 s, or no **Launch** button | Stop and Play again. |
| Headset: no picture after Launch | Quit Link in the headset and press Launch again. |
| Console: "libquestbridge not found, using the mock backend" | Download the native library from the release page into `Assets/Plugins/` ([Getting started](getting-started.md), step 2). |
| Meta's own Link doesn't work after using QuestBridge | Quit Unity. Meta's service starts again by itself. Restarting the PC also restores it. |

The library reconnects on its own after cable glitches, the headset sleeping, or Link being quit in the headset, so you normally don't need to replug. [Connecting](protocol/connecting.md) has the details.

## Logs

- **Native log:** set `QB_LOG=1` and `QB_LOG_FILE=<folder>/questbridge.log` before starting Unity Hub (the Unity Editor doesn't show plugin output). Players and `qbhost` also log to stderr.
- **Headset log** (with developer mode and adb): `adb logcat -s xrspd xrstreaming`.
- **Unity:** the Console. `QuestBridgeConnectionStatus` logs every disconnect and reconnect with a reason.

When you report a bug, include the Check Setup report, the native log and the Unity Console. The [bug report template](https://github.com/ElasticSea/questbridge/issues/new/choose) asks for them.
