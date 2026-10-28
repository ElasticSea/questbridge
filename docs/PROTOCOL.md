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

## 3. Encoding

### 3.1 Cap'n Proto

Most structured payloads are standard [Cap'n Proto](https://capnproto.org/encoding.html)
messages (verified: `tools/capnp_codec.py`, schema-less decode/encode).

- Always **one segment**; no far pointers, no capabilities seen. Segment table =
  `u32 segment_count − 1 (= 0)`, `u32 segment_words`.
- Only `video.capnp:EncodedSlice` (and a few unrelated schemas) are embedded in Meta's binaries
  (`tools/capnp_extract.py` → `schemas/`). All other messages are decoded structurally; field
  names in this spec are ours unless they come from that schema or from log strings.
- **Allocation order.** Meta's builder places pointer targets in the order fields are
  initialised, not in pointer-slot order (e.g. CONFIG v3: p1, then p3, then p2), and leaves
  zeroed holes where it re-allocated an object. Any valid Cap'n Proto layout is accepted by the
  headset as far as we know (semantic match for all 15 handshake messages), but to be
  byte-identical to Meta an encoder takes an explicit per-message order with gaps
  (`allocation_order` / `layout` in `tools/capnp_codec.py`). Verified: 15/15 handshake control
  messages re-encode byte-exact (`test_capnp_bodies_reencode`, `native/tests/test_capnp.cpp`).

### 3.2 Control body (topic 1)

Topic-1 bodies start with an 8-byte inner header (verified: `test_inner_header_reencode`,
`test_inner_header_size_field`):

| Off | Type | Field |
|---|---|---|
| 0 | u8 | message type (§4) |
| 1 | u8 | inner flags: `0x80` for clock sync; handshake messages use `0x0c`, `0x8c` or `0x2c` (meaning unknown; we copy Meta's value per message, see §4.2) |
| 2 | u16 | payload size in 16-byte units, rounded up. Cap'n Proto messages: `ceil((8 + words*8) / 16)` (segment table included). Clock messages: 2 |
| 4 | u32 | counter: 1 on every handshake message; per-sender sequence on clock messages (§4.4) |
| 8 | … | payload: a Cap'n Proto message **with its segment table inline**, or the 32-byte clock record. Zero bytes may follow up to the XRSP body end |

### 3.3 Cap'n Proto on stream topics

On every other topic that carries Cap'n Proto, one message is sent as **two XRSP messages** with
consecutive seq: first the 8-byte segment table (flags `0x10`), then the segment itself (flags
`0x10`, or `0x18` when padded per §2.2). Video then appends raw payload messages after the
segment (§6). Receivers should pair a table with the next message on the same topic and also
accept a table inline at the start of a body (`segments()` in `tools/decode_pose.py`).

### 3.4 Non-Cap'n Proto payloads

| Payload | Format |
|---|---|
| Clock sync (topic 1, types `0x06`/`0x16`) | 4 × u64 (§4.4) |
| Topic 2 Command | 32-byte packed record (§7.6) |
| Topics 8/9 Hands/Skeleton | packed little-endian C structs (§7.3) |
| Topic 31 RuntimeIPC | Meta's RIPC "Reflect V2" serialization inside a small Cap'n Proto envelope (§7.8) |
| Video csd / slice data (topics 10–14) | Annex-B H.264 (§6) |
