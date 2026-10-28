# Security policy

## Supported versions

Only the latest release gets security fixes.

## Reporting a vulnerability

Please **don't open a public issue**. Report it privately through GitHub's [private vulnerability reporting](https://github.com/ElasticSea/questbridge/security/advisories/new). Include what you found, how to reproduce it, and what an attacker could do with it. You'll get an answer within a week.

## Scope

Areas where a bug could matter:

- **USB input parsing** (`native/src/xrsp.cpp`, `capnp.cpp`, `session.cpp`, `pose.cpp`, `hands.cpp`, `controllers.cpp`, `audio.cpp`): everything the headset sends is untrusted input.
- **The elevated helper on Windows** (`native/src/ovr_service_win.cpp`): to pause Meta's Link service, the library starts one elevated PowerShell process after a UAC prompt.
- **Downloads**: `native/encoders/openh264/fetch.ps1` downloads Cisco's OpenH264 DLL and checks its hash.

Out of scope: problems in Meta's software or headset firmware. Report those to Meta.
