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

## 4. Session lifecycle

### 4.1 States

Names in `code` are from the headset's `xrspd`/`xrstreaming` logs; the host states are ours
(`ql_state` in `native/include/questlink.h`).

| Host state | What happens | Headset side |
|---|---|---|
| WAIT_HELLO | Host holds interface 0 open and reads | Link client running → sends HELLO v1 ~1.5 s after USB enumeration (`INVITE`) |
| PHASE1 | 3 request/response pairs (§4.2) | after `0x80`: `STATE_HOST_CONNECTED` |
| WAIT_LINK_REQUEST | Idle; answer nothing | User presses **Enable Link** / Launch → `0x85` (seconds to minutes later) |
| WAIT_HELLO_V3 | | sends HELLO v3 ~0.5 s after `0x85` |
| PHASE2 | Same 3 pairs with v3 bodies (§4.3) | `REASON_HANDSHAKE_SUCCESSFUL` |
| SETUP | Clock sync starts (§4.4), stream setup (§4.5) | `streaming session starting` → Connected; starts sending pose/audio/telemetry |
| STREAMING | Video, audio, controls | `first frame received` → `video stream started` |
| FAILED | | e.g. `buffer too small` → BYE, session over |

Phase 1 and phase 2 each complete in ~10 ms. Control `seq` restarts at 0 in both directions for
phase 2 (HELLO v3 and CONFIG v3 both carry seq 0). A host should accept a HELLO of either
version in WAIT_HELLO and skip phase 1 when the first one is v3 (our hosts do; whether a
relaunch ever starts at v3 is not recorded).

### 4.2 Phase 1 (cable plugged in)

Bracketed names are the headset's (`xrspd`); the others are ours (`host/handshake.py`,
`native/src/session.hpp`). "Session id" is the random u64 from HELLO v1 `d6`. Field notation:
`dN` = data word N, `pN` = pointer N of the root struct. All verified against the recording
(`tests/test_handshake.py`, `capture/handshake-decoded.txt`); the host-built messages are
Meta's recorded ones with only the session id replaced.

| # | Dir | Type | Name | Inner flags | Root (data/ptrs) | Content |
|---|---|---|---|---|---|---|
| 1 | Q→H | `0x81` | HELLO [INVITE] | `0x0c` | 7 / 13 | §8 |
| 2 | H→Q | `0x82` | CONFIG [OK] | `0x8c` | 15 / 4 | `d0` u16 1; `d10` = session id; `p0` empty data; `p1` u16 list `[1,3,2,4,8]`; `p2` struct `{u32 0xffffffff}`; `p3` null |
| 3 | Q→H | `0x83` | ACK [INVITE_ACK] | `0x8c` | 2 / 2 | `d0` 1; `d1` session id; `p0` null; `p1` u16 list `[1,2,0,4,8]` |
| 4 | H→Q | `0x89` | CONFIRM [CODE_GENERATION] | `0x0c` | 2 / 1 | `d0` 1; `d1` session id; `p0` null |
| 5 | Q→H | `0x8a` | CONFIRM_ACK [CODE_GENERATION_ACK] | `0x8c` | 2 / 0 | `d0` 1; `d1` session id |
| 6 | H→Q | `0x87` | START [PAIRING] | `0x8c` | 2 / 0 | `d0` = `0x00010001` (u16 1, u16 1); `d1` session id |
| 7 | Q→H | `0x80` | READY [PAIRING_ACK] | `0x8c` | 2 / 0 | `d0` 1; `d1` session id |
| 8 | Q→H | `0x85` | LINK_REQUEST | `0x8c` | 2 / 0 | `d0` 1; `d1` session id. Sent when the user enables Link |

Despite the names, no code or pairing secret is exchanged over USB; the messages carry only the
values above.

### 4.3 Phase 2 (Link session)

