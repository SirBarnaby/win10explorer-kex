# Native v2 implementation status — 2026-10-26

**Not released; not installed.** The existing QTTabBar and v1 snap installation remain the active configuration on this PC. This source implementation must not be described as a validated Windows 11 tab backport. It implements new title-bar integration against the Windows 10 shell.

Sandbox provisioning is deferred at the user's request. No Windows optional feature was enabled and no restart was requested. `SANDBOX-REQUEST.md` records the later environment and acceptance work; `MANUAL-TESTING.md` describes current native Options and snap tests that preserve the existing installation.

## Implemented source

- C++ COM Explorer extension, distinct class identifier, owning-STA checks, measured caption integration, reversible subclasses/regions, native theme rendering, tab/history/view-state model, session serialization, keyboard and mouse routes, OLE tab transfer/detach, shell file-drop delegation and MSAA accessibility.
- Native snap geometry, independent private gesture and optional keyboard routes, native tray Options, event-based settings reload, conflict rollback, demand-based startup and exit.
- Native cache codec with descriptor validation and readback, durable original/owned/restore journals, compare-and-restore semantics, legacy baseline import and exact-before rollback.
- Native four-page Options application and native setup frontend; component-based MSI authoring; source/checksum bundle builder with mandatory evidence gate.
- Bounded CLIXML migration parser with DTD prohibition, size/depth/node/text limits and ownership checks.

Source implementation is not runtime acceptance. See the evidence files for the exact tests that ran. In particular, model tests for flag combinations do not establish successful gesture changes, sign-in behavior, installer recovery, or native appearance.

## Verification completed

- MSVC x64 Release build succeeds with no reported compiler/resource warnings.
- 38/38 isolated snap checks, including all four corners, repeated Down, standard/custom frames, external corner detection, opposite-horizontal centering and minimum-size bottom/right anchoring. No user window is moved.
- 34/34 configuration and gesture restore model checks, including all 16 combinations of the three input/features switches and Restore Tabs. The atomic settings round-trip uses a disposable registry key.
- 8/8 migration parser checks, including later-user-change preservation and read-only parsing of this PC's actual v1 journal.
- 12/12 Explorer model/metadata checks, including bounded packet parsing, a real background metadata STA and cancellation of a message-only test target. Explorer itself is untouched.
- 5/5 release rejection checks: missing evidence, untested title bar, untested DLL, excessive memory measurement, and missing runtime evidence all block package production.
- Read-only COM cache probe succeeds on this PC and confirms that the existing v1 gesture route remains active. Native v2 private bindings are not active.
- PE import inspection finds Windows DLL dependencies only; no managed, Qt, Electron, WebView or separate VC runtime dependency is deployed.

## Current blockers and unverified requirements

1. **Isolated Explorer feasibility proof is outstanding.** This host has no configured Windows Sandbox, Hyper-V, VMware or VirtualBox guest, and `WindowsSandbox.exe` is absent. A hypervisor is present; CPU virtualization capability flags are therefore inconclusive. Online optional-feature inspection requires a Windows administrator token. No clean Windows 10 22H2 image is available. The extension has deliberately not been registered in the working desktop. Native caption paint ownership, BHO activation on this Explorer build, drag space, native caption buttons and ribbon coexistence require that experiment.
2. **Actual GUI inspection failed.** The Windows computer-use helper found the Options window but returned `FrameArrived timed out` and then `window capture timed out` on the recovery attempt. Native dialog layout, accessibility and keyboard traversal are therefore not visually certified.
3. Physical touchpad swipes, additional monitors, mixed DPI, sleep/resume and sign-in cycles are outstanding. Read-only cache inspection confirms the original v1 arrow bindings, not a native-v2 cutover.
4. No light/dark/high-contrast screenshot matrix or 100–200% scaling matrix has passed. Search, virtual/network/cloud folders and ordinary file operations require actual Explorer integration.
5. The 20 MiB / 50 ms p95 / 5% throughput targets and 1,000-operation resource test have not been measured. No performance target is a claimed result.
6. Install, cancelled elevation, component change, repair, upgrade, uninstall, interrupted setup and QT-only retirement must be tested on both this PC and a clean guest. Multi-user removal is not validated.

The release builder requires every acceptance Boolean, exact tested build identifiers, matching DLL hash, numerical performance results and checksum-bound evidence before producing an MSI. The default bundle is explicitly marked DEVELOPMENT and contains no installer payload.

## Continue the feasibility gate

Use a disposable, clean Windows 10 22H2 x64 guest with QTTabBar absent. Copy the development source/binaries and establish a checkpoint before registration. The extension's `RunProof` export additionally requires an explicit isolated-proof window marker and an owning-STA invocation; invoking it from an arbitrary external process is refused. The current proof checks only same-folder tab creation, shell connection, bounded state round-trip and feature-toggle preservation. It does not certify appearance or the release matrix.

Inspect actual Explorer windows in normal, maximized and snapped layouts. Exercise caption controls, drag, resize, ribbon, DPI changes and disable/enable before accepting the title-bar route. If the native caption cannot be made reliable, leave deployment blocked and record the failing window/layout evidence. Do not replace it with a toolbar or an external browser.

After feasibility passes, run the complete `native/acceptance.template.json` checklist, attach evidence and populate the measured fields. Rebuild after any repair; tests and acceptance must match the exact resulting DLL. Keep `status` descriptive and never turn untested fields true merely to generate an installer.
