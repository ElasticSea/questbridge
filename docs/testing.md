# Testing

Most tests need no headset: they run against recorded sessions and a fake headset.

## Suites

| Suite | Command | Needs |
|---|---|---|
| Python protocol tests | `python3 -m unittest discover -s tests` | Python 3, nothing else |
| Native (C++) | `ctest --test-dir native/build -C Release --output-on-failure` | a native build ([Building](building.md)) |
| Unity EditMode + PlayMode | `tools\test_all.ps1 -Only unity` | Unity 6000.3, Windows |
| Encoder slice cap | `python3 tools/slice_cap_stress.py` | a native build with a hardware encoder |

On Windows, `tools\test_all.ps1` runs everything and prints a summary (`-Quick` for Python + native only). Logs and NUnit results go to `out\runs\test_all\<stamp>\`. Set `UNITY_EXE` if Unity isn't in the Hub's default location. Unity runs through `tools\unity_hidden.ps1`, which starts it on a separate desktop so test runs don't open windows over your work.

What they cover:

- **Python** (`tests/`): XRSP framing, transfer packing and padding, the Cap'n Proto codec (re-encoding by allocation order), inner headers, the device description, hands and controllers, all byte-exact against recorded traffic; the handshake transcript and its golden facts; that no fixture holds a personal identifier.
- **Native** (`native/tests/`): the same byte-exact checks in C++, the full handshake replayed through the C API against recorded USB transfers, video frame building, the session API and recovery paths against a fake headset, encoder selection with fakes; on macOS also Metal and VideoToolbox frame submit, decoded strip by strip.
- **Unity** (`unity/com.elasticsea.questbridge/Tests`, run in `unity/BridgeTest`): projection math, render texture layout, late-latched poses, coordinate conversion, Input System devices and bindings, hands, audio, the XR loader, connection events, and Play/Stop cycles with the native library loaded (USB disabled with `QB_USB=0`).

## Fixtures

`tests/fixtures/<scenario>/` holds language-neutral JSON cut from USB captures: control messages, stream messages and raw transfers, plus a transcript. Both the Python and C++ tests read them. [tests/fixtures/README.md](../tests/fixtures/README.md) explains each scenario and how to record a new one.

`native/src/templates_data.cpp` is generated from the fixtures by `tools/gen_native_templates.py`; a test fails if it drifts.

## Hardware checks

These need a headset and a person to put it on. Each one shows its steps inside the headset and waits for a controller button press (A = yes / next, B = no).

- **Editor Play mode:** `unity\run-editorplay.ps1` starts a minimized Editor, drops the rig into a fresh scene and plays two rounds with a Stop in between. `-Mock` does a dry run without USB.
- **Player bench:** `unity\run-firstlight.ps1` runs the FirstLight test scene (audio beep and mic loop, video, hands) as a small windowed player.
- **Recovery:** run the FirstLight player with `-sequence-recovery` and `python3 tools/recovery_check.py --log <player.log> --qblog <native.log>` beside it. The board walks through two disconnects (headset sleep, Link quit) and the script disturbs the headset over adb when the board says so. It passes when every drop recovers without restarting the app. Set `ADB` and `QUEST_ADB` (the adb serial) if adb isn't on the PATH or more than one device is attached.
- **Connect soak:** `python3 tools/connect_soak.py` cycles `qbhost` against the headset with killed hosts, unanswered hellos and headset sleep, and fails if any cycle needs more than a goodbye and a fresh hello. No one needs to wear the headset.

The simulated versions of the recovery cases run in `native/tests/test_recovery.cpp` on every build.
