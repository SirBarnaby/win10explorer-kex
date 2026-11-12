# Learnings from the tab strip refinement round (10–12 November 2026)

Problems worth knowing about before the next round of work on Explorer MVP, with what fixed them and how to avoid them again.

## Switching speed came from our own code, not Explorer

**Problem.** A tab click took a median of 220 ms to highlight and up to 3.4 s before the folder view looked right.

**Cause.** Explorer was not the slow part. The strip drew itself disabled while a navigation ran, dropped clicks that arrived meanwhile, restored the view up to ten times on a timer, and saved the whole session four times per switch.

**Fix.** Highlight the requested tab at once and let the latest request win (`requested_` vs `active_`). Restore the view once, when the view reports its items, with one 1.5 s fallback. Save the session once after 1.5 s of quiet.

**Next time.** Measure first with the timing log (`sandbox/Analyze-Timing.ps1`) before assuming the shell is slow. Count restores, saves and renders per switch, not only milliseconds.

## Builds that hang forever

**Problem.** `Build-Native.ps1`, MSBuild `/analyze` and single-target builds sometimes stopped making progress, with no error.

**Cause.** Old `MSBuild.exe`, `mspdbsrv.exe`, `vctip.exe`, `Tracker.exe` and `cmake --check-stamp-list` processes were left with every thread suspended. New builds reused them (node reuse, the shared PDB server endpoint, file tracking) and waited on them.

**Fix.** Before building, set `MSBUILDDISABLENODEREUSE=1`, `TrackFileAccess=false` and a fresh `_MSPDBSRV_ENDPOINT_` GUID. Build lab-only targets straight from their project files with `/p:BuildProjectReferences=false /nodeReuse:false /m:1`, so no CMake re-check runs inside MSBuild. For static analysis, call `cl.exe /analyze` directly instead of MSBuild.

**Next time.** If a build is quiet for more than a minute, look for suspended build processes (`Get-Process` thread `WaitReason` = `Suspended`) before changing code. A sign-out clears them. Agents cannot stop them in auto mode.

## Rebuilding after testing invalidates the evidence

**Problem.** MSVC Release output is not byte-identical between links. Any relink changes the SHA-256 of the binaries.

**Cause.** Link timestamps and PDB identifiers are embedded in every image.

**Fix.** Build once, stage those exact files into the Sandbox payload, test, then generate the acceptance receipt (`New-HeaderAcceptance.ps1`) and package with `Build-Mvp.ps1 -SkipBuild`. The receipt refuses binaries the guest did not test.

**Next time.** Do not rebuild between the final Sandbox run and packaging. If a code change is needed, rebuild, restage and rerun the affected Sandbox stages.

## The Sandbox gate stops after a Windows update

**Problem.** After the November 2026 cumulative update the guest refused every request.

**Cause.** The guest checks that its `explorer.exe`, `ExplorerFrame.dll`, `UIRibbon.dll`, `user32.dll`, `win32u.dll`, `twinui.pcshell.dll`, `dwmcore.dll` and `uDwm.dll` are byte-identical to the host's. The update changed six of them on both host and guest; the manifest (`payload/host-components.json`) still held the old host hashes.

**Fix.** Refresh `host-components.json` from the host after every Windows update (keep the old one alongside), then relaunch the Sandbox. The runtime pins (`UIRibbon.dll`, `uxtheme.dll`) were unchanged, so the patch itself kept working.

**Next time.** After Patch Tuesday, check the two runtime pins first, then refresh the lab manifest. A guest refusing to run is the gate doing its job, not a lab fault.

## A missing test folder looks exactly like a hang

**Problem.** The U10 suite stalled for 28 minutes in its first stage, twice, with no error.

**Cause.** The suite ran the setup rehearsal first. Its fixture navigates to `C:\NativeLabFolders\Gamma`, which only existed once an earlier stage had created it. Explorer answered the navigation with a modal "File Explorer" message box, and the fixture's `IWebBrowser2::Navigate2` call waited for someone to dismiss it. Every window still answered messages, so nothing looked hung.

**Fix.** The suite creates every fixture folder before the first stage. Each stage now runs in its own process with a time limit. On a timeout, `Capture-Hang.ps1` records each visible window (including dialog text), the thread states, and the code modules on the waiting threads' stacks. That is how the message box was found: the fixture's only thread was inside a COM call, and a `#32770` dialog sat on the Explorer frame's thread.

