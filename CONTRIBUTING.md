# Contributing to QuestBridge

Thanks for helping. Bug reports, test results on hardware we don't have, protocol findings and pull requests are all welcome.

## Ground rules

- **Your own work only.** Don't contribute code, comments or data copied from other Link or streaming implementations, or from Meta's software (decompiled code, binaries, extracted assets). Facts about the protocol learned from your own devices' traffic are fine. Say how you learned them.
- **No personal data.** Never commit raw USB captures, logs with serial numbers, or anything with your headset's identity in it. Fixtures go through `tools/scrub.py` (see [tests/fixtures/README.md](tests/fixtures/README.md)), and `tests/test_fixture_privacy.py` checks them.
- Be kind. See the [Code of Conduct](CODE_OF_CONDUCT.md).

## Reporting a bug

Use the bug report template. The most useful things to include are:

1. The **Check Setup report**: QuestBridge → Check Setup → Copy report, or `qbhost --check` from a build.
2. The **native log**: set `QB_LOG=1` and `QB_LOG_FILE=<folder>/questbridge.log` before starting Unity Hub (or the player).
3. Your headset model and firmware, OS version, GPU and Unity version.

Security problems go through [SECURITY.md](SECURITY.md), not public issues.

## Development setup

- [docs/building.md](docs/building.md): native library, Unity package plugins, prerequisites per platform.
- [docs/testing.md](docs/testing.md): the test suites and which ones need a headset.
- [docs/protocol/spec.md](docs/protocol/spec.md): the protocol, if you're touching the wire format.

Run the tests that cover your change before opening a pull request:

```bash
python3 -m unittest discover -s tests
ctest --test-dir native/build -C Release --output-on-failure
```

On Windows, `tools\test_all.ps1 -Quick` runs Python and native in one go, and `tools\test_all.ps1` adds the Unity suites.

## Pull requests

- Keep each change focused: one fix, feature or finding per pull request.
- Match the surrounding code: naming, comment density and formatting. C++20 for native code, C# for the Unity package, Python 3 standard library only for tools and tests.
- Add or update tests. Protocol changes need a byte-exact test against a fixture where one exists.
- Update the docs your change touches (package README, `docs/`, the protocol spec).
- New source files start with the SPDX header used in the rest of the repo.
- Commit messages: one short line in the imperative mood, such as `Fix stick dead zone on the left controller`.

By contributing you agree that your contribution is licensed under the [Apache License 2.0](LICENSE).
