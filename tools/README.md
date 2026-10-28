# Tools

Python 3, standard library only. Run them from the repository root (`python3 tools/<name>.py`, or `py tools\<name>.py` on Windows). They import each other, so run them as scripts, not with `python -I`.

## Protocol toolkit

| Tool | What it does |
|---|---|
| `usbpcap.py` | Reads USBPcap `.pcap` files (one record per USB request). |
| `xrsp.py` | Splits bulk transfers into XRSP messages and builds them (framing, padding). |
| `capnp_codec.py` | Schema-less Cap'n Proto decoder/encoder that reproduces the original byte layout. |
| `capnp_dump.py` | Pretty-prints a Cap'n Proto message's pointer tree. |
| `capnp_extract.py` | Recovers the Cap'n Proto schema nodes embedded in a compiled program ([schemas](../docs/protocol/schemas)). |
| `pcap_summary.py` | Per-device and per-endpoint traffic summary of a capture. |
| `dump_xrsp.py` | Hex dump of the first XRSP transfers for a device. |
| `handshake.py` | Prints every control message (topic 1) of a capture, decoded. |
| `device_info.py` | Decodes the device description in the headset's HELLO. |
| `decode_video.py`, `decode_pose.py`, `decode_hands.py`, `decode_controllers.py`, `decode_audio.py` | Decoders for each stream. |
| `h264_fields.py` | Dumps every SPS, PPS and slice-header field of an H.264 stream, to compare encoders. |

## Fixtures and code generation

| Tool | What it does |
|---|---|
| `extract_fixtures.py` | Cuts a capture into test fixtures (`tests/fixtures/<scenario>/`), scrubbing identifiers. |
| `extract_replay.py` | Extracts a recorded video stream as a replay clip for `qbhost`. |
| `scrub.py` | Replaces the headset's serial, ids and the PC's name with equal-length placeholders. |
| `gen_native_templates.py` | Generates `native/src/templates_data.cpp` from the fixtures. |
| `encode_pattern.py` | Encodes a moving test pattern with FFmpeg into a replay clip (`FFMPEG` overrides the ffmpeg path). |

## Test runners and hardware checks

| Tool | What it does |
|---|---|
| `test_all.ps1` | Runs every suite on Windows and prints a summary ([Testing](../docs/testing.md)). |
| `unity_hidden.ps1` | Starts Unity or a player on a separate desktop so scripted runs don't open windows over yours. |
| `slice_cap_stress.py` | Pushes high-entropy clips through `qbenc`; every slice must fit one XRSP message. |
| `connect_soak.py` | Reconnect soak against a real headset. |
| `recovery_check.py` | PC side of the in-app recovery check. |

`ADB` (path to adb) and `QUEST_ADB` (adb serial of the headset) configure the scripts that talk to the headset.

## Capturing USB traffic (Windows)

Needs [Wireshark](https://www.wireshark.org/) with USBPcap. `tools\capture\start-elevated.ps1 -Name <scenario>` starts a capture on the hub the Quest is on (one UAC prompt) and writes `out\captures\<scenario>-<stamp>.pcap`; `tools\capture\stop-capture.ps1` stops it. Captures contain your headset's identity: keep them local and commit only scrubbed fixtures.

`tools\windows\ovr_service.ps1 stop|start|status` pauses or restores Meta's Link service by hand.

## Python reference host

`tools/pyhost/` is the first host implementation, in Python: `handshake.py` runs a Link session against a headset (stop Meta's Link service first on Windows), with `usb_link.py` (pyusb + libusb, or WinUSB on Windows via `winusb_link.py`). `libquestbridge` replaced it, but it's handy for protocol experiments: `XRSP_DUMP=<topics>` writes raw messages for the decoders (`decode_hands.py` reads that format), and `XRSP_ATTACH` continues a session another host started. On macOS it needs `pip install pyusb libusb-package`.
