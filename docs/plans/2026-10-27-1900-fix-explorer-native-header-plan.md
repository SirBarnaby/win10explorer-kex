---
title: Explorer header integration without overlaps or unwanted extras
date: 2026-10-27
type: fix
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-plan-bootstrap
execution: code
---

# Explorer header integration without overlaps or unwanted extras

## Goal Capsule

**Objective:** Use folder tabs in ordinary Windows Explorer while retaining every existing header control and the familiar Windows appearance.

**Means:** Replace the current caption overlay with an Explorer-owned header layout, following KTD1–KTD3. Remove proven leftovers from this project's earlier installations through U1 and U5.

**Authority:** The user's latest screenshot and native-integration requirements govern this plan. The user explicitly approved expanding the header by one row only when existing controls and tabs cannot share one row. Earlier approval of an MVP does not constitute acceptance of this implementation's appearance or coexistence behaviour.

**Execution boundary:** This document plans the next development phase. It authorizes no claim that the integration already works. Execute the audit and feasibility work first; later implementation and distribution depend on their evidence. Stop if the proposed integration still covers existing controls, requires a replacement file browser, or cannot restore Explorer's original layout.

---

## Product Contract

### Summary

Deliver one restrained addition to real Explorer: folder tabs that look and behave as part of its header. Preserve custom buttons such as the Navigation pane checkbox. Crowded headers expand rather than lose controls. The eventual patch must install, update and remove predictably on supported PCs.

### Problem Frame

The current preview becomes readable but still competes with Explorer for the same title-bar area. `Controller::Layout` in `native/ExplorerTabs.cpp` begins at a fixed 40 logical pixels and sizes the strip from the caption buttons. `ClipRibbon` removes that rectangle from the native Ribbon window's paint region. Neither operation establishes that Explorer's existing controls have been allocated space elsewhere.

This is an in-process overlay in layout terms. Being a child of Explorer does not establish correct header integration. The latest screenshot is evidence that the current result is not accepted. Earlier confirmation that a preview was readable and stable proves only that narrower observation for that earlier binary.

There are also multiple generations of installation state. The v1 documentation describes QTTabBar, a tray helper and unrelated extension behaviours. The MVP adds its own caption strip and startup mechanism. The presence and ownership of each remaining extra must be established before removal; an ordinary Explorer icon or the user's own Quick Access Toolbar button must not be mistaken for an unwanted addition.

### Requirements

| ID | Required outcome |
|---|---|
| R1 | Use the real Explorer window, Ribbon, address bar, folder view and file operations. |
| R2 | Tabs occupy space allocated by the header layout; they never cover, clip, intercept or imitate existing header controls. |
| R3 | Preserve all existing Quick Access Toolbar commands, checkboxes, ordering, states and customisations, including the Navigation pane command. |
| R4 | Use one row when it fits. When it does not, expand the integrated header by one row and keep every original command reachable. Tabs may also use their own overflow menu when there are many tabs. |
| R5 | Match the actual Windows theme, typography, spacing, focus, hover, pressed and inactive states. Preserve caption buttons, drag areas, resizing, system menus and accessibility. |
| R6 | Retire only extras and dependencies demonstrably introduced by this project. Keep independently installed extensions and later user changes. |
| R7 | No tray icon, floating header, replacement browser or unrelated previews, backgrounds, command bars or navigation gestures. |
| R8 | Supply an installable, removable patch with explicit Windows/build compatibility and recovery. No forced Explorer termination or silent modification of Microsoft system binaries. |
| R9 | Preserve the separate snapping requirement: left/right halves and four corners only; gestures never centre a window. Do not expand snapping scope during this header phase. |

