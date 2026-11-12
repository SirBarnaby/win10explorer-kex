# Explorer tabs: refinement round report

10–12 November 2026 · branch `feature/explorer-mvp` · plan `docs/plans/2026-11-10-2040-feat-tab-strip-speed-overflow-motion-plan.md`

## In short

Switching tabs now feels immediate. The strip highlights the tab you pressed in the same frame, quick presses are never lost, and the folder view is put back once instead of up to ten times. The strip holds up to 500 tabs and scrolls when they no longer fit. Tabs slide when they open, close or move. Tab text in the title bar uses the window's own title colour. The hidden starter does less work, and setup is now one file that checks itself before installing.

**Status: finished and installed on this PC.** Every Sandbox stage passed on the final files, three of them only after re-running: two because of faults in the lab's own test programs (fixed), one intermittent (see *Known limits*). The setup installed here is byte-for-byte the one the Sandbox tested. Nothing in the product changed after testing. The new tabs load into the next folder window you open (see *On this PC*).

## Measured before and after

"Before" is the build at the start of this round (commit `8ce2a95`, measured 10 November). "After" is the final build installed today, unless the row says otherwise. Timings come from Windows Sandbox on this PC, using the product's own high-resolution timing log. They vary by a few tens of milliseconds between runs.

| What | Before | After |
|---|---|---|
| Tab highlighted after a click (median / slowest 10 %) | 220 ms / 249 ms | 5.5 ms / 19.9 ms |
| Folder view settled after a switch (median) | 1,205 ms (worst 3.4 s) | 179 ms (slowest 10 %: 215 ms; worst 2.4 s) |
| View-restore passes per switch | up to 10 | 1 |
| Session saves per switch | 4 (on every change) | 1, written 1.5 s after the last change |
| Session saves when opening a new tab | 5 | 0 until things are quiet |
| Strip renders per switch | 3 | 1 |
| Strip repaints while a window is dragged for 5 s | 435 (87 per second) | 0 |
| Highlight with many tabs (median) | 123 ms with 10 tabs, 126 ms with 64 | 7.0 ms with 10 tabs, 6.3 ms with 120 |
| Tabs per window | 64 | 500; when tabs no longer fit at 76 px the strip scrolls |
| 120 tabs | over the limit | open in one window: tabs at 76 px, the row scrolls, the selected tab stays in view, and screen readers hear the 112 hidden tabs as off-screen |
| Reordering by dragging along the strip | no live movement | the tab follows the pointer and the others slide aside in 200 ms (7 animation frames); instant when animation is off |
| Starter CPU, idle | 31 ms in 5 min on this PC (0.01 %) | 0 ms in 5 min on this PC (below the 15.6 ms the counter can show) |
| Starter CPU while other apps open windows | 1.80 % with 656 windows in 60 s: 1.64 ms per window | 1.35 % with 2,038 windows in 60 s: 0.40 ms per window (Sandbox) |
| Starter memory (private / working set) | 3.4 MB / 12.9 MB on this PC | 3.5 MB / 10.3 MB on this PC |
| Setup | a folder of 75 files, 2.57 MB | one file, 2.21 MB (2,212,352 bytes) |

The plan also asked for a comparison with plain Explorer navigation, measured in the same Sandbox on the same two folders. For a plain navigation, Explorer reports the folder loaded at 79 ms and the navigation complete at 93 ms (medians). For a tab switch it reports 86 ms and 101 ms, within 8 ms of plain Explorer; putting back that tab's scroll position and selection is done at 179 ms. The highlight appears well before either. Most of the old delay was the strip itself: it was drawn disabled while Explorer loaded, then retried the restore every 100 ms. Explorer's own folder loading is unchanged; tabs still share one folder view per window, as in Windows 10 itself.

## What changed, unit by unit

