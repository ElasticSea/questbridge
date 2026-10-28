# Test fixtures

Language-neutral JSON cut from USB captures of a Quest talking to Meta's own PC software, used by the Python tests (`tests/`) and the C++ tests (`native/tests/`). They let both check our framing, Cap'n Proto encoding and message building byte for byte, without a headset.

| Scenario | Contents |
|---|---|
| `handshake/` | A full Link session start on a Quest 3: control messages (`control.json`), stream messages (`streams.json`), raw USB transfers (`transfers.json`) and the message order (`transcript.json`). |
| `sensors/` | The same files for a session with controllers, haptics and hand tracking, plus `controllers.json` and `haptics.json`. |
| `hands/` | Hand-tracking stream messages (topics 8 and 9). |
| `devices/` | The device description from a Quest 3 and a Quest Pro, with the values our parser should get. |
| `replay/` | One recorded video frame (the message framing of five slices) and the 48 setup messages the PC sends when streaming starts. The video payload is blanked: only headers and parameter sets remain. |

## Privacy

Identifiers are replaced with equal-length placeholders, so lengths and offsets stay valid:

| Identifier | Placeholder |
|---|---|
| Headset serial | `QUEST0SERIAL00` |
| Device and session UUIDs | `00000000-0000-4000-8000-00000000000N` |
| Hardware device id | `1` |
| The PC's host name | `HOSTNAMEHOSTNA…` (same length) |

`tools/scrub.py` learns the identifiers from each capture's HELLO and replaces them in every message, as text and as raw bytes. `tests/test_fixture_privacy.py` fails if a known identifier shows up in any fixture.

## Adding a scenario

1. Record a capture (Windows, [tools/README.md](../../tools/README.md#capturing-usb-traffic-windows)).
2. `python3 tools/extract_fixtures.py out/captures/<file>.pcap <device> <scenario> [until-s]`
3. Add the scenario to `SCENARIOS` in `tests/test_roundtrip.py` and add its golden facts.
4. If templates changed, regenerate them: `python3 tools/gen_native_templates.py`.
5. Never commit the raw capture.