**Next time.** When a lab step stops with zero CPU, look for a dialog first. Do not reorder lab stages without checking what earlier stages create.

## Lab fixtures that assume more than the product promises

**Problem.** On the final binaries, the combined run's physical-input stage failed although nothing in the product had changed since U9. Later, the setup rehearsal could not prepare its second window.

**Cause.** Both were assumptions in the fixtures:

- `HeaderInteractionTests` closed "the selected tab" by looking up `Close <name>`. A new tab opens on the same folder as the tab it came from, so two tabs can share a name. Narrow background tabs hide their close button (U4, as in Firefox), and a hidden button reports an empty rectangle at the strip's top-left corner. The first match was the hidden one, so the click selected Alpha instead of closing anything. Every later step was then off by one.
- The behavior fixture leaves each frame it prepares always-on-top at the same place. With two frames, the first can stay above the second and take its click and Ctrl+T.
- The rename check pressed Home, then F2. In plain Explorer (no tab strip), a click on empty view space can leave keyboard focus on the first item, and Home then selects nothing, so F2 does nothing. The keys went to the folder view and Backspace correctly went back a folder.
- The graphics fault check injected a draw failure and only invalidated the strip window. The renderer draws only what changed, so often nothing was drawn and nothing failed.
- Stages assumed a clean desktop. The setup rehearsal left a folder window and an open popup menu (`#32768`) in the lab's Explorer process. An open menu takes the next click to close itself, so the combined run's first click and Ctrl+T never reached its window and it started with one tab instead of two.

**Fix.** Close a tab through its own close button: the accessible child right after its body. Push each prepared frame behind (not topmost, bottom of the z-order) before preparing the next. Verify mode needs no focus. Select the rename target through the folder view, press the real F2, and wait for the editor to take focus before typing. After injecting a draw fault, send the frame `WM_SYSCOLORCHANGE` so the strip redraws everything. Before each stage that drives input, `Reset-GuestDesktop.ps1` cancels open menus (`WM_CANCELMODE` to the menu owner from `GetGUIThreadInfo`), closes every folder window and records what it found in `reset-<stage>.json`.

**Next time.** Start every input-driving stage from a known desktop; `guest-windows.txt` (written when the suite stops) shows what an earlier stage left behind. Identify tabs by accessible child id, never by name. Wait for a state (focus on the editor) instead of a fixed delay. When a fixture fails on unchanged product code, compare the run's screenshots with the last passing run, and repeat the step in plain Explorer without the tab strip, before suspecting the product (`state-allocated-row-settled.png` showed the extra tab at once; `Diagnose-Rename2.ps1` showed Home not selecting in plain Explorer).

## Explorer restarted by Winlogon is not a crash

A guest Explorer whose parent is `winlogon.exe` and whose command line is a bare `explorer.exe` was restarted by `AutoRestartShell` after something stopped it. Check the Application log (`Application Error`, `Windows Error Reporting`) before calling it a crash. The lab payload has two read-only probes for this: `Probe-GuestCrashes.ps1` and `Probe-GuestState.ps1`.

## Closing the Sandbox needs the user

**Problem.** An agent in auto mode cannot stop `WindowsSandbox.exe`.

**Fix.** Ask the user to close the window, then relaunch the `.wsb`. Run the whole suite from one request file (`Run-U10Suite.ps1`) so one launch covers everything, and rerun only failing stages with `-Only`.

## A single-file setup needs a tamper test, not just a hash table

**Problem.** An embedded SHA-256 table proves nothing unless something checks that a changed byte is rejected.

**Fix.** `Test-Mvp.ps1` flips one byte inside the embedded licence text of a copy of the setup and expects the self-test to fail exactly `embedded-payload-intact`. It also checks that the setup's UIRibbon/uxtheme pins equal the runtime's pins, so the setup cannot promise support the DLL will refuse.

## Comparing static scans

Compare a scan of the base commit with a scan of the head by (file, rule, message), not by line number, or every edit looks like a new finding. `ccdiff.py`-style normalisation (digits removed from messages) keeps the diff to real changes.
