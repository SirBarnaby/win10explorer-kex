---
title: Explorer tabs - faster switching, many tabs, Firefox-style motion, one-file setup - Plan
date: 2026-11-10
type: feat
artifact_contract: ce-unified-plan/v1
execution: code
origin: user request 2026-11-10 (Claude project thread "Refine Windows 10 native tabs and snap")
predecessor: docs/plans/2026-11-04-2233-fix-native-explorer-header-plan.md (accepted 2026-11-10, receipt evidence/native-v2/header-acceptance.json)
planned_by: Claude Opus 5.5, extra-high effort
executor_effort: high
---

# Explorer tabs refinement - Plan

## Goal Capsule

- **Objective:** The tabs the user already likes become a finished product: switching feels instant, dozens or hundreds of tabs behave like Firefox, opening/closing/reordering move like Firefox, colours are right in every Windows mode, the footprint is measured and small, and one setup file installs it on another Windows 10 22H2 PC.
- **Means:** Change the tab model, layout and renderer inside `ExplorerTabs.dll`, the hidden starter's window watching, and setup packaging. The accepted caption/Ribbon integration (`HeaderIntegration.cpp`) and the snap receiver stay as they are.
- **Authority:** The user's message of 2026-11-10 (quoted in Sources) asks for bug fixes, refinements, practical additions, a tab-limit check, "very preliminary" Firefox animations, faster tab switching "if the scope does not get out of hand", a performance answer, a SonarQube recheck, simplify, compound engineering and an easy one-exe install. They are happy with the current look and behaviour: nothing that already works may regress.
- **Execution profile:** One local update on `feature/explorer-mvp`, measured first, unit-tested, verified in the existing Windows Sandbox lab, reviewed, packaged, then installed on the user's desktop through setup (previous payload retained for rollback).
- **Stop conditions:** Any change that would need new hooks, new hard-coded offsets, relaxed version/hash guards, an Explorer restart on the host, or Computer Use on the host. Stop and report instead.

---

## Product Contract

### Summary

Keep the real-Explorer header exactly as accepted. Inside it: select tabs optimistically and never drop rapid switches; restore each tab's view once, when the folder is ready; save sessions lazily; raise the tab cap from 64 to 500 with a Firefox-style overflow (narrower minimum, scroll arrows, wheel and touchpad scrolling, faded edges, smooth scroll-into-view); run open/close/reorder/scroll motion on the DirectComposition compositor with Firefox's timings; fix caption text colours for dark, accent and DWMBlurGlass captions; reduce the background starter's system-wide event traffic; and ship one self-contained setup exe.

### Problem Frame (verified in source, 2026-11-10 tree)

