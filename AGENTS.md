# Win10NativePatch

Explorer MVP: native title-bar tabs for Windows 10 Explorer and touchpad snapping. Current state and history: `MVP-STATUS.md`. Building: `docs/building.md`.

## Read before changing code here

- `docs/solutions/2026-11-12-tab-strip-refinement-learnings.md`: build hangs, frozen binaries after testing, the Sandbox component gate after Windows updates, setup tamper testing.

## Rules that stay in force

- Do not change `HeaderIntegration.cpp` mechanics, hash pins, offsets or version guards.
- Runtime tests run only in Windows Sandbox using the lab under `evidence/header-rebuild-20261104/renderer-production/`. Never restart or kill Explorer, or drive input, on the host.
- Keep the user's settings, sessions, Ribbon/QAT preferences and touchpad bindings exactly as they are.
