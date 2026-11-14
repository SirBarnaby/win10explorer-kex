# Patch notes

## 0.1.1 (14 November 2026)

[ExplorerMvpSetup.exe](https://github.com/SirBarnaby/win10explorer-kex/releases/download/v0.1.1/ExplorerMvpSetup.exe)

**Fixed**

- Tabs could stop appearing in new folder windows until the next sign-in. When setup or Options was run from a terminal, a script or a remote tool, Windows ended the hidden starter together with that program. In that case setup now has the Windows desktop start it, so it keeps running.
- If the starter is ended anyway, for example from Task Manager, it comes back within about a second. A second, idle copy watches it; it uses no CPU while it waits. After three quick failures in a row it stops trying until you sign in again or apply settings in Options or setup.
- The starter and the snap receiver no longer quit when another program sends them a plain close message. They stop only when setup, Options or sign-out asks them to.
- The starter checks folder windows again when Explorer restarts, or when Explorer starts after it.

**Changed**

- Touchpad snapping is an optional extra and is off on a new install. Turn it on with **Touchpad snap gestures** in Options, or tick it in setup. An update keeps the choice you already made, and all your other settings.
- While snapping is off, its receiver does not run and has no startup entry.

**Known issue**

- To go back to 0.1.0 while touchpad snapping is on, uninstall first. The older setup cannot stop the newer snap receiver.

## 0.1.0 (12 November 2026)

First public release: [ExplorerMvpSetup.exe](https://github.com/SirBarnaby/win10explorer-kex/releases/download/v0.1.0/ExplorerMvpSetup.exe). Compared with the earlier internal build:

**Faster**

- The tab you click is highlighted at once: a median of 5.5 ms, down from 220 ms.
- The folder view is back in place in a median of 179 ms after a switch, down from 1.2 s. Explorer's own folder loading is unchanged.
- Quick presses are no longer lost; the last one wins. Close, new tab and reopen during a switch are queued.
- Moving a window no longer repaints the tab strip.
- The tab session is saved once, 1.5 s after the last change, instead of on every change.

**New**

- Up to 500 tabs per window, up from 64. Tabs shrink to 76 px, then the strip scrolls with the wheel, the touchpad or its arrows, and keeps the selected tab in view.
- Tabs slide when they open, close or move. Dragging a tab along the strip moves it live and the others slide aside.
- **Animate tabs** in Options → Tabs turns the motion off. Windows' "Show animations in Windows" does too.
- Tab text in the title bar follows the window's own title colour: high contrast, DWMBlurGlass colours, dark and accent-coloured title bars.
- Setup is one file, 2.2 MB, and checks its own contents before installing.

**Lighter**

- The hidden starter wakes only when a top-level window opens or closes, not for every menu or tooltip. Idle, it uses no measurable CPU and about 3.5 MB of memory.

**Known limits**

- Dragging a tab out to its own window can occasionally drop it back on its own strip. Drag it out again.
- After a graphics driver reset the strip may stay empty until the pointer moves over it.
- Setup keeps earlier version folders, about 2 MB each, until you uninstall.
- The setup file is not code-signed, so SmartScreen warns the first time.
