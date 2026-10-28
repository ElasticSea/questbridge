# Meta Quest Link for Mac

Plug a Quest into a Mac and play PC VR — built from scratch, speaking Meta's own Link (XRSP) protocol so the headset's built-in "Enable Link" works.

- [docs/roadmap.md](docs/roadmap.md) — components and milestones
- [docs/protocol/spec.md](docs/protocol/spec.md) — everything learned about the protocol so far
- `capture/` — USB capture scripts (Windows, USBPcap); raw `.pcap` files are git-ignored
- `tools/` — pcap/XRSP parsers, Cap'n Proto schema extractor and codec
- `schemas/` — Cap'n Proto schemas recovered from Meta's runtime DLLs

Tools run with `py` (Python 3.14) on the Windows capture PC.