1. **Switching feels slow.** `Activate` (ExplorerTabs.cpp:1634) starts a Shell navigation and sets `pending_`. Until `Synchronize` (1642) sees the new folder, the old tab stays highlighted and the whole strip is drawn disabled (`enabled=!state.busy`, TabHeaderRenderer.cpp:171/216/218). While `pending_` is set, clicks (2174), menus (2045), `InvokeTarget` (2137), close (1727), new tab (1702) and accessibility actions (3204/3217) are silently dropped, so tapping Ctrl+Tab three times moves one tab. After arrival, view state is re-applied up to 10 times at 100 ms intervals (`restoreTries_`, 2824 and the `RestoreMessage` handler), which re-sorts/re-selects for about a second and can overwrite a selection the user just made. Every tab change also serialises every tab into the registry synchronously (`TabChanged` -> `SaveSession`, 1776/1823).
2. **Tab limit and overflow.** `MaxTabs = 64` (55). `BuildLayout` (HeaderLayout.cpp) clamps tab width to 92-200 DIP and hides tabs beyond capacity; only the active tab is kept in view. There is no wheel scrolling, no scroll buttons and no hint that tabs are hidden; the chevron menu is the only way to reach them. `MaxMetadata = 64` (137) bounds the name/icon queue. A session over `MaxPacket` (8 MiB) fails to save.
3. **No motion.** The renderer paints one surface per strip (TabHeaderRenderer.cpp `Paint`); tabs appear, vanish and jump instantly. Reordering only happens on drop, through OLE drag-and-drop with no live feedback.
4. **Colours.** Caption text uses the light `WINDOW` theme's caption colour (TabHeaderRenderer.cpp ~133). Dark-mode captions, "Show accent colour on title bars" with a dark accent, and the user's DWMBlurGlass text colours (active black, inactive white per `dwmblurglass-config.ini`) are not taken into account, so inactive or dark windows can show low-contrast tab text.
5. **Footprint.** `ExplorerMvpHost.exe` listens to `EVENT_OBJECT_CREATE..EVENT_OBJECT_HIDE` for every process (ExplorerMvpHost.cpp:195), so every window/tooltip/menu show or hide in Windows wakes it. Each Explorer window creates its own hardware D3D11 device (TabHeaderRenderer.cpp:76). Every `Layout` renders twice (direct `Render` plus `InvalidateRect`), including on plain window moves.
6. **Install.** Setup copies eight sibling files (MvpSetup.cpp:30/163), so another PC needs the whole extracted folder. Setup does not tell the user up front whether that PC's `UIRibbon.dll`/`uxtheme.dll` match the pinned hashes (HeaderIntegration.cpp), which decides between caption tabs, row tabs or no tabs.
7. **Small defects from the baseline scan.** Use-after-move of `result` in the metadata worker (ExplorerTabs.cpp:223/236, benign today because the moved field is an integer, but fragile); unchecked `std::optional` access in `Timer` (2857-2868); 32 functions above Sonar's cognitive-complexity limit (KeyProc 104, Draw 103, FrameProc 79, Timer 71, StripProc 60, Drop 53, Enable 45).

### Requirements

**Speed**

| ID | Outcome |
|---|---|
| R1 | A click, Ctrl+Tab, Ctrl+PgUp/PgDn, Ctrl+1-9 or menu choice highlights the target tab in the same frame. The strip is never drawn disabled during a switch. |
| R2 | Rapid switches are never dropped: consecutive activations collapse to the last one; close/new/reopen requested during a navigation are queued (max 8) and replayed in order. |
| R3 | A busy indicator appears on the requested tab only if its folder takes longer than 300 ms. |
| R4 | View state is restored once, when the folder view reports its items (DShellFolderViewEvents `EnumDone`/initial-enumeration-done), with a single 1.5 s fallback. Any user input in the view cancels a pending restore. View mode, sort and grouping are only set when they differ from the view's current values. |
| R5 | Sessions are written after 1.5 s of quiet and on close, detach, disable and end-session; never synchronously on every tab change. |
| R6 | A window move or a repaint with unchanged geometry does not re-run allocation or redraw tab content. |

**Many tabs**

| ID | Outcome |
|---|---|
| R7 | Up to 500 tabs per window (constant `MaxTabs`), with names/icons loaded visible-first, a bounded session (<= 1 MiB: background tabs keep their current entry plus up to 16 history entries; selections only on current entries), and responsive menus and accessibility. |
| R8 | Tabs shrink from 200 DIP to Firefox's 76 DIP minimum, then the row scrolls: wheel (one tab per notch), horizontal touchpad scroll (pixel exact), and scroll arrows at both ends with press-and-hold repeat. Faded edges show hidden tabs. The active tab scrolls into view. The new-tab button pins to the right end while overflowing. The chevron "list all tabs" menu stays. |
| R9 | Tabs narrower than 140 DIP show their close button only when selected (Firefox `tabClipWidth`); middle-click still closes any tab. A native tooltip shows the full name and folder path. |
| R10 | Closing tabs with the mouse keeps tab widths locked until the pointer leaves the strip, so the next close button lands under the pointer; then widths settle. |

**Motion (Firefox timings, verified in Firefox tabs.css)**

