# Changelog

## [0.1.0] - 2026-10-28
First tester build.

- Streams a Unity scene to a Quest 3 over USB with Meta's Link protocol, from Editor Play mode or a Windows/macOS player. No app on the headset.
- Head, controllers (buttons, sticks, haptics) and hands (26 joints, XR Hands provider) as Input System XR devices. Game audio to the headset, headset mic to Unity.
- Native libraries included: `questlink.dll` (Windows x64, Direct3D 11 + Media Foundation H.264) and `libquestlink.dylib` (macOS, Apple silicon, Metal + VideoToolbox). Install by git URL, no build step.
- Windows: Meta's OVRService is stopped automatically when the Quest is found (one admin prompt per Unity session) and started again when Unity quits. `QL_OVR_STOP=0` turns this off.
- **QuestLink > Check Setup**: OS, Meta Link USB driver, Quest on USB, Link interface, OVRService, GPU encoder and Editor graphics API, each with a fix. Also `qlhost --check` and `-executeMethod ElasticSea.QuestLink.Editor.QuestLinkCheckSetup.RunFromCommandLine`.

Known limits: Quest 3 only so far; macOS streaming not yet tried with a headset; 72 Hz; H.264.
