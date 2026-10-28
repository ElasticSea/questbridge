# Changelog

## [0.2.0] - 2026-10-28
First public release, as QuestBridge.

- Renamed to QuestBridge: package `com.elasticsea.questbridge`, namespace `ElasticSea.QuestBridge`, native library `questbridge` / `libquestbridge`, C API prefix `qb_`, environment variables `QB_*`, menus under **QuestBridge**.
- macOS: HEVC by default, Metal zero-copy frame submit.
- Windows: encoder chosen per GPU vendor, OpenH264 software fallback.
- Recovery inside a running app after disconnects; `QuestBridgeConnectionStatus` events.
- Apache-2.0 license.

## [0.1.0] - 2026-10-28
First tester build.

- Streams a Unity scene to a Quest 3 over USB with Meta's Link protocol, from Editor Play mode or a Windows/macOS player. No app on the headset.
- Head, controllers (buttons, sticks, haptics) and hands (26 joints, XR Hands provider) as Input System XR devices. Game audio to the headset, headset mic to Unity.
- Native libraries included: `questbridge.dll` (Windows x64, Direct3D 11 + Media Foundation H.264) and `libquestbridge.dylib` (macOS, Apple silicon, Metal + VideoToolbox). Install by git URL, no build step.
- Windows: Meta's OVRService is stopped automatically when the Quest is found (one admin prompt per Unity session) and started again when Unity quits. `QB_OVR_STOP=0` turns this off.
- **QuestBridge > Check Setup**: OS, Meta Link USB driver, Quest on USB, Link interface, OVRService, GPU encoder and Editor graphics API, each with a fix. Also `qbhost --check` and `-executeMethod ElasticSea.QuestBridge.Editor.QuestBridgeCheckSetup.RunFromCommandLine`.

Known limits: Quest 3 only so far; macOS streaming not yet tried with a headset; 72 Hz; H.264.
