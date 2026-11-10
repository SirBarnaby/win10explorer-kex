# Explorer MVP status

## 4 November 2026 follow-up: tab startup and snap transition fixes installed

The installed follow-up payload is `7d82f98aba3dc4472824ad72113316d619fb2da172329dbb6b5b65301d8a4e68`. Tabs had remained off in a newly opened frame after a temporary header-layout rejection; the controller now retries safely for up to two seconds. Persistent failure still restores ordinary chrome and preserves the saved session. Side-to-side snapping now repositions an already-arranged window directly, avoiding the native Win+Arrow restore cycle through its floating position.

Final Sandbox checks passed: 172 model checks, 32 native snapping checks, 83 core acceptance checks, and the temporary-header-failure recovery proof. Measured receiver transitions were 47 ms left-to-right, 62 ms right-to-left, 109 ms floating-to-left and 93 ms maximized-to-left, without restored-middle samples. The native divider resized both windows after their sides were swapped. These are guest measurements, not physical touchpad timing claims. The matching-component/base-build qualification in the earlier acceptance record remains applicable.

Both updated desktop components and their startup entries were verified after setup; all installed binary hashes match the tested build. Settings, Ribbon preferences and active touchpad bindings were preserved. No Explorer folder window remained open during this cutover, so final live-window appearance was not reverified on the host. No Explorer restart or Computer Use was performed. Evidence: `evidence/snap-latency-20261104-180945/final-acceptance.json` and `desktop-update-result.json` in the same directory. The earlier installed payload remains available for recovery.

## 4 November 2026, 17:16: installed on the user's Windows 10 desktop

The user authorized desktop installation after Sandbox delivery. The verified package installed successfully through `ExplorerMvpSetup.exe --install`, preserving Config v2 and the effective touchpad assignments. The new host and snap receiver run from immutable payload `7801d621dc338b6b1c8a93999ed4c80b833a729c6c51302ea81d574e90f7af85`; startup entries, Apps & Features and both Start menu links point there. All five installed binary hashes match the Sandbox-tested package. The native receiver confirmed active touchpad bindings.

A read-only inspection of the existing Explorer frame verified that the new DLL owned its active tab-strip class, every native command was inspectable, and no command or address-bar collision existed. The folder window later closed independently; no folder window remained at final inspection. No Explorer restart or Computer Use was performed. The saved Ribbon collapse preference was restored from the pre-install snapshot after its persisted state changed during the handover; QAT controls and order were unchanged. Physical gestures remain untested by the agent.

Installation and recovery evidence: `evidence/desktop-install-20261104-171601/result.json`. The prior payload and registry snapshots are retained. Earlier Sandbox-only statements below describe that earlier phase, not the current installed state.

## 4 November 2026: native header and shared resizing implemented

The header no longer subtracts a region from the Ribbon. It measures the original commands, shares available caption space, and reserves an extra row through the native Ribbon height notification when necessary. Narrow/wide transitions, exact restoration on detach and the original Navigation pane checkbox were exercised in Sandbox using the user's saved QAT layout. The checkbox changed the real pane and changed it back. Native command and address-bar overlap checks passed.

Snapping now enters Windows' arranged state and preserves it during frame adjustment. Native shared-divider dragging, remembered per-display proportions, side/corner reuse, repeat-Up maximization, Down restoration and cancellation after a focus change are covered by guest integration tests. Pure arithmetic/storage checks cover odd work areas, negative coordinates, independent displays and incompatible records. The installed desktop payload remains unchanged.

The guest reports base build 19041. Its Explorer, ExplorerFrame, UIRibbon, user32, win32u and twinui.pcshell binaries match the 19045 host byte for byte. The header lab temporarily adapts only the production DLL's version query while attaching, then restores that import; production files and version checks are unchanged. This qualification must accompany the guest evidence. The recorded screenshots come from the guest harness, without Computer Use.

Final evidence is under `evidence/sandbox/20261104-143632/`. All 172 model checks, 24 native snapping checks and 75 aggregate acceptance checks passed against the final binary hashes. The completed Compound Engineering review raised four P2 findings; all four were fixed and their required regression proofs passed. See `docs/header-acceptance.md`, `evidence/implementation-20261104/final-acceptance.json` and `review-fixes.json` in that evidence directory. Physical touchpad input, several physical monitors, mixed DPI, sleep/resume and public-distribution acceptance remain outside this Sandbox run. Earlier incomplete-status sections below are historical.

## 3 November 2026: combined plan and maximize change

The combined implementation plan is `docs/plans/2026-11-03-2005-fix-native-headers-snapping-plan.md`. The updated source and development binary now maximize on Up from either top corner, retain maximization on another Up, and restore the previous corner on Down. The revised checks failed on the old implementation first: eight failures among 62 checks. After the implementation change all 62 passed, including existing bottom-repeat/no-minimize, minimum-size, frame compensation and foreground-preservation checks. Reports are under `evidence/implementation-20261103/`.

This is a development change, not an installed update. Native shared-divider resizing, remembered split proportions and the allocated Explorer header are still incomplete and have not passed their required desktop proofs. The existing installed receiver and Explorer DLL remain unchanged.