| ID | Outcome |
|---|---|
| R11 | Open: the new tab grows from 0 to its width in 100 ms ease-out while its content fades in; neighbours slide. |
| R12 | Close: the tab shrinks to 0 in 100 ms ease-out; neighbours slide in; width unlock (R10) settles in 150 ms ease-out. |
| R13 | Reorder: dragging a tab horizontally inside the strip moves it with the pointer while the others slide aside with 200 ms `cubic-bezier(.07,.95,0,1)`; release settles with the same curve. Leaving the strip vertically hands over to the existing OLE drag (move to another window / new window). Ctrl+Shift+PgUp/PgDn moves the active tab with the same slide. |
| R14 | Scrolling, scroll-into-view and edge fades animate (150 ms ease-out). |
| R15 | Motion runs on the DirectComposition compositor, so it stays smooth while Explorer's UI thread is busy creating a folder view. It honours Windows' "Show animations in Windows" (`SPI_GETCLIENTAREAANIMATION`) and a new Options switch "Animate tabs" (default on). With either off, every change is instant. |

**Look and footprint**

| ID | Outcome |
|---|---|
| R16 | Caption text, glyph and underline colours follow, in order: high contrast system colours; DWMBlurGlass's own active/inactive (light/dark) text colours when it is running; Windows dark captions; "accent colour on title bars" (Windows' light/dark text rule); then the theme. Row mode keeps borrowing the real ReBar/Toolbar themes. No change to the accepted glass/material path. |
| R17 | The starter only wakes for top-level window creation/destruction (shell hook or an equally narrow filter), and idles at 0% CPU. |
| R18 | Graphics cost per Explorer window is measured; if a window adds more than 8 MB private bytes, share one D3D11/D2D device per Explorer process (multithread-protected) and keep one DComp device per window thread. Bitmaps/brushes are created once per device, not per paint. |
| R19 | A measured performance report (idle CPU, memory per process and per window, switch latency vs plain Explorer navigation) is written to `docs/reports/`. |

**Install, options, quality**

| ID | Outcome |
|---|---|
| R20 | `ExplorerMvpSetup.exe` is one self-contained file: the payload is embedded, checked against an embedded SHA-256 table and extracted into the existing immutable payload directory. Double-click shows the existing dialog; `--install`/`--quiet` work for scripted installs. Before installing it states what this PC will get (caption tabs, row tabs, or unsupported Windows build), from the same checks the DLL uses. The extracted-folder flow keeps working for development. |
| R21 | Options gains "Animate tabs" (stored as an inverted bit so existing configs default to on) and lists the new shortcuts. |
| R22 | Stand-in SonarQube scan (clang-tidy + cppcheck, plus MSVC `/analyze` on the PC) shows no new findings in touched code; touched functions are split below cognitive complexity 25 where practical (KeyProc, Draw, Timer, Drop at minimum). The two concrete scan findings in Problem Frame 7 are fixed. |
| R23 | A `/simplify`-style pass on the diff, a compound-engineering review (correctness, reliability, adversarial, maintainability), and a compound learnings note in `docs/solutions/` linked from `AGENTS.md`. |
| R24 | Everything accepted before (header placement, glass, QAT/Ribbon untouched, input, accessibility, detach, session hand-off, setup update/rollback, snapping) still passes on the final binaries. |

### Key Decisions

