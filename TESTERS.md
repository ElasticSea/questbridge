# Testers: 5-minute start

Press Play in Unity and your scene runs on a Quest 3 over a USB cable. There's nothing to install on the headset. This is build **0.1.0**; the changes are in [CHANGELOG.md](unity/com.elasticsea.questlink/CHANGELOG.md).

## You need
- **Windows 10/11 (x64)** with a GPU that has a hardware H.264 encoder (NVIDIA, AMD or Intel from the last ~8 years; no usable encoder? see [Software encoder](#optional-software-encoder-windows)), **or a Mac with Apple silicon** (M1 or newer) on macOS 26. The Mac path hasn't been tried with a headset yet, so tell us what happens.
- **Unity 6000.3** and **git** on the PATH (Unity installs packages with it).
- A **Quest 3** and a USB-C **data** cable. Plug it straight into the computer, not through a hub. Use a USB 3 port if you can.
- **Access to the GitHub repo** `ElasticSea/meta-quest-link-for-mac`, which is private. Check access once in a terminal: `git ls-remote https://github.com/ElasticSea/meta-quest-link-for-mac.git`. It should list refs without asking again. If you use SSH keys, use the `git@github.com:` URL below instead.

## 1. One-time machine setup
**Windows:** install the **Meta Quest Link** app (Meta Horizon Link) from [meta.com/help/quest/pcvr](https://www.meta.com/help/quest/pcvr/). QuestLink only needs the USB driver it installs. You don't have to sign in or keep the app open.

**Mac:** nothing to install.

**Headset:** go to Settings → System → Quest Link and turn **Quest Link** on.

## 2. Add the package
1. Unity → **Window → Package Manager → + → Add package from git URL…**
2. Paste one of these:
   ```
   https://github.com/ElasticSea/meta-quest-link-for-mac.git?path=/unity/com.elasticsea.questlink#v0.1.0
   git@github.com:ElasticSea/meta-quest-link-for-mac.git?path=/unity/com.elasticsea.questlink#v0.1.0
   ```
3. If Unity asks to enable the new Input System and restart, say **Yes**.

The native libraries come with the package, so there's no build step.

## 3. Check Setup
Plug in the Quest, wake it up, then go to **QuestLink → Check Setup**. Each line is green, yellow (works, but read the note) or red (fix this first). Red lines say what to do, and some have a button, for example **Use Direct3D 11**, which restarts Unity once. Press **Check again** after fixing something. **Copy report** puts the text on the clipboard for bug reports.

## 4. Play
1. Add the rig to your scene with **GameObject → XR → QuestLink Rig**. If that menu item isn't there yet, add the `QuestLinkRig` component to an empty GameObject on the floor.
2. Press **Play**.
3. **Windows only, once per Unity session:** a Windows prompt asks to let *Windows PowerShell* make changes. Press **Yes**. It stops Meta's Link service (OVRService), which otherwise holds the headset, and starts it again when you quit Unity. If you'd rather not see the prompt, stop "Oculus VR Runtime Service" in `services.msc` yourself before pressing Play.
4. Put the headset on and press **Launch** on the Link dialog. Your scene shows up within a few seconds.

You can Stop and Play again as often as you like and you stay in Link. Using the head, controllers, hands and audio in your scripts: [QUICKSTART.md](unity/com.elasticsea.questlink/QUICKSTART.md); everything else is in the [package README](unity/com.elasticsea.questlink/README.md).

## If something's off
| You see | Do this |
|---|---|
| Check Setup: "Quest on USB: no Quest found" | Check the cable (a data cable, no hub) and wake the headset. |
| Check Setup: "Link interface … no driver" (Windows) | Install Meta Quest Link (step 1), then unplug and replug. |
| Check Setup: "Link interface free: another program has it open" | Close other Unity Editors in Play mode, QuestLink players and the Meta Quest Link app. |
| You pressed **No** on the Windows prompt | Press **Stop OVRService now** in Check Setup, or restart Unity and press Play again. |
| Headset: "Attempting to connect" for more than ~10 s, or no **Launch** button | Stop and Play again. |
| Headset: no picture after Launch | Quit Link in the headset and press Launch again. |
| No picture, native log says `no usable H.264 encoder` | Your GPU's encoder can't make the stream the headset needs: install the [software encoder](#optional-software-encoder-windows). |
| Meta's own Link doesn't work after testing | Quit Unity. The service starts again by itself; rebooting also restores it. |

## Optional: software encoder (Windows)
QuestLink picks the GPU's hardware encoder by itself (NVIDIA, AMD, Intel) and checks that its output suits the headset. Only NVIDIA has been tried with a headset so far, so on AMD or Intel please send the native log line that starts with `encoder:`. If no hardware encoder works (no GPU encoder, a remote desktop, an old driver), QuestLink falls back to **OpenH264**, Cisco's free software encoder. It uses the CPU (a 6+ core CPU keeps up at 72 Hz on normal scenes) and isn't shipped with the package: Cisco covers the H.264 licence only when you download its binary yourself. To install it:
- From a repo checkout: run `native\encoders\openh264etch.ps1`.
- Without one: download [openh264-2.7.0-win64.dll.bz2](https://ciscobinary.openh264.org/openh264-2.7.0-win64.dll.bz2) (Cisco, ~450 KB), unpack it with 7-Zip and put `openh264-2.7.0-win64.dll` in `%LOCALAPPDATA%\QuestLink\openh264\`.

Restart Unity afterwards. To force a backend for testing, set `QL_ENCODER=openh264` (or `nvidia`, `amd`, `intel`) before starting Unity Hub.

**Reporting a bug:** send us the Check Setup report (**Copy report**), the Unity Console, and a native log. To get the native log, set the environment variables `QL_LOG=1` and `QL_LOG_FILE=<some folder>/questlink.log` before starting Unity Hub.

Command-line check without Unity (from a repo checkout with `native/` built): `qlhost --check`. In batch mode: `Unity -batchmode -nographics -quit -projectPath <project> -executeMethod ElasticSea.QuestLink.Editor.QuestLinkCheckSetup.RunFromCommandLine` (exit code 1 if something is red).
