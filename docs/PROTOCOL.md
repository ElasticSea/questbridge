# XRSP Link protocol specification

Meta's wired Link protocol (XRSP over USB) as this project implements it: a host on the PC/Mac
talks to the Quest's built-in Link client with no headset install. Everything here was learned
from USB captures of Meta's own PC software, headset and PC logs, and bench runs of our host.

- Reference setup: Quest 3, headset firmware `releases-oculus-14.0-v207`, Meta Horizon Link PC
  runtime 1.208. Other headsets and firmware versions may differ.
- Raw, chronological lab log: [PROTOCOL_NOTES.md](../PROTOCOL_NOTES.md). This document is the
  cleaned-up version. When they disagree, this one is newer, unless the log has a later entry.
- All multi-byte values are little-endian. Offsets are in bytes unless noted. "Word" means a
  64-bit Cap'n Proto word unless the context says 32-bit XRSP words.
- Personal identifiers are replaced by placeholders: `<SERIAL>`, `<DEVICE_ID>`, `<UUID>`.

Confidence labels used throughout:

| Label | Meaning |
|---|---|
| **verified** | Byte-exact test against Meta's traffic, or a bench run of our host that depends on it |
| **observed** | Seen consistently in captures, not exercised by our host |
| **inferred** | Derived from names in binaries/logs or from a single sample; may be wrong |
| **unknown** | Not decoded |

## 1. Overview

### 1.1 Transport