**Key decision:** Header integration replaces covering existing caption content (session-settled: user-directed — chosen over the overlay and lower toolbar approaches because the tabs must coexist with the user's actual Explorer controls). Governs R1–R3.

**Key decision:** Expand by one header row only when necessary (session-settled: user-directed — chosen over keeping one row with only a tab overflow menu so the existing controls remain usable). Governs R4.

### Intended Appearance

These are layout sketches, not claims about a completed implementation. Labels stand for the user's actual controls, not replicas.

Normal width:

```text
[Your Explorer buttons] [Downloads ×] [Work ×] [+]      [—][□][×]
[File] [Home] [Share] [View]
[Back] [Forward] [Up] [Address                         ] [Search]
```

Crowded header:

```text
[Downloads ×] [Work ×] [+] [Tabs ▾]                    [—][□][×]
[Your existing Explorer buttons and checkboxes — all retained   ]
[File] [Home] [Share] [View]
[Back] [Forward] [Up] [Address                         ] [Search]
```

The added row belongs to Explorer's managed header and moves the Ribbon/content down as part of layout. It is not a bar below the address field and not a separate window following Explorer. Expanding the header is permission to reflow controls, not permission to rebuild their commands as lookalikes.

### Acceptance Examples

- **AE1:** Add the Navigation pane checkbox to the Quick Access Toolbar, open three tabs, resize the window, and toggle the checkbox. It remains visible, clickable and accurate throughout.
- **AE2:** Fill the header with native commands. The header expands once, no original control disappears, and all tabs remain reachable through the strip or its menu. Widening the window returns to one row without flicker or repeated switching.
- **AE3:** Disable the tabs feature. Explorer restores its original header, exact command state and dimensions without losing folders or resetting preferences.
- **AE4:** On a supported clean PC, install the patch without QTTabBar or a development environment. Only the requested features appear. Remove it and verify restoration.

### Scope Boundaries

This phase covers the header, the tab behaviours needed to use it, attributable legacy cleanup, and packaging. Retain independently configurable snapping, with R9 unchanged.

Deferred: adding tab groups, previews, themes, command bars, alternative file views, broad shell redesigns, Windows 11 support, and redesigning the touchpad transport. Moving every snapping component into Explorer is separate work; this header plan must not claim to have achieved it.

---

## Planning Contract

### Existing Evidence and Prior Art

- The current `Layout`/`ClipRibbon` path explains the collision directly. Existing tab/session/navigation code can be assessed for reuse; its paint placement must be replaced.
- Earlier window-tree reconnaissance shows `NetUIHWND` spanning both the caption and Ribbon. A child-window inventory alone cannot establish the bounds of every windowless Ribbon control. The next investigation must include actual command/layout ownership and accessibility bounds.
- [QTTabBar](https://github.com/indiff/qttabbar/releases) is actively released; v1.6.4 is listed as the latest release, dated 19 October 2026. It integrates with Explorer's toolbar surface and supplies many additional features. It does not establish the requested caption layout and is not the final dependency for this product.
- [Groupy 2](https://www.stardock.com/products/groupy/) is a currently offered commercial product for Windows 10/11 that adds tabs to application frames. It is the closest finished visual comparator, not a redistributable source base for this patch. Its coexistence with this exact customised Explorer is unverified. A historical [Stardock support thread](https://forums.stardock.com/487952/feature-request-group-icon-on-taskbar) documents the same Quick Access Toolbar concern; that is a test requirement, not evidence that current Groupy still has that defect.
- [ShellTabs](https://github.com/shefben/shelltabs) provides C++ Explorer tab/deskband code, but its documented surface is a toolbar. Maintenance and compatibility with this exact build have not been established. It does not solve the header ownership question.
- [Aerexplorer](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/aerexplorer.wh.cpp) demonstrates version-sensitive in-process Explorer modifications and explicitly targets Windows 10 Vibranium builds. It is useful implementation prior art, not a complete title-bar tab solution. [ExplorerTabUtility](https://github.com/w4po/ExplorerTabUtility) enhances Windows 11's existing tab engine and does not provide the missing Windows 10 engine.
- GitHub, Reddit, Stardock forums, Windhawk and a Nexus search were checked. No verified drop-in solution meeting every requirement was found. Community reports were discovery leads; technical decisions above rely on project documentation and source.
- Microsoft's [custom frame guidance](https://learn.microsoft.com/en-us/windows/win32/dwm/customframe) describes frame composition, caption handling and hit testing for application authors. Its [band documentation](https://learn.microsoft.com/en-us/windows/win32/shell/band-objects) describes toolbar/Explorer-bar extension surfaces. These do not establish a supported arbitrary caption-tab insertion point in Windows 10 Explorer. The inference is that private Explorer integration will be needed, with build-specific proof and compatibility limits.

### Key Technical Decisions

**KTD1 — Solve layout ownership before appearance.** Investigate Explorer's frame/Ribbon layout and reserve actual regions for tabs and existing commands. Reading occupied rectangles and drawing in a gap can assist diagnosis but is not the final solution: it cannot guarantee space when the header is full. The native controls must participate in the resulting layout, including row expansion. Implements R1–R4.

**KTD2 — Keep one layout owner inside Explorer.** The target is an in-process integration module with coherent sizing, painting and hit testing. Use documented Windows frame/theme/accessibility services where applicable. Prove activation on newly opened Explorer windows without a resident external header host before claiming final integration. A one-time installer/bootstrap step is distinct from a resident UI follower. The exact supported activation and private layout boundary remain feasibility work in U2, not an established API contract.

**KTD3 — Preserve real commands and let tabs overflow.** Keep the existing command objects and their settings. At insufficient width, reflow the original commands into the added header row while keeping tabs beside the caption buttons. Extra tabs use an accessible native menu rather than displacing commands or dragging space. Use theme text metrics and actual control bounds; do not carry forward the fixed left offset.

**KTD4 — Reuse only independent working components.** Assess the tab model, navigation/history, session serialization, drop handling and accessibility separately from the overlay. Retain suitable code and its licence notices. Avoid replacing a whole tab engine merely to fix frame layout. QTTabBar and broad visual-modification products are not final dependencies.

**KTD5 — Cleanup requires ownership, not resemblance.** Use `native/LegacyMigration.cpp`, the existing installation journals, exact paths/registrations and compare-and-restore semantics. Produce a human-readable inventory of each proposed retirement. Preserve the user's entire Quick Access Toolbar state; do not reset Explorer to defaults or remove all extensions to obtain a clean screenshot.

**KTD6 — Compatibility is demonstrated, not universal.** Start with Windows 10 22H2 x64 build 19045.7807 and relevant Explorer/Ribbon binary versions. Extend support only after matching runtime evidence. Unsupported or conflicting layouts retain ordinary Explorer rather than falling back to an overlay. Do not backport by copying Windows 11 system DLLs. A claim of flawless behaviour across every update or third-party title-bar modification is not justified by this plan.

### High-Level Technical Design

Directional component sketch:

```text
Install / recovery record
        │ establishes supported activation and owned changes
        ▼
Explorer process → Header coordinator
                       ├─ Existing native commands and caption buttons
                       ├─ Tab model + native themed tab surface
                       └─ Header layout / painting / input / accessibility
```

Directional layout states:

```text
Disabled → original Explorer header
Enabled, supported, sufficient room → shared single row
Enabled, supported, insufficient room → integrated two-row header
Unsupported / incompatible → original Explorer header + on-demand explanation
```

Lifecycle: discover and snapshot → prove the integration boundary → attach → relayout on relevant changes → detach and restore. Activation, teardown and configuration changes must be serialized on the owning Explorer thread. Never leave a clipped Ribbon or stale callback after disabling.

### Sequencing and Assumptions

U1 and U2 precede all user-desktop deployment. U3 depends on the U2 proof; U4 depends on U3. U5 packages the verified result, and U6 establishes release evidence. Cleanup of the user's active setup occurs only with its recovery snapshot and replacement readiness established; it is not part of writing this plan.

The specific private frame/Ribbon boundary is an implementation-time unknown requiring inspection and a disposable experiment. If the row reflow cannot preserve actual controls, report that result and revise the architecture; do not silently substitute copied buttons, clipping or a lower toolbar.

**Environment prerequisite:** A disposable matching Windows guest or test machine has not been verified as available; earlier sandbox provisioning was deferred. U1 and read-only research can proceed, but the invasive portion of U2 waits for that environment. Do not enable Windows optional features, reboot, or use the primary desktop as a substitute merely to bypass this prerequisite.

**Current-state caution:** A read-only inspection during this planning turn found an MVP installation directory and enabled feature flags, but no matching helper processes in the process query. That does not establish either successful installation or a crash. U1 must reconcile this state with actual startup entries, registrations and recovery records; historical status documents must not override current evidence.

---

## Implementation Units

### U1. Establish an attributable baseline and recovery inventory

**Goal / Requirements:** Identify what the project added and what the user needs preserved. Covers R3, R6, R8 and R9.

**Files:** `native/LegacyMigration.cpp`, `native/MvpSetup.cpp`, `MVP-STATUS.md`; planned `docs/explorer-header-baseline.md` and `Test-HeaderMigration.ps1`.

**Approach:** Record original toolbar command/state data, folder-view preferences, extension registrations, startup entries and exact owned files. Distinguish v1 QTTabBar, MVP overlays and independently installed extensions. Reconcile interrupted setup state before another deployment. Explain each proposed removal in ordinary terms. Store recovery data outside payload files that uninstall may delete.

**Test scenarios:**

1. A project-installed QTTabBar is marked as attributable; an independently installed copy is preserved.
2. The Navigation pane command, its checked state and toolbar ordering survive migration and rollback.
3. A setting changed after installation is retained rather than replaced by an older backup.
4. An interrupted install, missing helper or loaded legacy DLL is reported accurately without force-closing Explorer.

**Verification:** The inventory accounts for observed extras and can explain every planned change. No preference reset is required to obtain a native baseline.

### U2. Prove actual header ownership and row reflow

**Goal / Requirements:** Establish whether the target can satisfy R1–R5 and R7 before investing in another full implementation.

**Files:** `native/ExplorerTabs.cpp`, `native/ExplorerMvpHost.cpp`, `native/ExplorerTabs.def`; planned `native/HeaderIntegration.cpp`, `native/HeaderIntegration.h`, `Test-HeaderIntegration.ps1` and `docs/explorer-header-feasibility.md`.

**Approach:** Inspect Explorer's actual frame, native command objects and Ribbon layout boundary on the target build. In a disposable matching environment, prove allocated tab space, one extra header row, correct client-area resizing, and complete restoration. Prove activation on new windows without the current resident host. Compare candidate private layout boundaries against these outcomes; do not select one merely because a child strip can be drawn. Reuse existing validated activation/thread-safety code where suitable.

**Execution note:** Start with a deliberately crowded header and an integration experiment, not another visual styling pass. A synthetic geometry test cannot prove this unit.

**Test scenarios:**

1. Covers AE1: Native Navigation pane control remains visible, clickable and correct with a tab strip present.
2. Covers AE2: Header reflows at narrow widths and reverses cleanly when widened; paint and input rectangles never overlap.
3. Maximized, restored, left/right snapped and DPI-changed windows retain working native caption buttons and resize borders.
4. Covers AE3: Detach restores the original native paint/layout state after both successful attachment and a failed feasibility attempt.
5. A new Explorer process/window receives the integration without launching a resident caption-following application.

**Verification:** Save visible before/after evidence, actual command and hit-test evidence, build identifiers and restoration results. Failure blocks U3 onward and produces a concrete feasibility finding.

### U3. Replace the overlay with the proved header layout

**Goal / Requirements:** Implement reliable single/two-row coexistence. Covers R1–R4 and R7 using KTD1–KTD3.

**Files:** `native/ExplorerTabs.cpp`, planned `native/HeaderIntegration.cpp`, `native/HeaderIntegration.h`, `native/HeaderLayoutTests.cpp`; `native/CMakeLists.txt`, `Test-HeaderIntegration.ps1`.

**Approach:** Replace the fixed-offset placement and Ribbon cut-out strategy. Have the proved layout boundary allocate non-overlapping command, tab, drag and caption regions. Recalculate on size, DPI, theme, toolbar customisation and frame-state changes. Reflow actual commands for the extra row, preserve their state, and provide tab overflow. Keep transitions stable near the width threshold.

**Test scenarios:**

1. Empty, short and fully customised command sets produce disjoint usable regions.
2. One, many and long-named tabs remain selectable without covering original commands.
3. Adding/removing a toolbar command while Explorer is open updates the layout correctly.
4. Repeated resizing near the row threshold does not flicker, alternate rows continuously or obscure hit targets.
5. Multiple Explorer windows can use different widths and layouts independently.

**Verification:** Layout checks and the real customised Explorer agree. No native control is hidden with a paint-region cut-out to manufacture room.

### U4. Match Windows appearance and complete interaction behaviour

**Goal / Requirements:** Make the integrated result usable and visually consistent. Covers R3, R5, R7 and R9.

**Files:** `native/ExplorerTabs.cpp`, `native/Options.cpp`, `native/Options.rc`; `native/ModelTests.cpp`, `Test-HeaderIntegration.ps1`; planned `docs/header-acceptance.md`.

**Approach:** Use Windows theme/colour/font metrics for all states. Preserve caption/system-menu behaviour and a usable drag surface. Verify tab keyboard routes, focus, accessibility and file-drop delegation. Remove visual workarounds that exist only to conceal the old glass/clipping problem. Keep settings on demand and avoid introducing any unsolicited feature.

**Test scenarios:**

1. Light, dark, high contrast, active and inactive windows remain readable at 100%, 125%, 150% and 200% scaling.
2. Clicking each native caption/toolbar control invokes only its intended action. Dragging, double-click maximise, right-click system menu and resize edges behave normally.
3. Tab creation, closing, switching and reopening work with mouse and keyboard; a screen reader exposes names and selection.
4. Navigation, rename, search, copy/paste, drag/drop and file operations remain Explorer operations across tab switches.
5. Existing edge/corner snapping remains unchanged, including repeated directions and no-centre behaviour.

**Verification:** User-visible inspection and actual input tests pass on the same binary. Rendering tests alone are insufficient.

### U5. Retire owned legacy additions and package the supported integration

**Goal / Requirements:** Deliver a clean installation and predictable recovery. Covers R6–R9 and KTD5–KTD6.

**Files:** `native/MvpSetup.cpp`, `native/MvpSetup.rc`, `native/LegacyMigration.cpp`, `native/PatchRuntime.cpp`, `Build-Mvp.ps1`, `MVP-README.md`; `Test-HeaderMigration.ps1`, `Test-Mvp.ps1`.

**Approach:** Package the verified module, on-demand options and necessary activation only. Retire owned QTTabBar registrations and extras after preserving user state and establishing replacement readiness. Remove the superseded resident tab host from the final path if U2 proved its replacement. Apply explicit per-build compatibility checks. Keep exact rollback, ownership checks and honest in-use removal status; do not leave accidental startup mechanisms behind.

**Test scenarios:**

1. Covers AE4: Clean installation works with no QTTabBar, compiler, Windhawk or trial program installed.
2. Upgrade from the owned v1/MVP setup removes attributable extras and preserves the customised toolbar and original gesture recovery record.
3. Failed installation restores the exact prior effective state, not merely registry values that look similar.
4. Disabling/removing restores the original header, removes only owned startup entries and reports locked files accurately.
5. An independently installed extension and an unsupported Windows build remain unchanged.

**Verification:** Clean install, upgrade, disable, re-enable and removal have corresponding runtime evidence. No claim of complete removal is made while registered or loaded remnants remain.

### U6. Establish release evidence on the real configuration and another PC

**Goal / Requirements:** Verify the promised product rather than the presence of a tab control. Covers R1–R9 and AE1–AE4.

**Files:** `Test-HeaderIntegration.ps1`, `Test-HeaderMigration.ps1`, `Test-Mvp.ps1`, `docs/header-acceptance.md`, `MVP-STATUS.md`, `MVP-README.md`.

**Approach:** Test the user's actual customised header and a clean matching Windows installation, followed by a second supported PC. Bind evidence to the delivered binary and system versions. Include resize/theme/DPI transitions, additional monitors, sleep/resume and sign-in. Recheck compatibility after relevant shell updates. Keep a short user-facing list of what changed and how to undo it.

**Test scenarios:**

1. All acceptance examples pass with customised and empty toolbars.
2. Sign-in, opening another Explorer process and sleep/resume do not produce missing or duplicate headers.
3. The clean second system has no dependency on the developer's registry, tools or previous trial installations.
4. A later unsupported shell version retains usable ordinary Explorer and explains compatibility on demand.

**Verification:** The exact distributed build passes both automated and visible/runtime acceptance. Untested conditions remain explicit support limits.

---

## Verification Contract

Keep the existing native model and six-destination snap checks as regression evidence. They do not prove caption integration, physical gestures, native appearance or clean installation.

Add focused layout and ownership/migration coverage, then use the real Explorer acceptance scenarios above. Establish baseline behaviour before changing the header. Never move user windows or apply desktop experiments merely to make a unit test pass.

Record the application/OS build, DLL hash, toolbar configuration, DPI/theme, input action and expected/actual outcome with every integration finding. A visible screenshot must show the original controls as well as tabs; verify their real clicks and command state separately. Review production changes before distribution. The earlier multi-agent code review did not complete and must not be cited as a release approval.

### Risks and Stop Conditions

- **Private Explorer layout:** U2 is a hard feasibility gate. If no safe boundary is found, report the limit instead of shipping another overlay.
- **Shared Ribbon painting:** Do not infer individual command space from parent HWND bounds. Prove windowless command coexistence and input routing.
- **Legacy state ownership:** Do not uninstall or reset uncertain third-party state. Reconcile it in U1 before migration.
- **Explorer stability:** Use a disposable matching environment for the first invasive frame experiment; preserve active file operations and rollback data. No automatic process killing.
- **Finite width:** The approved second row and tab overflow handle crowded headers; unlimited controls cannot be simultaneously visible in a fixed-size single row.
- **Cross-PC/update behaviour:** Support is bounded by actual build evidence. Passing on this PC does not establish compatibility with arbitrary Windows versions or shell mods.

---

## Definition of Done

The work is complete only when the tabs share an allocated Explorer header, a crowded customised toolbar remains usable through the approved extra row, the native appearance and inputs pass inspection, proven legacy extras are retired, and the delivered patch passes installation/removal on supported clean and existing setups.

The current overlay, a good-looking screenshot alone, or successful model tests do not satisfy this definition. No production code or further installation is part of this planning turn.