| # | Dir | Type | Inner flags | Content |
|---|---|---|---|---|
| 9 | Q→H | `0x81` HELLO v3 | `0x0c` | `d0` u16 3; no session id (`d6` = 0); session UUID in `p7`; refresh list; feature list (§8) |
| 10 | H→Q | `0x82` CONFIG v3 | `0x2c` | below |
| 11 | Q→H | `0x83` ACK v3 | `0x0c` | `d0` u16 3, u16 1; `d1` 0; `p0` selected mode `{u32 Hz = 72; p0 {f64 0.65, f64 0.60}; p1 {u64 123,875,920}}` (the 72 Hz entry of the mode table, §8; the u64 is probably bit/s); `p1` u16 list `[1,2]` |
| 12 | H→Q | `0x89` v3 | `0x0c` | `d0` 3; `d1` 0; `p0` null |
| 13 | Q→H | `0x8a` v3 | `0x8c` | `d0` 3; `d1` 0 |
| 14 | H→Q | `0x87` v3 | `0x8c` | `d0` 3; `d1` 0 |
| 15 | Q→H | `0x80` v3 | `0x8c` | `d0` 3; `d1` 0 |

CONFIG v3 (root 15 data / 4 ptrs):

| Field | Value (Meta) | Meaning |
|---|---|---|
| `d0` | u16 3, u16 1, u32 15 | version 3; other halves unknown |
| `d1` u32 | 5 | **slices per frame** (the headset logs `5 strips`; only 5 tested end to end) |
| `d2` u32 | 72 | **refresh rate, Hz** (one of HELLO's refresh list) |
| `d3` | 1 | unknown |
| `d4` | `0x00010001` | unknown |
| `d11` | u32 @+4 = 1 | unknown |
| `p0` | null | (empty data in v1) |
| `p1` u16 list | `[1,3,2,19,7,14,15]` | **selected session features** (§4.7) |
| `p2` text | `"USB3"` | transport |
| `p3` u16 list | `[0,2]` | unknown |

Our hosts send Meta's recorded v3 messages unchanged (optionally overriding `d1` and appending
features for experiments: `XRSP_SLICES`, `XRSP_FEATURES_ADD`).

### 4.4 Clock sync

Right after phase 2 the host sends the first ping; both sides then ping each other and answer
the other's pings (NTP style). Topic 1, inner flags `0x80`, inner size 2, body = 4 × u64
(layout from the recording; our host's use of it is verified on the bench):

| Message | Type | u64[0] | u64[1] | u64[2] | u64[3] |
|---|---|---|---|---|---|
| Ping | `0x06` | 0 | 0 | sender transmit time | sender's current offset estimate (other clock − own clock; 0 until known) |
| Pong | `0x16` | ping's transmit time (echo) | receive time of the ping | pong transmit time | sender's offset estimate |

- Times are in the **sender's own clock**: the PC uses QPC in ns (our host: its monotonic ns),
  the Quest its own monotonic ns (~days since boot). Offsets are two's complement (the Quest's
  estimate is negative because its clock is ahead).
- Inner counter: pings are numbered 1, 2, 3… per sender; a pong carries the counter of the ping
  it answers. (Our host numbers its pongs with its own counter; the headset accepts that.)
- Pattern in Meta's session: a burst of ~30 exchanges in the first 0.2 s, another burst ~2 s
  later, then the Quest pings once per second. The headset logs every update as
  `receive ping/pong ... new clock offset=... (rtt, dispersion, jitter)`.
- **What a host needs:** `quest_offset = ping.u64[2] − local receive time` from any Quest ping
  (error ≈ one-way USB latency, ~1 ms; good enough for video, §6.4). Answer every Quest ping
  with a pong.

### 4.5 Stream setup

After the handshake Meta's PC sends, in this order (times from the end of phase 2; observed):

| t | Topic | Dir | Content |
|---|---|---|---|
| +0.08 s | 1 | ↔ | clock-sync burst |
| +0.08 s | 26 AudioControl | ↔ | one message each way (§7.4) |
| +0.08 s | 31 RuntimeIPC | ↔ | RIPC transport handshake and initial state (§7.8) |
| +0.08 s | 2 Command | H→Q | four commands: type 12 = 2, 15 = 1, 17 = 0, 19 = 2 (§7.6) |
| +0.4 s | 34, 35 | Q→H | head pose stream starts |
| +2.07 s | 28 InputControl | H→Q | `{type 1, value 0}` = hands off (§7.3) |
| +2.1 s | 4 Mesh | H→Q | one message: `{u32 1001, u32 4096, u32 2240, u32 3648, u32 1968, …}` (§6.5) |
| +2.1 s | 10–14 | H→Q | video starts |
| +2.37 s | 7 Haptic | H→Q | one message (§7.7) |