In Link mode the Quest enumerates as a SuperSpeed composite USB device, VID `0x2833`
(PID `0x5013` on our Quest 3; Meta's runtime also accepts `0x5010–0x5013`, `0x5017–0x501a`).
All interfaces are vendor class `0xFF` and bind to WinUSB on Windows, so no kernel driver is
needed (libusb/IOKit on macOS can claim them directly).

| Intf | Name | Class / Sub / Proto | Use |
|---|---|---|---|
| 0 | XRSP | ff / 8a / 01 | **All Link traffic.** Bulk OUT `0x01` (host→Quest), bulk IN `0x81` (Quest→host), max packet 1024 |
| 1 | Commlib | ff / 8b / 01 | Idle over USB (one packet in the capture) |
| 2 | Highwind | ff / 8c / 01 | Idle over USB; Meta's runtime configures a cert exchange on it (`certExchangeTimeoutMs=1000`) that we do not need |
| 3 | ADB | ff / 42 / 01 | Standard ADB; stays usable while a host holds interface 0 |

Only interface 0 is required. XRSP over USB is **plaintext and unauthenticated**: any host that
opens interface 0 receives the headset's HELLO (verified). Air Link uses the same library over
TLS/TCP; it is out of scope here.

### 1.2 Roles

- **Headset**: the native daemon `xrspd` owns the USB function and forwards XRSP to a subscribed
  Link client (`com.meta.pclinkservice.server` / `com.oculus.xrstreamingclient`). Without a
  running client it logs `There's no subscribed highwind/crosswind client` and sends nothing.
  The client starts when the user selects Link in the headset; `adb shell` cannot start it.
- **Host** (Meta's OVRService, or ours): waits for HELLO, answers the handshake, runs clock
  sync, sends video/audio/haptics/control, and receives pose, hands, audio, telemetry.

The headset drives the session: it sends HELLO, decides when Link is enabled, and keeps the
session alive after the host process exits (a new host can attach, §4.6).

### 1.3 Status at a glance

| Area | Status |
|---|---|
| Framing, padding, transfer packing | verified (byte-exact) |
| Control encoding (Cap'n Proto, inner header) | verified (byte-exact) |
| Handshake, clock sync | verified (our host completes it) |
| Video (H.264 slices + `EncodedSlice`) | verified (our encoder plays at 72 fps, timewarp works) |
| Head pose (topic 34) | verified (decoded, used by our host) |
| Audio PCM (topic 6) | verified (byte-exact re-encode); playback by our host not yet tested |
| Hands (topics 8/9, InputControl 28) | verified on the bench (enable + decode) |
| Controllers, buttons, haptics | partial: candidates known, not decoded |
| Passthrough, camera stream (32), body (30), RuntimeIPC (31) | inferred / unknown |

Tools and tests that check each fact are listed per section. Python tests: `py -m unittest
discover -s tests`; C++ tests: `native\build.ps1`.

## 2. Framing

### 2.1 XRSP message header

Every message starts with an 8-byte header (verified: `tools/xrsp.py` `encode_message`,
`tests/test_roundtrip.py` `XrspFraming`, `native/tests/test_xrsp.cpp`).

| Off | Type | Field |
|---|---|---|
| 0 | u8 | flags: `0x10` normal; bit `0x08` = body padded (§2.2). Seen: `0x10`, `0x18` |
| 1 | u8 | topic (0 = filler, 1 = control, others = streams, §5) |
| 2 | u16 | length in 32-bit words, **excluding the first 4 bytes**: message size = `4 + len*4` |
| 4 | u16 | sequence number, per topic and per direction, wraps at 65536 |
| 6 | u16 | reserved, 0 |
| 8 | … | body (`len*4 − 4` bytes, so always a multiple of 4) |

Other bits of `flags` are unknown; always send `0x10` / `0x18`.

### 2.2 Body padding (flag `0x08`)

Bodies must be a multiple of 4 bytes. When the payload isn't, or when the topic pads to a larger
block, the sender sets flag `0x08` and appends padding: filler bytes `0xDE`, with the **last byte
= pad count** (the whole pad, including that last byte; observed 1–249). The receiver strips
`body[-1]` bytes. Flag `0x08` is not a fragmentation bit (verified). How a pad above 255 B
would be encoded is unknown; in practice the payload sizes keep it below that.

The pad target depends on the topic (verified on all fixture samples; `tools/decode_audio.py`
`encode_audio` is byte-exact):

| Topics | Pad target |
|---|---|
| 10–14 (video), 31 (RuntimeIPC) | body to a multiple of 4 B |
| 6 (audio), 33 (Logging) | whole message (8 B header + body) to a multiple of **1024 B** |
| 1 (control) | no flag; the Cap'n Proto message is zero-padded inside the body instead (§3.2) |

Messages that are already aligned (segment tables, `EncodedSlice` headers) carry `0x10`.

### 2.3 USB transfers

Messages are packed back to back into bulk transfers. Each transfer is padded to a multiple of
1024 B with one **topic-0 filler message**: flags `0x10`, zero body, `seq` = the seq of the last
real message in the transfer. If fewer than 8 bytes remain to the boundary, the filler grows by
another 1024 B (verified: `pack_transfer` in `tools/xrsp.py`, `test_transfers_repack`).

Our host sends each frame (all slices) in one transfer; the headset accepts that (verified).

### 2.4 Reassembly

A message can span several transfers (HELLO is ~1.1 KB, video messages up to ~128 KB). The
receiver keeps a carry buffer per direction, parses headers from it, waits for `4 + len*4`
bytes, drops topic-0 messages, and keeps the remainder for the next transfer. A header whose
size is < 8 means filler/garbage: discard the rest of the transfer. Our hosts read IN in 1024 B
units (`native/src/transport.hpp`).

### 2.5 Maximum message size

The headset's receive buffer is ~128 KiB per XRSP message (verified on the bench):

- 128,480 B in one message works; 148,544 B → headset logs `Error while processing inbound
  data: buffer too small`, sends BYE, and the session ends.
- A payload may **not** be split over several XRSP messages to get around this (64 KiB chunks
  of one slice → same error).

Our host refuses to send anything above 120 KiB (`kMaxStreamMessage` in `native/src/frame.hpp`).

### 2.6 Sequence numbers

- Each (topic, direction) has its own `seq`, starting at 0 at session start and incremented for
  **every** XRSP message on that topic, including segment-table messages (§3.3).
- Control (topic 1) restarts at 0 for phase 2 of the handshake (§4).
- The headset tracks seq for the whole Link session. A host that re-attaches must continue the
  numbering; restarting video topics at 0 makes the headset wait forever
  (`timed out waiting for new decode frame index`) (verified).