- **KD1 Optimistic selection, not live views.** A Windows 10 frame has one Shell view. True instant switching would need one hosted view per tab (Windows 11's approach): a rewrite of the navigation model, extra memory per tab, and new risk. Out of scope; the perceived delay is mostly our own disabled strip, dropped input and the 1-second restore loop, which this plan removes.
- **KD2 Compositor-driven motion.** Tab switching and new tabs coincide with Explorer creating a folder view on the same UI thread (tens to hundreds of ms). Timer-driven frames would stall exactly then, and `SetTimer` only ticks at ~64 Hz. DirectComposition animations run in DWM independent of that thread. The DComp device already exists.
- **KD3 Firefox numbers, Windows switches.** 100 ms ease-out open/close, 200 ms photon curve for reorder, 150 ms for settle/scroll/edges, 76 DIP minimum, 140 DIP close-button threshold. Off when Windows animations are off.
- **KD4 Hooks are frozen.** No edits to `HeaderIntegration.cpp` mechanics, hash pins or offsets. Wider Windows-build compatibility (symbol-based offsets, like Windhawk) is a separate follow-up the user can choose later.
- **KD5 Cap, not unlimited.** 500 is far beyond practical use and keeps every structure bounded.
- **KD6 One file, same installer.** Embed the payload in the existing per-user setup rather than adding an MSI or a new bootstrapper.

### Acceptance Examples

- AE1: With five tabs, tap Ctrl+Tab three times within 300 ms: the highlight moves three tabs immediately; the view ends on the third tab; no intermediate tab's restore runs.
- AE2: Select two files in tab A, switch to B and back: both files are selected again, once. Click a different file within 200 ms of arriving: that click wins.
- AE3: Open 120 tabs: tabs shrink to 76 DIP, arrows and faded edges appear, the wheel scrolls one tab per notch, the active tab stays reachable, the session saves (<= 1 MiB) and restores.
- AE4: Close three tabs by clicking the same spot: each close button arrives under the pointer; moving the pointer off the strip lets the tabs widen smoothly.
- AE5: Drag the second tab past the fourth: the third and fourth slide left as it passes; release settles it; drag it down out of the strip: it opens in a new window as before.
- AE6: With "Show animations in Windows" off, every change is instant and identical in end state.
- AE7: Dark mode, accent title bars (dark accent) and the user's DWMBlurGlass config each give readable tab text that matches the native title text in active and inactive windows.
- AE8: Copy only `ExplorerMvpSetup.exe` to a clean Windows 10 22H2 Sandbox, double-click, install: tabs and snapping work; setup's message stated what that PC supports.

### Scope Boundaries

- Not changing: snapping behaviour (`NativeSnap.exe`, `SnapLayout.*`), Ribbon/caption allocation and hooks, the session hand-off protocol, QTTabBar migration.
- Not doing: per-tab live Shell views, pinned tabs, tab groups, tab previews, multi-select of tabs, a GitHub repository or release (prepare files only), code signing (no certificate available; note SmartScreen in the README).

---

## Planning Contract

### Key Technical Decisions

- **KTD1 Request vs committed selection.** Add `requested_` (tab id). `Activate` sets it and repaints; if no navigation is in flight it navigates, otherwise the request waits. `Synchronize` commits `active_`, then navigates again if `requested_` differs, without restoring the superseded tab. Ctrl+Tab/PgUp/PgDn/1-9 index from the requested tab. Other blocked commands go into `std::deque<Command>` (max 8), replayed after commit, cleared on disable, error, teardown or the 15 s timeout.
- **KTD2 One restore, event-driven.** On each new view, advise a small sink on the view's `SVGIO_BACKGROUND` dispatch (`DShellFolderViewEvents`): DISPID 201 (EnumDone) and 223 (initial enumeration done) trigger `Restore` once. Fallback one-shot at 1.5 s. The existing WH_GETMESSAGE hook cancels a pending restore on mouse or keyboard input aimed at the view. Use a separate sink object, not the controller's `IDispatch`, so DISPIDs cannot collide with `DWebBrowserEvents2`. Unadvise on navigation, disable and teardown.
- **KTD3 Layout as data.** `BuildLayout` takes `scroll` (px), `lockedWidth` (optional) and returns content width, `maxScroll`, arrow rects, edge-fade flags and per-tab `showClose`. Pure, unit-tested, no Win32 calls besides `MulDiv`.
- **KTD4 Visual tree.** Renderer becomes: root -> background (row mode only) -> tab viewport (clip) -> scroller (animated OffsetX = -scroll) -> one visual + surface per realised tab (animated OffsetX, animated clip right edge, animated opacity) -> edge fades -> controls (arrows, +, chevron). Tabs are realised only inside the viewport plus any range being scrolled or animated. Hover/press/focus/selection repaint only the affected tab surface. One `Commit` per state change.
- **KTD5 Easing on DComp.** `IDCompositionAnimation` only has cubic segments in time, so a helper samples a CSS cubic-bezier into Hermite cubic segments (denser near t=0 for the photon curve; 16-24 segments), ending with `End(duration, to)`. Pure math is unit-tested: max error < 0.5% of travel versus the exact bezier.
- **KTD6 Motion diff.** `header::Motion` compares the previous and next `LayoutState` by tab id: new ids grow (clip 0 -> width, opacity 0 -> 1), removed ids become ghosts that shrink and are dropped after their duration (one-shot timer), moved ids slide. Reorder drag sets the dragged visual's offset directly per mouse move; others animate when the dragged centre crosses their centre.
- **KTD7 Local reorder before OLE.** A horizontal drag inside the strip uses mouse capture and KTD6. When the pointer leaves the strip vertically by more than half the strip height, the local drag is cancelled (others settle back) and the existing `StartDrag`/`DoDragDrop` path takes over, unchanged.
- **KTD8 Lazy session.** `SaveSession` marks dirty and arms a 1.5 s timer; `FlushSession` encodes within a 1 MiB budget (KTD for R7), writes once. Flush on WM_CLOSE, WM_ENDSESSION, detach, disable.
- **KTD9 Caption colour policy** as R16, in one function `CaptionInk(frame, active)`, unit-testable with injected inputs. DWMBlurGlass detection: find its running process / config file and read `activeTextColor`, `inactiveTextColor`, `activeTextColorDark`, `inactiveTextColorDark` (ARGB). Verify the byte order against a native title-text pixel in Sandbox before relying on it.
- **KTD10 Starter wakeups.** Replace the system-wide `EVENT_OBJECT_CREATE..HIDE` WinEvent range with `RegisterShellHookWindow` (`HSHELL_WINDOWCREATED`/`HSHELL_WINDOWDESTROYED`), keeping the existing `Refresh()` sweep on settings changes and start. If the Sandbox shows a missed folder window (for example one created hidden and shown later), keep a WinEvent hook for `EVENT_OBJECT_SHOW` only, filtered to `explorer.exe` process ids, instead.
- **KTD11 Graphics sharing is measurement-gated** (R18). Regardless of the measurement, cache D2D bitmaps/brushes per device by moving to `ID2D1DeviceContext` via `DCompositionCreateDevice2` with an `ID2D1Device`, so `BeginDraw` returns a device context and resources survive across draws.
- **KTD12 Embedded payload.** CMake builds the payload first, then compiles `MvpSetup.rc` with `RCDATA` entries for each payload file plus a generated SHA-256 table header. Setup prefers embedded resources, falls back to siblings (dev). The immutable payload id stays the combined hash.

### Measurement plan (do this first, U1)

Add a compile-time-optional timing log (QPC, in-memory ring, dumped by `HeaderLab --timing` or a registry-gated debug flag; never on in release builds by default). In the Sandbox, with 2/10/120 tabs on the same two test folders (one with 2,000 files):
- click -> highlight painted; click -> `NavigateComplete2`; -> `DocumentComplete`; -> EnumDone; -> restore applied; plain Explorer address-bar navigation between the same folders for comparison.
- `ExplorerMvpHost.exe` and `NativeSnap.exe`: CPU time over 5 idle minutes and over 2 minutes of opening/closing menus and tooltips elsewhere; private bytes.
- `explorer.exe` private bytes with 1 and 5 folder windows, tabs on vs off.
- Renders per second during a 5 s window drag.
Record baseline, then repeat after U2-U8. Put both in the report.

### Sequencing

U1 measure -> U2 switching -> U3 session/render dedupe -> U4 layout+overflow (pure) -> U5 renderer visual tree -> U6 motion -> U7 colours -> U8 footprint -> U9 setup/options -> U10 quality, acceptance, install, report. U2/U3 ship value even if U5/U6 slip; commit after each unit.

### Risks

- Renderer rewrite could regress the accepted glass/material match: rerun the existing renderer-modes/material harness after U5 before continuing.
- `EnumDone` may not fire for some namespaces (This PC, Network, search results, libraries): the fallback covers them; test each.
- DComp animation of a clip needs `IDCompositionRectangleClip` with animatable edges; if it misbehaves, animate a scale transform on a child content visual instead.
- Shell hook messages arrive only while a shell is registered; the existing `Refresh()` on start covers the gap.

---

## Implementation Units

### U1. Baseline measurements and timing log
- **Files:** `native/ExplorerTabs.cpp` (timing points behind `#ifdef`/registry gate), `native/HeaderLab.cpp` or a new `native/TimingProbe.cpp`, Sandbox payload script.
- **Done when:** baseline numbers from the Measurement plan are saved to `evidence/refine-20261110/baseline.json`.

### U2. Optimistic selection, coalesced switching, queued commands, one-shot restore
- **Files:** `native/ExplorerTabs.cpp` (Controller: `requested_`, command queue, `Activate`, `Navigate`, `Synchronize`, `Timer`, KeyProc table, view-events sink), `native/TabHeaderRenderer.*` (busy only on the requested tab after 300 ms, never disabling the strip).
- **Approach:** KTD1, KTD2. Add Ctrl+PgUp/PgDn (switch) and Ctrl+Shift+PgUp/PgDn (move). Replace the `restoreTries_` loop and `RestoreMessage` retries. Make `Restore` compare before setting. Split `KeyProc` into a table of shortcuts.
- **Tests:** a pure `SwitchModel` reducer test (requests, commits, errors, timeouts, queue overflow) in a new `native/TabModelTests.cpp`; Sandbox: AE1, AE2, namespaces This PC/Network/Libraries/search results, navigation error, 15 s timeout.

### U3. Lazy session and render de-duplication
- **Files:** `native/ExplorerTabs.cpp` (`SaveSession`/`FlushSession`, `Layout`, `Paint`).
- **Approach:** KTD8. In `Layout`, return early when allocation bounds, revision, DPI, tab ids, requested/active ids, scroll and lock are unchanged. Keep the queued-paint contract noted in `simplify-result-20261110.md`, but `WM_PAINT` only redraws when the content epoch changed (DComp keeps presented content). Fix the metadata use-after-move (capture the cookie before the move) and the unchecked optionals in `Timer`; split `Timer` by duty.
- **Tests:** unit test for the save scheduler and budgeted encoder; Sandbox: session survives close, end-session, crash-free restart; renders per second during a window drag drop to ~0.

### U4. Overflow layout (pure)
- **Files:** `native/HeaderLayout.h/.cpp`, `native/HeaderLayoutTests.cpp`.
- **Approach:** KTD3 and R7-R10. Raise `MaxTabs` to 500 in `ExplorerTabs.cpp`; metadata queue serves visible tabs first; hidden tabs report `STATE_SYSTEM_OFFSCREEN` and `accDoDefaultAction` scrolls them into view. Wheel/`WM_MOUSEHWHEEL`, arrow press-and-hold (`SetTimer`, system repeat rate), scroll-into-view, width lock and unlock on `WM_MOUSELEAVE`. Native tooltip (`TOOLTIPS_CLASS`) with display name and parsing path. Drag-over (files) still activates a hovered tab after 600 ms and auto-scrolls near the ends.
- **Tests:** layout tests for 1/8/64/120/500 tabs at 96/120/144/192 DPI, caption and row widths, scroll clamping, arrow visibility, close-button threshold, lock widths, hit-testing with scroll; Sandbox AE3/AE4.

### U5. Renderer visual tree and device context
- **Files:** `native/TabHeaderRenderer.h/.cpp`, `native/HeaderRendererStateTests.cpp`.
- **Approach:** KTD4, KTD11. Split `Draw` into background, tab, controls and edge-fade painters. Per-tab surfaces sized to the tab; realise/unrealise by range. Keep `Reset`/device-loss/`ThemeChanged` behaviour and the first-commit wait.
- **Tests:** existing renderer state/fault/mode harness must pass unchanged in Sandbox (native, faults, modes, DPI 96/120/144/192, high contrast, transparency off); visual comparison with the accepted captures.

### U6. Motion
- **Files:** new `native/TabMotion.h/.cpp` (easing segments, diff), `native/TabHeaderRenderer.cpp`, `native/ExplorerTabs.cpp` (local reorder drag, OLE hand-off), `native/MotionTests.cpp`.
- **Approach:** KTD5-KTD7, R11-R15. Read `SPI_GETCLIENTAREAANIMATION` and the Options bit on each layout change (and on `WM_SETTINGCHANGE`).
- **Tests:** easing accuracy and diff unit tests; Sandbox AE5/AE6, captures mid-animation (screen capture at known offsets) to show partial widths/offsets, and end states identical with motion on/off; reorder then detach; reorder while a folder is loading.

### U7. Caption colours
- **Files:** `native/TabHeaderRenderer.cpp` (or a small `native/CaptionInk.cpp` + test).
- **Approach:** KTD9, R16.
- **Tests:** pure policy test; Sandbox AE7 with light, dark, accent (light and dark accent), high contrast, transparency off, and the user's DWMBlurGlass config (`evidence/header-rebuild-20261104/dwmblurglass-config.ini`) active/inactive.

### U8. Footprint
- **Files:** `native/ExplorerMvpHost.cpp`, `native/TabHeaderRenderer.cpp`.
- **Approach:** KTD10, KTD11, R17-R19.
- **Tests:** starter attaches to windows opened from Win+E, Explorer's "Open in new window", `explorer.exe <path>`, a separate-process folder window and after Explorer restarts inside the Sandbox; idle CPU and memory repeated.

### U9. One-file setup and Options
- **Files:** `native/MvpSetup.cpp`, `native/MvpSetup.rc`, `native/CMakeLists.txt`, `Build-Mvp.ps1`, `native/Options.cpp/.rc`, `native/Patch.h` (`NoTabAnimations = 16`), `MVP-README.md`.
- **Approach:** KTD12, R20, R21. Setup's compatibility message: Windows build check, then `UIRibbon.dll`/`uxtheme.dll` hash checks reported as "Tabs in the title bar" / "Tabs in a row under the ribbon" / "Not supported on this Windows build". `Build-Mvp.ps1` emits `dist/<label>/ExplorerMvpSetup.exe` (single file) plus `Source.zip`; README notes SmartScreen for unsigned files.
- **Tests:** setup self-tests extended for embedded payload hash mismatch, partial extraction rollback, update over the currently installed version with open multi-tab windows (existing session hand-off), uninstall; AE8 in a fresh Sandbox with only the exe copied in.

### U10. Quality, acceptance, install, report
- Stand-in Sonar scan (R22), simplify pass, compound review (R23), all unit suites, the full Sandbox acceptance once on final binaries (R24), regenerate `evidence/native-v2/header-acceptance.json` honestly (new hashes, new checks `switching`, `overflow`, `motion`, `colours`, `footprint`, `singleFileSetup`), `Build-Mvp.ps1`, install on the user's desktop with `ExplorerMvpSetup.exe --install` (no Explorer restart; previous payload retained), and write `docs/reports/2026-11-10-refinement-report.md` with before/after numbers, what changed, what is untested on physical hardware, and how to roll back.

---

## Verification Contract

- Unit suites: `HeaderLayoutTests`, `HeaderRendererStateTests`, `SnapLayoutTests`, `HeaderHookOwnershipTests`, new `TabModelTests` and `MotionTests`, `Test-Mvp.ps1`.
- Sandbox: existing renderer-modes, behaviour (expanded/menu/edit/detach/drag/lifetime/graphics-loss), snap-generic and snap-explorer-pair suites once on the final binaries, plus the new AE1-AE8 cases. Reuse a running guest; run only affected cases while iterating; run the broad suite once at the end.
- Desktop: after install, a read-only probe (`ExplorerMvpHost.exe --probe`) shows the new DLL owning the tab strip in any open folder window; no Explorer restart, no Computer Use.

## Definition of Done

All requirements met or explicitly deferred with a reason in the report; acceptance receipt regenerated from real runs; single-file setup in `dist/`; installed on the desktop; report, learnings note and commits on `feature/explorer-mvp`.

## Executor rules

- Work in `C:\Users\barna\ReverseEngineering\Win10NativePatch` on branch `feature/explorer-mvp`. First commit the current uncommitted Codex work as a baseline (sources only; `.gitignore` already excludes build, dist and evidence), then one commit per unit. Do not create other branches; do not push anywhere.
- Build with the existing VS 2022 Build Tools + `C:\mingw64\bin\cmake.exe` (`build-mvp`, Release). Static CRT stays.
- Runtime tests only in Windows Sandbox using the existing lab (`sandbox/`, `evidence/header-rebuild-20261104/renderer-production/guest-*/payload`). Never restart Explorer, kill Explorer or drive input on the host. No Computer Use.
- Do not edit `HeaderIntegration.cpp` mechanics, hash pins, offsets or version guards. If a requirement seems to need that, stop and report.
- If any tool or safety system refuses a step, stop that step and report it; do not work around it.
- Keep the user's settings, sessions, QAT/Ribbon preferences and touchpad bindings exactly as they are.

## Baseline scan (2026-11-10, cloud, clang-tidy 18 + cppcheck 2.13 against MinGW headers)

All sources compile with MinGW g++ 13 `-fsyntax-only`. clang-tidy (bugprone, cert, clang-analyzer, performance, selected readability/cppcoreguidelines; cognitive-complexity threshold 25): 37 int-to-pointer casts (Win32 handles; ignore), 28 assignment-in-if (style), 32 functions over complexity 25, 4 unchecked optional accesses (`Timer`), 1 use-after-move (metadata worker), 5 implicit-widening multiplications, 5 `wcscmp` used as bool, 3 switches without default. cppcheck: no defects in project code. MinHook vendored code excluded.

## Sources

- User request, 2026-11-10 18:02 UTC: refinements, tab limit, Firefox-like animations, SonarQube/simplify/compound engineering, faster switching without scope blow-up, performance and nativeness questions, one-exe install, keep no tray icon.
- Firefox `browser/themes/shared/tabbrowser/tabs.css` (mozilla/gecko-dev master, fetched 2026-11-10): `--tab-min-width: 76px`; `.tabbrowser-tab:not([pinned])` `transition: min-width 100ms ease-out, max-width 100ms ease-out`; `--tab-dragover-transition: transform 200ms var(--animation-easing-function)` under `prefers-reduced-motion: no-preference`; `.closing-tabs-spacer` `transition: width 0.15s ease-out`; overflow indicators `transition: opacity 150ms ease`.
- Mozilla bug 1355507: reorder curve `cubic-bezier(.07, .95, 0, 1)` at 200 ms.
- `Research.md` (2026-10-25) survey of QTTabBar, ShellTabs, Groupy, WindowTabs, Windhawk, FancyZones, AltSnap, RectangleWin; a 2026-11-10 re-check found no new Windows 10 title-bar tab tool.
- Microsoft docs: `DShellFolderViewEvents`, `IDCompositionAnimation`, `DCompositionCreateDevice2`, `RegisterShellHookWindow`, `SPI_GETCLIENTAREAANIMATION`.