The Quest's mic audio (6) and telemetry (33) begin right after the handshake, about 1.5 s before
the PC's audio.

**What our host sends** (verified sufficient): Meta's 48 recorded setup messages on topics 26,
31, 2 and 28 (`host/replay/meta-idr.json` `setup`, embedded in `native/src/templates_data.cpp`),
spaced as recorded with gaps capped at 0.5 s, then video. It does not send topics 4 or 7, yet
frames with `rectifyMeshId` 1001 are reprojected, so mesh 1001 is known to the headset without
topic 4 (inferred: built in, or the topic-4 values only repeat the headset's defaults).

### 4.6 Host exit, re-attach, replug

- **Host exit:** the headset session (and its decoders) survive the host process ending. A new
  host can open interface 0 and continue without a handshake (verified, `XRSP_ATTACH`), provided
  it continues every per-topic `seq` (control, each video topic, others it used) and the
  `EncodedSlice` frame index (§6.4), and waits for the next Quest ping (≤ 1 s) for the clock
  offset.
- **HELLO timing:** the Quest sends HELLO once per Link start. If no host is reading, it is
  lost. A host should poll for the interface and open it immediately (our hosts poll every
  20 ms, as Meta's hotplug watcher does).
- **Relaunch:** quitting Link in the headset and pressing Launch again produces a new HELLO;
  no replug is needed while the host keeps the interface open.
- **Replug after a dropped session:** `xrspd` expects USB back within ~2 s; Windows takes
  ~3.7 s to re-enumerate after a software restart of the port, so the first replug yields no
  HELLO and the second one works (observed). Headset-side `svc usb resetUsbGadget` is
  unreliable (USB stayed off until a physical replug).
- **Ending:** on a protocol error the headset sends BYE and drops the session (type code not
  decoded). No graceful host-side shutdown message is known; Meta's PC tears down every topic
  (`destroying topic N (Name)` in its log).

### 4.7 Session features

HELLO v3 `p6` lists the features the headset offers; CONFIG v3 `p1` the ones the host selects.
Wire id = Meta's internal `XrsSessionFeature` value − 1; both sides share the same serializer
(inferred from the binaries, high confidence; `FEATURES` in `tools/xrsp.py`).

| Wire | Meaning | Quest 3 offers | Meta selects |
|---|---|---|---|
| 1, 2, 3 | base protocol (always pushed by the PC) | 1 | 1, 2, 3 |
| 5 | unreliable tracking | ✓ | – |
| 6 | Opus audio (else raw PCM, §7.4) | ✓ | – |
| 7, 19 | protocol features, on whenever the headset offers them (names not recovered) | ✓ | ✓ |
| 8 | OAF interface | | |
| 9 | HMD pose frequency | ✓ | – |
| 10 / 11 / 12 | disable HMD mic / single decoder / ML super-resolution | | |
| 13 | protocol feature behind a PC-side check (probably a gatekeeper) | ✓ | – |
| 14 / 15 | RuntimeIPC raw messages / RIPC transport handshake (topic 31) | ✓ | ✓ |
| 18 | mobile PTW | | |
| 20 | PCA streaming: passthrough camera images to the PC on topic 32 (§7.9) | ✓ | – |

The phase-1 lists (`[0,4,1,2,3,8]` offered, `[1,3,2,4,8]` / `[1,2,0,4,8]` exchanged) probably
use a different id space; unknown. None of the features turns on hands or controllers; those
are runtime controls (§7.3).

## 5. Topics

Names are Meta's, from the PC runtime log (`destroying topic N (Name)`; `TOPICS` in
`tools/xrsp.py`). Meta's PC creates all of them every session. Rates are from an 8.5 s capture
of the Link home environment (counts include segment-table messages) unless noted.

| Id | Name | Dir | Rate | Format | Status |
|---|---|---|---|---|---|
| 0 | (filler) | both | per transfer | zero body (§2.3) | verified |
| 1 | Builtin | both | ~5/s after setup | inner header + Cap'n Proto / clock (§3.2, §4) | verified |
| 2 | Command | both | few | 32-byte record `{time, type, value}` (§7.6) | partial |
| 3 | Pose | – | not seen | – | unknown (superseded by 34) |
| 4 | Mesh | H→Q | once | Cap'n Proto, 3 data / 2 ptrs (§6.5) | partial |
| 5 | Video | – | not seen | – | unknown (video uses 10–25) |
| 6 | Audio | both | 200 msg/s each way | Cap'n Proto `{u64 ts, Data pcm}` (§7.4) | verified |
| 7 | Haptic | H→Q | once at setup | Cap'n Proto, 3 data / 1 ptr (§7.7) | partial |
| 8 | Hands | Q→H | 60/s when enabled | packed struct, 1320 B (§7.3) | verified (bench) |
| 9 | Skeleton | Q→H | 46 msgs once when enabled | packed struct chunks, 4120 B (§7.3) | verified (bench) |
| 10–25 | Slice0–Slice15 | H→Q | per frame | `EncodedSlice` + H.264 (§6); 10–14 used with 5 slices | verified |
| 26 | AudioControl | both | once each way | Cap'n Proto, 2 data words (§7.4) | partial |
| 27 | UserSettingsSync | – | not seen | – | unknown |
| 28 | InputControl | H→Q | on change | Cap'n Proto, 1 data word (§7.3) | verified (bench) |
| 29 | Asw | – | not seen | – | unknown (name suggests asynchronous spacewarp) |
| 30 | Body | Q→H | not seen | – | unknown (body tracking, enabled by InputControl type 2) |
| 31 | RuntimeIPC | both, mostly Q→H | ~600/s, ~590 B | RIPC state mirror (§7.8) | partial |
| 32 | CameraStream | Q→H | not seen | camera images (§7.9) | unknown |
| 33 | Logging | Q→H | ~7/s, ~190 B | Cap'n Proto, padded to 1 KiB; telemetry/stats | unknown |
| 34 | Pose2 | Q→H | ~543 poses/s | Cap'n Proto head pose (§7.1) | verified |
| 35 | DeviceInput | Q→H | ~543/s | Cap'n Proto, 2 data / 1 ptr (§7.2) | partial |

Both binaries also hold a newer static table (`… 33 Logging, 35 UnreliablePose, 36 Pose2,
37 DeviceInput, 38 FrameFeedback`) that is not used with this firmware/runtime pair; the log and
captures agree on the ids above. Expect renumbering with future firmware.

## 6. Video (topics 10–25)

### 6.1 Picture and codec

| Property | Value | Status |
|---|---|---|
| Codec | H.264, **Constrained Baseline** (profile 66, constraint flags `0xc0`), level 6.0, Annex-B | verified |
| Encoded size | 4096 × 2240 (HELLO device description, §8) | verified |
| Eye layout | side by side: **left half = left eye**, right half = right eye, 2048 × 2240 each | verified |
| Rate | the CONFIG refresh rate (72 Hz tested) | verified |
| Bitrate | Meta's selected mode: ~124 Mbit/s at 72 Hz | observed |
| Strips | CONFIG `slices` (5): strip *i* goes on topic `10 + i`, 448 px (28 MB rows) tall | verified |

At stream start the headset logs `Creating VideoReceiver: 1 temporal layers, 5 strips,
robustnessMode 0, with alpha, decoder per strip`: **each strip topic feeds its own decoder.**
So two equivalent encodings work (both verified at 72 fps, 0 glitches):

1. **Meta's:** one 4096 × 2240 picture in 5 slices (`first_mb_in_slice` 0 / 7168 / 14336 /
   21504 / 28672), the same SPS/PPS copied to every strip.
