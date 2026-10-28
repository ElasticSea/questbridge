# Building

The Unity package ships prebuilt native libraries, so you only need this to change native code or to run the C++ tests.

## macOS (Apple silicon)

Prerequisites: Xcode or the Command Line Tools, and [Homebrew](https://brew.sh):

```bash
brew install cmake libusb pkgconf
```

Build and test from the repository root:

```bash
cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release
cmake --build native/build -j
ctest --test-dir native/build --output-on-failure
```

Output lands in `native/build/bin/`: `libquestbridge.dylib` (arm64), `qbhost`, `qbenc` and the test executables. Homebrew's static `libusb-1.0.a` is linked in, so the dylib doesn't need Homebrew at run time. Without libusb, CMake warns and builds a library that can't open USB (`qb_host_open` returns `QB_ERR_UNSUPPORTED`).

Homebrew's libusb targets the macOS version it was built on, so the dylib's real minimum is that version (macOS 26 for the release builds). Build libusb from source with `MACOSX_DEPLOYMENT_TARGET=13.0` to support older systems.

No driver, root access or kernel-driver detach is needed. libusb claims only the headset's Link interface, so adb keeps working.

## Windows

Prerequisites: Visual Studio 2022 or newer with the *Desktop development with C++* workload (it includes CMake).

```powershell
native\build.ps1            # configure, build Release, run the tests
```

Or with any CMake 3.20+:

```powershell
cmake -S native -B native/build -A x64
cmake --build native/build --config Release
ctest --test-dir native/build -C Release --output-on-failure
```

Output lands in `native\build\bin\Release\`: `questbridge.dll`, `qbhost.exe`, `qbenc.exe` and the tests. USB goes through WinUSB, which Meta's Link app installs as the driver for the headset's Link interface.

**OpenH264 fallback (optional):** for PCs without a usable hardware encoder, `native\encoders\openh264\fetch.ps1` downloads Cisco's OpenH264 DLL into `%LOCALAPPDATA%\QuestBridge\openh264\`. Cisco covers the H.264 patent royalties only for its own binary downloaded by the end user, so it's never shipped with QuestBridge.

## Updating the Unity package's plugins

The package keeps its native libraries in `unity/com.elasticsea.questbridge/Runtime/Plugins/`, because installs from a git URL can't build anything. After a native change the package needs:

- **Windows:** close the Unity Editor (Windows locks a loaded DLL), then run `unity\install-native.ps1`. It builds `questbridge.dll` and copies it into the package with its import settings.
- **macOS:** run `unity/install-native.sh`. It builds `libquestbridge.dylib` with libusb linked statically and installs it the same way.

Commit the updated binaries together with the source change.

## The qbhost CLI

`qbhost` drives one headset connection through the C API, without Unity:

```bash
qbhost --check                      # setup check, exit code 1 if something is red
QB_LOG=1 qbhost 30                  # connect, stream a test clip for 30 s
```

Select Link in the headset after starting it; the Quest only says hello after that. `XRSP_REPLAY=<clip.json>` picks the clip to stream; `qbenc --name <name>` encodes one with this machine's encoder into `out/clips/`. On Windows, stop Meta's Link service first (`tools\windows\ovr_service.ps1 stop`) or let the library ask for it.
