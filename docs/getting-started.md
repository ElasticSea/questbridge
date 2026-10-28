# Getting started

Press Play in Unity, and your scene runs on a Quest over a USB cable. There's nothing to install on the headset.

## You need

- **A Mac with Apple silicon** (M1 or newer) on macOS 26, **or a Windows 10/11 PC (x64)** with a GPU that has a hardware H.264 encoder (NVIDIA, AMD or Intel from the last ~8 years).
- **Unity 6000.3** and **git** on the PATH (Unity installs git packages with it).
- A **Quest 3** or **Quest Pro** and a USB-C **data** cable. Plug it straight into the computer, not through a hub. Use a USB 3 port if you can.

## 1. One-time setup

**Headset:** Settings → System → Quest Link → turn **Quest Link** on.

**Windows:** install the **Meta Quest Link** app (Meta Horizon Link) from [meta.com/help/quest/pcvr](https://www.meta.com/help/quest/pcvr/). QuestBridge only needs the USB driver it installs. You don't have to sign in or keep the app open.

**Mac:** nothing to install.

## 2. Add the package

1. Unity → **Window → Package Manager → + → Add package from git URL…**
2. Paste:
   ```
   https://github.com/ElasticSea/questbridge.git?path=/unity/com.elasticsea.questbridge#v0.2.0
   ```
3. If Unity asks to enable the new Input System and restart, say **Yes**.

The native libraries come with the package, so there's no build step.

## 3. Check Setup

Plug in the Quest, wake it up, then open **QuestBridge → Check Setup**. Each line is green, yellow (works, but read the note) or red (fix this first). Red lines say what to do, and some have a button, for example **Use Direct3D 11**, which restarts Unity once. Press **Check again** after fixing something.

## 4. Play

1. **GameObject → XR → QuestBridge Rig** adds the rig to your scene. Put it where the player should stand, on the floor.
2. Press **Play**.
3. **Windows only, once per Unity session:** Windows asks to let *Windows PowerShell* make changes. Press **Yes**. This pauses Meta's Link service, which otherwise holds the headset, and starts it again when you quit Unity.
4. Put the headset on and press **Launch** in the Link dialog. Your scene shows up within a few seconds.

You can Stop and Play again as often as you like, and you stay in Link. Unity can be in the background or minimized; the headset keeps running at 72 Hz.

**Existing XR Origin / XR Interaction Toolkit project?** Skip the rig: Project Settings → XR Plug-in Management → Windows, Mac & Linux → tick **QuestBridge** (and untick OpenXR), then press Play.

## Next steps

- Use the head, controllers, hands and audio in your scripts: [QUICKSTART.md](../unity/com.elasticsea.questbridge/QUICKSTART.md).
- Everything the package can do: [package README](../unity/com.elasticsea.questbridge/README.md).
- Something's off: [Troubleshooting](troubleshooting.md).