2. **Independent strips:** five 4096 × 448 streams, each with its own SPS/PPS and
   `first_mb_in_slice` 0. This fits encoders without slice-count control (one VideoToolbox
   session per strip).

Meta's stream (NVENC-like, from `tools/h264_fields.py`): `log2_max_frame_num` 8, POC type 2,
`max_num_ref_frames` 3, VUI timing 1000/144000 fixed, full-range BT.709, `nal_ref_idc` 3 on IDR
and P, slice_type 2/0, **`disable_deblocking_filter_idc` 2**. Low-latency SPS (no reordering,
`max_dec_frame_buffering` 1) is what our encoders use.

### 6.2 Messages per strip

For every frame, each strip topic carries, in seq order (verified: `tools/decode_video.py`,
`native/src/frame.cpp`, `native/tests/test_frame.cpp`):

| # | Flags | Body |
|---|---|---|
| 1 | `0x10` | Cap'n Proto segment table, 8 B: `00000000 19000000` (one segment of 25 words) |
| 2 | `0x10` | `EncodedSlice` segment, 200 B (§6.3) |
| 3 | `0x10`/`0x18` | csd: Annex-B SPS + PPS, `csdLength` bytes, padded to 4 B. **Only when `csdLength` > 0** |
| 4 | `0x10`/`0x18` | slice data: Annex-B NAL unit(s), exactly `videoLength` bytes after unpadding, padded to 4 B |