Despite the user's reported restart, the available Windows session still reports a kernel boot timestamp of 28 October, CBS reboot/packages pending and no System32 Sandbox launcher. Do not infer successful Sandbox activation from yesterday's enable report. The first invasive header experiment and input-driving native pair test remain blocked until the matching isolated desktop is available.

## 2 November 2026: native-command overlap reproduced

The installed MVP does **not** meet header acceptance. A read-only MSAA inspection of the actual Explorer window on Windows 10 Pro 22H2, build 19045.7807, found four visible native commands intersecting the tab strip: Properties, New folder, Navigation pane, and Customise Quick Access Toolbar. The strip occupied screen rectangle `[107,540,819,570]`. The full command rectangles and states are recorded in `evidence/header-command-overlap-20261102.json`; the window hierarchy is in `evidence/header-baseline-20261102.json`.

The causal chain remains the fixed 40-DIP start in `Controller::Layout`, followed by `ClipRibbon` subtracting the strip's rectangle from the Ribbon's window region. The current settings still request tabs and gestures. The installed DLL, startup entries, toolbar customisation and snapping settings were not changed during this diagnostic pass.

`ExplorerHeaderProbe.exe` and `Test-HeaderIntegration.ps1` now inspect actual native Ribbon command rectangles without attaching, loading or stopping the extension. The check fails on the four overlaps above. Missing tabs, unavailable accessibility data or a truncated inspection cannot pass. `Test-MvpExplorer.ps1` now requires this check as well as caption placement; its previous caption-only success condition missed this bug. These checks establish geometry only, not appearance or correct input routing.

The next production change is still gated on the header-reflow experiment described in `docs/plans/2026-10-27-1900-fix-explorer-native-header-plan.md`. At the start of this pass Windows Sandbox was absent, and no Hyper-V management module, VMware or VirtualBox installation was found. The Windows screen-capture helper again failed with `FrameArrived timed out`; accessibility-only inspection succeeded.

The user subsequently approved enabling Windows Sandbox. `Enable-HeaderTestSandbox.ps1` ran successfully through an administrator process at 2026-11-02 14:51 UTC. Windows reports `Containers-DisposableClientVM` as `Enabled` and `RestartNeeded: true`; see `evidence/sandbox-setup-20261102/result.json`. Windows was not restarted, and the Explorer patch was not changed. The user will choose when to restart. After that restart, verify Sandbox launch, guest build 19045 and usable capture, then resume U2's allocated-header/reflow proof. Enabling the feature does not itself satisfy that proof.

## Historical MVP evidence — 27 October 2026

Target: Windows 10 22H2 x64. This PC: 19045.7807.

## Implemented

- Native tab control attached inside real Explorer folder windows by a hidden, per-user starter. No imitation file browser or external header window.
- Caption layout measured from the actual window buttons. Constant-opacity child surface and stable clipping prevent glass transparency and routine layout flicker.
- Native Explorer-owned Options with independent feature switches; separate hidden snap receiver, with no tray icon.
- Six snap destinations: left/right halves and four corners. Opposite horizontal directions select the requested half. Floating Up/Down does nothing. Never centre, maximise or minimise through this handler.
- Per-user offline setup, immutable payload directories, original gesture migration, Start-menu entry points, and truthful disable/removal status.

## Observed evidence

- 58 isolated snap checks pass, including a 24-transition position matrix, floating no-ops, repeated actions, manual repositioning, minimum sizes and visible-frame compensation. The previous implementation failed 24 revised checks before the fix.
- Configuration/restore model checks: 34 pass. Migration parser: 8 pass. Explorer model/metadata checks: 12 pass.
- A reversible live check attached the tab control as a child of Explorer window 1773090, in the caption-button row. Its Explorer PID 36140 was separate from desktop shell PID 12976. A normal-user execution context was required; sandbox process inspection could not see the target.
- The user reported initial faint/intermittent rendering. After the opaque child-surface and clipping correction, the user confirmed: "Yes, readable and stable now." That visual confirmation applies to DLL SHA256 FC8B10BD94DF04BCD150F3BE67CE65A475C79E62FCB686BF7E2DBDDBD93CDFAB.
- Native screen capture failed with FrameArrived/window capture timeouts. The user supplied visual evidence. Computer Use stopped on Escape; no further Computer Use input was issued.

## Remaining acceptance

Physical swipes, all normal tab actions, final installer cutover/removal, a clean second PC, dark/high-contrast themes, mixed DPI, several monitors and sleep/resume still require acceptance. Later code changes require their own matching evidence; the visual confirmation above must not be represented as covering an untested binary.

Window placement checks do not establish Windows' own Snap state, adjacent-window resizing or Snap Assist integration. Moving the starter/receiver fully into the shell and preserving those deeper native behaviours are subsequent work, not MVP completion claims.

The MVP package is distinct from the earlier full v2 release path. The full release's clean-machine/performance acceptance gate remains unchanged; no untested full-release acceptance fields were marked true.
