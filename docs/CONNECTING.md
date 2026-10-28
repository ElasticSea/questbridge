# Connecting the Quest

One procedure, no replugs. Why it works: `docs/PROTOCOL.md` §4.6.

## Procedure

1. Plug the Quest in (any time, before or after the app starts). Leave it plugged in.
2. On Windows, Meta's OVRService must not hold the XRSP interface: `host\ovr_service.ps1 stop`
   (once per boot; `start` restores Meta Link). The library does this itself when it finds the
   Quest busy: one UAC prompt per process, service restarted when the process exits
   (`QL_OVR_STOP=0` turns it off).
3. Start the host (`qlhost.exe`, the Unity player, or the Editor in Play mode). Within ~2 s the
   headset's Link dialog shows **Launch** (phase 1 done; log: `phase 1 done`).
4. In the headset press **Launch**. Streaming starts (log: `sent 48 setup msgs`).

That's all. No replug, no reboot, no Link quit, in any of these cases:

| Situation | What the host does | User step | Verified |
|---|---|---|---|
| Quest plugged in before the host started, dialog stuck on "Attempting to connect to PC" | preluding BYE 50 ms after opening; headset sends a fresh HELLO ~1.6 s later | none, then Launch | yes (soak) |
| Host restarted / app relaunched / Unity Play stopped and started | same | Launch again if a stream was running | yes (soak, orphaned stream) |
| Earlier host died right after reading the headset's HELLO | BYE → fresh HELLO | none | yes (soak) |
| Headset slept and woke | BYE → fresh HELLO | none | yes (soak, 15 s sleep) |
| Link quit in the headset (dialog or stream) | the headset drops its USB function (xrspd logs `REASON_CABLE_UNPLUGGED`); host reopens, BYE → fresh HELLO ~1.4 s later | Launch | yes (bench, 2× quit → Launch → picture) |
| Headset dropped the stream (BYE, e.g. protocol error) | back to waiting for HELLO, BYE every 3 s until one comes | Launch | unit test only |
| A handshake step stalls | BYE, start over | none | unit test only |
| USB unplugged and replugged | reopen within 50 ms, then BYE | Launch | same path as Link quit; physical replug not tried |

Inside one process (Unity Editor, a player) the native runtime keeps the USB link and the
headset session across Play sessions, so only a real new stream needs Launch.

## If it still doesn't connect

Logs: `QL_LOG=1` (and `QL_LOG_FILE=<path>` for Unity) on the host; `adb logcat -s xrspd
xrstreaming` on the headset.

- Host log shows `BYE (no HELLO yet)` every 3 s and never `0x81`: the headset isn't reading
  XRSP. Seen only when the cable was unplugged while the headset's HELLO was unanswered: its
  `xrspd` stays in `HOST_CONNECTING` and starts no new pairing until the next disconnect
  (`PROTOCOL_NOTES.md`, "Replug bug"). Unplug and replug once, or `host\replug.ps1` (UAC).
  Keeping the host running while plugging in avoids it.
- `cannot open XRSP interface`: another program holds it (OVRService, a second host).
- Nothing on USB: check `adb devices` and the cable.

## Regression check

`py tools\connect_soak.py` (bench, OVRService stopped, no other host) cycles the host against
the headset with the disturbances above (killed hosts, unanswered HELLO, headset sleep) and
fails if any cycle needs more than BYE + HELLO. Launch (phase 2) needs a person: check it once
with steps 3–4 after a soak.