- Messages 3 and 4 must each fit one XRSP message (≤ ~128 KiB, §2.5); they cannot be split.
  A high-quality IDR strip can exceed this; our hosts drop such frames before sending.
- A frame = one message group on each of the `slices` topics with the same `frame.index`. Our
  host sends all of them in one USB transfer.
- Meta sends csd with intra frames; sending it on every frame (repeat headers) also works.

### 6.3 `EncodedSlice` (schema `video.capnp`, recovered from Meta's binaries)

Root struct: 18 data words, 2 pointers, followed by two `FovPort` structs (2 data words each)
= 200 B segment. Offsets below are from the start of the **data section** (segment offset + 8).

| Off | Type | Schema field | Use |
|---|---|---|---|
| 0 | u64 | `frame.index` | Frame counter, must increase over the whole headset session |
| 8 | u32 | `frame.rectifyMeshId` | **1001** for game frames (enables reprojection), 0 for Meta's loading screen (§6.5) |
| 12 | 4 × f32 | `frame.renderPose.orientation` x, y, z, w | Head pose the frame was rendered with |
| 28 | 3 × f32 | `frame.renderPose.translation` x, y, z | metres |
| 40 | i64 | `frame.renderPose.timeInNanoseconds` | Quest clock |
| 48 | u8 | `slice.index` | 0 … slices−1 (= topic − 10) |
| 49 | bits | bit 0 `frame.isIntraFrame`, bit 1 `slice.isLastSliceOfAFrame` | intra: frame is IDR; last: only on the last strip |
| 50 | u16 | `frame.envBlendMode` | 0 invalid, 1 opaque (Meta's frames), 2 additive, 3 alphaBlend (§7.9) |
| 52 | u32 | `slice.yOffsetInPixels` | 448 × index |
| 56 | u32 | `slice.heightInMacroBlocks` | 28 |
| 60 | u32 | `slice.csdLength` | bytes of message 3, 0 = no csd message |
| 64 | u32 | `slice.videoLength` | bytes of message 4 |
| 68 | – | – | unused (0) |
| 72 | i64 | `frame.metrics.pcPresentBeginNanosecondsSinceEpoch` | Quest clock |
| 80 | u64 | `slice.metrics.pcPresentToTransmissionBeginDurationNanoseconds` | duration (Meta: 3–7 ms, growing per strip) |
| 88 | i64 | `frame.metrics.targetVsyncNanosecondsSinceEpoch` | **Quest clock**, see §6.4 |
| 96 | i64 | `frame.metrics.targetDeadlineNanosecondsSinceEpoch` | Quest clock |
| 104 | i64 | `frame.metrics.estimatedGpuEndNanosecondsSinceEpoch` | Quest clock |
| 112 | i64 | `frame.metrics.pcAppLatencyMarkerSensorNanosecondsSinceEpoch` | Quest clock |
| 120 | u64 | `frame.pcPresentIndex` | 0 in Meta's recorded frame |
| 128 | u64 | `frame.apertureId` | 0 |
| 136 | u64 | `frame.passthroughLayerId` | 0 |
| 144 | ptr | `frame.renderFovPort0` | left eye `FovPort` |
| 152 | ptr | `frame.renderFovPort1` | right eye `FovPort` |

`FovPort`: `f32 leftTan, rightTan, upTan, downTan` (positive tangents of the half-angles). Meta's
game frames on Quest 3: left eye (1.3764, 0.8391, 0.9657, 1.4281), right eye (0.8391, 1.3764,
0.9657, 1.4281), i.e. tan of 54° / 40° / 44° / 55° from the HELLO FOV (§8), mirrored per eye.

Despite the `SinceEpoch` names, all absolute times are in the **Quest clock** (§4.4). Our host
builds headers from Meta's recorded one and patches: index, pose, mesh id, FovPorts, slice
index/bits, lengths, and shifts all non-zero timing words together so that `targetVsync` hits
its target (`patch_slice_header` in `native/src/frame.cpp`).

### 6.4 Timing and ordering requirements

All verified on the bench by breaking them:

| Requirement | Failure when violated (headset log) |
|---|---|
| Timing words (40, 72, 88–112) in the Quest clock and near now | stale: `Frame arrived N ms after deadline, frame drop` |
| `targetVsync` not too far ahead either | `Frame arrived ~7.6 ms before deadline, frame drop to prevent submit offset` (seen at send time + 20 ms with a fast sender; Deadline 6.5 ms). Our runtime targets send time + 16 ms (`QL_VSYNC_LEAD_MS`); + 20 ms works for slower senders |
| `frame.index` increases across the whole headset session (also across host restarts) | `timed out waiting for new decode frame index`, nothing shown |
| Per-topic `seq` continues across host restarts | same as above |
| All `slices` strips of every frame sent (CONFIG `slices` = 5) | 1–4 strips per frame: nothing shown, **no error**; recovers once full frames arrive again |
| Every message ≤ ~128 KiB, unsplit | `buffer too small`, BYE, session over |
| No deblocking across strip edges (`disable_deblocking_filter_idc` 1 or 2) when using one picture with P frames | idc 0 (deblock across slices): decode collapses to ~1 fps and the decoders wedge until a new Link session; the host's writes time out |

Intra-only and IDR + P (GOP 72) both work with the deblocking rule above. Motion vectors can
still cross strip edges (with x264 too); no visible problem so far but unverified.

### 6.5 Timewarp, rectify mesh and FOV

- The headset reprojects each frame from its `renderPose` to the current head pose **only when
  `rectifyMeshId` = 1001 and both `FovPort`s are filled in** (verified: a fixed renderPose then
  stays world-locked). With mesh 0 (Meta's loading-screen frame, which also has zero FOV and an
  identity pose) the image is shown head-locked, unreprojected.
- Each eye must be rendered through its own asymmetric projection (the FovPort values). The
  optical centre lies at x = L/(L+R) × 2048 = 1272 px in the left half and 776 px in the right
  half, y = U/(U+D) × 2240 = 904 px. Identical images in both halves give double vision.
- `renderPose` uses the topic-34 head pose convention (right-handed, +Y up, −Z forward, metres;
  inferred from the native API's convention and the matching quaternions).
- Topic 4 Mesh (sent once by Meta, not needed by our host): a **3-segment** Cap'n Proto message.
  Segment table `02000000 06000000 06420000 01300000` (segments of 6, 16,902 and 12,289 words).
  Root (3 data / 2 ptrs): u32 1001, u32 4096, u32 2240, u32 3648, u32 1968; both pointers are
  far pointers into segments 1 and 2. Inferred: the rectify mesh with id 1001 (encode size
  4096 × 2240, 3648 × 1968 maybe the per-frame render target). The two large segments (~132 and
  ~96 KiB) were lost to capture truncation; how they are carried within the ~128 KiB message
  limit is unknown.
