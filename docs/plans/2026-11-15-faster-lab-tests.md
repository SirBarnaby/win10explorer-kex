# Plan: faster Sandbox tests without weaker tests

Status: proposal, 15 November 2026. Nothing here is built yet.

## Where the time goes today

Measured on the 0.1.1 runs (14 and 15 November).

Stage time in one full suite is about 31.5 minutes:

| Stage | Seconds | What takes the time |
| --- | ---: | --- |
| migration | 374 | 9 setup runs, each started through Python + frida to spoof the build number |
| footprint | 309 | fixed 120 s idle sample + 60 s churn sample |
| final-combined | 289 | 8 behaviour runs, each with fixed 1-3 s waits after every action |
| strip-timing | 277 | 13 switch rounds with fixed 1.8 s + 2.2 s waits, plus fixed phase waits |
| setup-rehearsal | 173 | setup runs through frida, fixed waits |
| strip-overflow | 162 | fixed waits |
| keepalive | 70 | deliberate restart delays (3 quick ends) |
| everything else | 239 | 8 small stages |

The stage time is not the biggest cost. Over 14 and 15 November most of the wall-clock time went to:

- Waiting for a person to close the Sandbox window: 4 times. I am not allowed to end the Sandbox process from the host, so every fresh guest needed you.
- Rerunning flaky stages: final-combined ran 9 times on 15 November alone. 0.1.0 needed 5 runs of the same stage. Each failure was a different timing or focus check.
- Hidden ordering: final-combined only works after setup-rehearsal, and some stages only work on the sign-in shell. A lone rerun failed for reasons unrelated to the product.

So the plan attacks the three in order of payoff: the person in the loop, flakiness, then raw speed. Running less on small releases comes last.

## Rules the plan keeps

- No check is removed and no time or pixel budget is loosened.
- Every retry is recorded in the stage result, never hidden.
- The receipt still binds the exact binary hashes, guest component hashes and lab script hashes.
- A release still needs one full suite on a clean guest.

## Step 1: no person in the loop

- Close the guest from inside. A new lab request ends the guest with `shutdown /s /t 0`, which should close the Sandbox window by itself. To verify first: one try at the end of the current guest. If it does not close the window, the fallback is a one-time permission rule from you that lets me end only `WindowsSandbox*.exe`.
- One host command runs the whole cycle: `Invoke-LabRun.ps1 -Stages <list|all>`.
  - Start a fresh guest outside the app's job (WMI), wait for `ready.json`, run the stages, collect results, close the guest, file the records into the evidence folder.
  - No polling by hand, no stale guests reused by accident.
- Expected gain: the 30-90 minute waits for a window close disappear. A fresh guest costs about 1.5 minutes, so "start clean" becomes the default instead of the exception.

## Step 2: stop the flakiness

This is where most reruns come from. Each change makes the tests both faster and steadier.

- Replace fixed waits with waits for the actual state. `HeaderBehaviorTests`, `SnapIntegrationTests` and `HeaderLab` use `Pump(1200)`, `Pump(1800)`, `Pump(3000)` after actions. Each becomes "wait until the tab is selected / the frame has moved / the timing mark appeared", with the old value as the upper bound. A step that finishes in 80 ms no longer waits 1.8 s, and a slow step gets its full time instead of failing.
- Focus. Every snap-pair failure on 15 November started with the test failing to put its own window in front, or with Snap Assist taking focus. `Focus()` will retry, verify, and record each attempt. The product check that follows is unchanged.
- Snap timing budgets. The 150 ms checks measure one gesture. They will run each gesture 3 times and require the median to meet 150 ms, with all 3 timings recorded. A single slow frame no longer fails the stage. A genuinely slow product still does.
- Self-contained stages. Each stage sets up what it needs: final-combined installs the release itself if it is not installed, instead of relying on setup-rehearsal having run first. Any stage can then be rerun alone, and its result means the same thing.
- Stages that must see the sign-in shell run first. Stages that end Explorer run last (keepalive, starter, migration). This ordering is already in place since 15 November.

## Step 3: make each stage faster

| Stage | Change | Estimate |
| --- | --- | --- |
| strip-timing | state waits in the 13 switch rounds and phases | 277 → ~90 s |
| final-combined | state waits in the 8 behaviour runs | 289 → ~140 s |
| strip-overflow, -reorder, -drag, -switching | state waits | 277 → ~130 s |
| footprint | 30 s idle + 30 s churn, measured by CPU cycles and wake-ups instead of milliseconds | 309 → ~90 s |
| migration + setup-rehearsal | native build-spoof launcher instead of Python + frida; share one install between them | 547 → ~200 s |
| all stages | one precompiled lab helper instead of compiling C# (`Add-Type`) in every script | ~1-2 s per stage |
| suite start | cap the quiet-processor wait at 60 s | up to 3 min saved |

Notes:

- Footprint: a 30 s window still catches anything that wakes more often than every 30 s. CPU cycle counts (`QueryProcessCycleTime`) are far more precise than millisecond CPU time, so the shorter window loses no sensitivity for what the check is for: an idle starter that does not spin.
- Native spoof launcher: Microsoft Detours (open source, MIT) or a small injected hook on `RtlGetVersion`. It does the same thing frida does today, without starting Python and the frida runtime 11 times. Measure frida's share first; if it is small, skip this item.

Estimated result: a full suite of about 12-14 minutes instead of 31.5.

## Step 4: run less on small releases

- Reproducible release builds. Today a rebuild of unchanged source gives different hashes, so nothing earlier can be reused. Add `/Brepro` (compiler, librarian, linker), `/PDBALTPATH:%_PDB%` and check `.rc` output. Then rebuild twice and compare the hashes.
- Reuse of passing results. A stage result is keyed by the hashes of the binaries it exercises, the guest component hashes and its lab scripts. If none changed, the receipt may cite the earlier passing record, and it says so.
  - Stage-to-binary map: the strip-*, renderer-modes, caption-ink and behaviour checks depend on `ExplorerTabs.dll`; keepalive and starter on `ExplorerMvpHost.exe`; snap checks on `NativeSnap.exe`; options on `Win10PatchOptions.exe`; migration, rehearsal and self-test on the setup and all files it carries.
  - For 0.1.1 this would have reused about 12 minutes of tab-strip results: `ExplorerTabs.cpp` did not change.
- Two levels:
  - Development: only the stages touched by the change, on a fresh guest. A few minutes.
  - Release: the full suite once on a clean guest. Results may be reused only when every hash matches.

## How we know the tests did not get weaker

- Parity run: old suite and new suite against the same binaries; the set of checks and their pass results must match.
- Fault runs: the lab already injects faults (for example HeaderLab's draw fault). Run the new suite against known-bad cases and confirm each one still fails: a starter that spins, a snap that lands one pixel off, a tab switch that never completes, a setup that drops the user's settings.
- Repeat run: the new suite 3 times on clean guests with the same binaries. Target: no failures. The old suite fails most of the time.

## Order and size

1. Guest closes itself + `Invoke-LabRun.ps1`. Small; largest gain.
2. Self-contained stages + focus and median timing in the snap fixtures. Medium.
3. State waits in the behaviour and strip fixtures; footprint window. Medium.
4. Native spoof launcher (only if frida's share is large). Small to medium.
5. Reproducible builds + result reuse. Medium; needs a receipt format change.

Lab tools only change in steps 1-4; no shipped binary is touched. Step 5 changes build flags, so the first release built with it gets a full suite.
