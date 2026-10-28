# QuestBridge documentation

**Using it**
- [Getting started](getting-started.md): install the package and stream your first scene.
- [Troubleshooting](troubleshooting.md): Check Setup results, connection problems, logs.
- [Unity package reference](../unity/com.elasticsea.questbridge/README.md): rig, XR loader, input, hands, audio, connection events.
- [Scripting quick start](../unity/com.elasticsea.questbridge/QUICKSTART.md): head, controllers, hands and audio in your scripts.

**Working on it**
- [Architecture](architecture.md): how Unity, the native library and the headset fit together.
- [Building](building.md): native library and the package's prebuilt plugins on macOS and Windows.
- [Testing](testing.md): test suites, fixtures and the hardware checks.
- [Roadmap](roadmap.md)

**The protocol**
- [Protocol spec](protocol/spec.md): XRSP framing, handshake, video, pose, input, audio, haptics.
- [Connecting](protocol/connecting.md): how the host connects and recovers without replugging.
- [Schemas](protocol/schemas/): Cap'n Proto field names and ids of the control messages.