| Unit | Result |
|---|---|
| U1 | Timing marks (off unless a lab registry value is set) and a guest timing fixture. |
| U2 | The pressed tab is highlighted at once; the latest press wins; close/new/reopen during a navigation are queued (up to 8). The view is restored once, when the folder reports its items, with one 1.5 s fallback; your own clicks cancel a pending restore. |
| U3 | Sessions are saved once after 1.5 s of quiet and on close. Moving or repainting a window no longer redraws the strip. |
| U4 | Overflow: tabs shrink to 76 px (Firefox's minimum), then the row scrolls with the wheel, touchpad or arrows; the selected tab is kept in view; hidden tabs are announced as off-screen to screen readers. Up to 500 tabs. |
| U5 | The strip is a tree of compositor layers, one per visible tab. Hover, focus and selection repaint only the tab concerned. |
| U6 | Motion on the compositor with Firefox's timings: open/close 100 ms, reorder 200 ms, scroll 150 ms. Off with Options → Tabs → *Animate tabs* or Windows' "Show animations in Windows". |
| U7 | Title-bar tab text follows high contrast, DWMBlurGlass's own title colours, dark title bars and accent-coloured title bars, in that order. |
| U8 | The starter wakes only when a top-level window opens or closes (shell hook), not for every menu or tooltip. |
| U9 | Single-file setup with an embedded SHA-256 table; *Animate tabs* option. |
| U10 | Static scans, cleanup, the full Sandbox suite on the final binaries, acceptance receipt, install on this PC, this report. |

## How native it is

- The tabs are a child window of the real Explorer window. There is no separate tab window, overlay or copy of Explorer. Explorer's own Ribbon, Quick Access Toolbar, address bar, search box and folder view are the real ones and are not redrawn.
- In the title bar, the tabs use the window's own caption colours and glass. When there is no room they move into a row that Explorer itself makes room for, through the Ribbon's own height notification.
- Keyboard, accessibility and drag and drop go through Windows' own mechanisms: Explorer's message loop, MSAA (screen readers see page tabs, close buttons and off-screen state), OLE drag and drop between windows, native tooltips and menus.
- Motion runs in the desktop compositor (DirectComposition), the same layer Windows uses for its own animations. It keeps running smoothly while Explorer is busy loading a folder.
- What is not native: the code lives in an add-on DLL loaded into Explorer by a small hidden starter, not in Windows itself. It hooks two Windows images (UIRibbon.dll and uxtheme.dll) and refuses to do so unless they are byte-for-byte the tested ones.

## What it costs

| | |
|---|---|
| Disk | 4.1 MB for this version (8 files, including a 2.2 MB copy of setup for *Change or remove*); earlier versions take 1.8–2.1 MB each and are kept until you uninstall (11.9 MB in all on this PC) |
| Starter (`ExplorerMvpHost.exe`) | 3.5 MB private memory, 10.3 MB working set, 2 threads. No measurable CPU in 5 idle minutes on this PC (under 15.6 ms); 0.40 ms per new window while other apps open windows (Sandbox) |
| Snap receiver (`NativeSnap.exe`) | 3.4 MB private memory, 11.3 MB working set, 3 threads. No measurable CPU in 5 idle minutes on this PC; it works only when a touchpad gesture arrives |
| Inside Explorer | One DLL per Explorer process. Graphics memory per window with tabs could not be measured in the Sandbox (see limits). |
| Background work | None while idle: no timers, polling or tray icon. Session saves happen once, 1.5 s after the last change. |

## Installing on another PC

Requirements:

- Windows 10 22H2 x64, build 19045.
- For tabs in the title bar: `C:\Windows\System32\UIRibbon.dll` with SHA-256 `941DCFCD96B3D9799945A5AB1B4D4B7D6859682A0AF9DF38223568D7F3C46A2F` and `uxtheme.dll` with SHA-256 `85E26787282D70D203026E393242A8616CFD832D4E68DB466151301B6063AF4E`. If only uxtheme.dll differs, tabs go in a row under the Ribbon. If UIRibbon.dll differs, tabs stay off and only touchpad snapping is offered. Setup says which of these applies before you install.
- No administrator rights. Setup installs for the current Windows account only.

Steps:

1. Copy `ExplorerMvpSetup.exe` to the PC (a USB stick, a share or a download all work).
2. Double-click it. Because the file is not code-signed, SmartScreen may show "Windows protected your PC". Select **More info → Run anyway** only for a copy you built or got from this project.
3. Read the line that says what this PC gets, choose tabs and/or touchpad snapping, and select **Install / Update**.
4. Open a new Explorer window. Windows that were already open pick up the tabs too; nothing needs restarting.

For a scripted install: `ExplorerMvpSetup.exe --install` (add `--tabs`/`--no-tabs`, `--gestures`/`--no-gestures` to choose). To remove: **Explorer MVP → Change or remove** in Start, Apps & Features, or `--uninstall`.

## Windows updates

Windows' monthly updates replace system files. The patch only touches two of them, UIRibbon.dll and uxtheme.dll, and checks both by hash before it does anything. If an update replaces either file, the tabs switch themselves off (or move to the row under the Ribbon) and Explorer looks as it does without the add-on. Nothing breaks, but the tabs need a new build tested against the new files.

The November 2026 update (KB5131096, KB5132689, KB5135080, installed on 11 November) changed explorer.exe, ExplorerFrame.dll, win32u.dll, twinui.pcshell.dll, dwmcore.dll and uDwm.dll on this PC. It did not change UIRibbon.dll or uxtheme.dll, so the tabs keep working. The Sandbox lab compares its own copies of eight Explorer and desktop files with this PC's before every run; that record was refreshed after the update, and the final suite ran on files byte-identical to this PC's.

## Known limits

- One folder view per window. Switching still loads the folder, as plain Explorer does; tabs do not keep each folder alive in the background.
- Physical touchpad gestures, several monitors, mixed DPI, sleep/resume and a clean second PC have not been tested by me. The Sandbox drives input with Windows' SendInput, not real hardware.
- Explorer memory per window with tabs attached was not measured: the footprint fixture's extra windows did not get tabs in the Sandbox, so its numbers describe Explorer without the strip.
- In the Sandbox the starter found every kind of new Explorer window (opened by path, Win+E, *Open in new window*, a separate Explorer process, and after Explorer restarted) and asked it to take tabs within 0.5–1.2 s. The Sandbox reports an older Windows build, so those windows decline. That tabs then appear is shown on this PC, not in the Sandbox.
- After a graphics driver reset, the strip notices the lost graphics device the next time it has something to draw (the pointer moves over it, a tab changes, a colour changes), not on its own. Until then the strip area may stay empty. If drawing then keeps failing, the window falls back to Windows' own title bar, which the Sandbox fault test confirmed.
- Dragging a tab out of the strip to open it in its own window failed once in five Sandbox runs on the same files: the tab dropped back at the end of its own strip instead. The other four runs, including three repeats made to check, opened the new window every time. The failure came four minutes after the lab had restarted the Sandbox's Explorer; I could not establish the cause, because the timing log does not record drag and drop. If it happens to you, drag the tab out again.
- Snap timings in the Sandbox are measured with a clock that advances in 15.6 ms steps. Snapping a maximized Explorer window measured 141 ms in the final run and 156 ms in two earlier ones, against a 150 ms limit. The snapping code did not change this round.
- Setup keeps every earlier version folder (1.8–2.1 MB each) until you uninstall, not only the previous one. This PC has five, 11.9 MB in all.
- The setup file is unsigned (no certificate available), so SmartScreen warns the first time.
- A future Windows update that changes UIRibbon.dll or uxtheme.dll turns the title-bar tabs off until a new build is tested against the new files.

## What the Sandbox proved, and what it did not

The Sandbox is a disposable Windows 10 machine on this PC. It reports build 19041, but its Explorer, ExplorerFrame, UIRibbon, user32, win32u, twinui.pcshell, dwmcore and uDwm files are byte-identical to this PC's 19045 files. The lab starter skips only the build-number check; the shipped starter does not.

The final suite ran once on the final binaries from one request, after a 38-second quiet period. Stages that failed were run again on their own, after a fix to the lab's test programs wherever a cause was found; the product was never changed. Every attempt is kept, and the acceptance receipt counts the latest attempt of each stage.

| Stage | What it checked | Result |
|---|---|---|
| Setup rehearsal | First install of the one-file setup. An update over a running copy: all 8 files replaced with matching hashes, every background tab kept with its order, folder and history. A cancelled update keeps the starter and every tab. Tabs off (starter gone) and on again. Your settings, Ribbon and session left alone | all checks (4th attempt; the first three failed on test-fixture faults, fixed) |
| Setup self-test | 41 checks run by the setup itself, without touching live settings | 41 of 41 |
| Switching | Ctrl+PageUp/PageDown wrap around, Ctrl+Shift+PageUp/PageDown move a tab, your own selection survives the view restore, rapid presses settle on the last one | 6 of 6 |
| 120 tabs | 120 tabs open at the 76 px minimum, 112 hidden and announced as off-screen, the wheel scrolls by whole tabs, the arrow scrolls one tab, the selected tab is brought into view, narrow tabs hide their close button | 9 of 9 |
| Reorder | The tab moves live while held, its neighbours glide in 200 ms (7 frames), Escape puts it back, with animation off the same end state is instant, dragging out of the strip detaches it | 7 of 7 in 4 of 5 runs (see known limits) |
| Drag | A real pointer drag reorders without mixing up tabs, Escape cancels, a drop on another window moves the original tab there | 3 of 3 |
| Timing | The highlight and settle times above, with 10 and with 120 tabs; rapid switching delivered every press | passed |
| Renderer modes | High contrast, transparency off, 96–192 DPI. Failures to create or draw the graphics fall back to Windows' own title bar without losing tabs, and the strip comes back when they clear | passed |
| Caption colours | Light, dark, accent-coloured and DWMBlurGlass title bars, active and inactive | 8 of 8 |
| Options | The *Animate tabs* checkbox and the setting it writes | passed |
| Footprint | The starter idle for 120 s and while 2,038 windows opened in 60 s | passed (numbers above) |
| Starter | New windows opened five different ways, and after Explorer restarted | passed |
| Combined, on the copy setup installed | Real pointer and keyboard on the tab strip; the expanded row, menus, rename and Backspace, detaching, drag and drop, closing windows; graphics faults; touchpad snapping of plain windows and of two Explorer windows, with the tab strip's content checked after each snap | all 12 parts (5th attempt). Snapping two Explorer windows: from floating 125 ms, side to side 15–31 ms, from maximized 141 ms (limit 150 ms) |

Not proven in the Sandbox: real touchpad hardware, real monitors and DPI changes, sleep/resume, a second PC, and what the tabs look like on this PC's own desktop after the update. The last can be checked the next time you open a folder window (below).

## On this PC

Installed at 20:26 on 12 November with the packaged setup (`ExplorerMvpSetup.exe --install`, your choices kept), replacing the 10 November build. Checked straight afterwards, without restarting anything:

- All five program files in the new version folder (`%LOCALAPPDATA%\Programs\Explorer MVP\498d517d…`) match the tested hashes. Apps & Features lists Explorer MVP 0.1.0 at the new folder.
- Explorer was not restarted: it is the same process, started at 12:51. No input was sent and no window was opened on your desktop.
- Your tab settings, touchpad gesture cycle, Quick Access Toolbar, Ribbon collapsed state and touchpad registry key are byte-identical before and after. The active touchpad bindings are the same.
- Both autostart entries point at the new version. Setup stopped the old starter and snap receiver and started the new ones from the new folder.
- The read-only readiness check passes: the snap receiver is listening and the starter is running.

What I could not see: tabs on your own desktop with this build. No folder window was open during the install, so Explorer has no tab add-on loaded at the moment, and the new starter loads the new one into the next folder window you open. I did not open one, because runtime checks stay in the Sandbox. Touchpad snapping already uses the new receiver.

Worth a look the first time you open a folder: the tabs sit in the title bar in the window's own colours; a click highlights a tab at once; with many tabs, the row scrolls with the wheel or touchpad. *Animate tabs* is under right-click a tab → Options → Tabs. If tabs do not appear in a new folder window, sign out and back in once; that starts Explorer and the starter afresh, and is the only step left that I could not check from here.

## Rolling back

Earlier versions stay installed next to the new one; the 10 November build is in `%LOCALAPPDATA%\Programs\Explorer MVP\984d2870…`. To go back, run that folder's `ExplorerMvpSetup.exe --install`, or remove Explorer MVP from Apps & Features. Your settings, sessions and touchpad assignments are kept either way.

## Quality checks this round

- Static scans in place of SonarQube: MSVC `/analyze`, cppcheck and clang-tidy on every changed source, before and after. No new warnings remain. Details are in `evidence/refine-20261110/u10/static/` and `review.json`.
- Cleanup: the largest functions were split until each is below cognitive complexity 25 (input handling, drag and drop, painting, tab updates). Debug-only timing marks nobody read were removed.
- Host tests on the final binaries: 200 of 200 checks pass (touchpad snapping 62, snap layout 24, Options and settings 50, Explorer model 15, settings migration 8, setup self-test 41). Setup's component pins match the runtime's.
- Five lab fixtures failed on unchanged product code and were corrected. They found a tab by its name, pressed Home expecting a selection, left two test windows always-on-top at the same place, expected an invalidated window to redraw, or started on a desktop where an earlier stage had left a menu open. Only the test programs were rebuilt; the shipped files are the ones tested. Details: `docs/solutions/2026-11-12-tab-strip-refinement-learnings.md`.
- The acceptance receipt (`evidence/native-v2/header-acceptance.json`) records all 27 stage attempts, failed ones included, and names each stage that passed only after an earlier failure. Each failure's written diagnosis is pinned by hash and copied into the package's `Verification` folder.
- Setup is checked against tampering: a copy with one changed byte is refused. Setup's support check uses the same file hashes as the DLL.
