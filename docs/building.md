# Building, testing and packaging

For people working on Explorer MVP. Users only need the setup from the releases page.

## Tools

- Windows 10 22H2 x64.
- Visual Studio 2022 C++ Build Tools with a Windows 10 SDK.
- CMake 3.24 or later.
- Windows PowerShell 5.1.

MinHook is vendored in `native/vendor/minhook`; nothing is downloaded during the build.

## Build

```powershell
.\Build-Native.ps1 -BuildDirectory .\build-mvp
```

This configures `native/` with the Visual Studio 2022 generator and builds Release into `build-mvp\Release`.

If a build stops making progress without an error, look for build processes left suspended by an earlier build (`MSBuild.exe`, `mspdbsrv.exe`, `vctip.exe`, `Tracker.exe`); new builds reuse them and wait. Signing out clears them. Setting `MSBUILDDISABLENODEREUSE=1` before building avoids the reuse.

## Test

```powershell
.\Test-Mvp.ps1 -BinaryDirectory .\build-mvp\Release
```

This runs the model and self-tests of each binary on the build PC: snapping, snap layout, Options and settings, the Explorer tab model, settings migration and the setup self-test. It also checks that a setup with one changed byte is refused, and that setup and the add-on agree on the supported `UIRibbon.dll` and `uxtheme.dll`.

These tests do not attach to Explorer. Anything that loads the add-on into Explorer, restarts Explorer or drives the pointer and keyboard is run in Windows Sandbox, never on the working desktop. `sandbox/` holds the guest scripts.

## Package

```powershell
.\Build-Mvp.ps1 -StageSetup     # single-file setup from the current binaries, for testing
.\Build-Mvp.ps1 -SkipBuild      # the release package, from binaries that were tested
```

`ExplorerMvpSetup.exe` embeds the four program files, `Source.zip`, the licence and `MVP-README.md` (as `README.txt`), with a SHA-256 table it verifies before installing.

Release builds are not reproducible byte for byte: every link embeds a timestamp. Build once, test those exact files, then package with `-SkipBuild`. Packaging refuses binaries that do not match the acceptance record written by `New-HeaderAcceptance.ps1`.

## Supported system files

Title-bar integration is pinned to exact images, checked by hash at run time:

| File | SHA-256 |
|---|---|
| `UIRibbon.dll` | `941DCFCD96B3D9799945A5AB1B4D4B7D6859682A0AF9DF38223568D7F3C46A2F` |
| `uxtheme.dll` | `85E26787282D70D203026E393242A8616CFD832D4E68DB466151301B6063AF4E` |

Do not change the pins, offsets or version checks in `native/HeaderIntegration.cpp` without testing against the new files. After a Windows update, check these two files first.

## How the pieces fit

- `ExplorerTabs.dll`: the tab strip and its model, loaded into Explorer. The strip is a child window of the Explorer frame. Extra row space is reserved through the Ribbon's own height notification, so native commands are never cut off.
- `ExplorerMvpHost.exe`: the hidden starter. It wakes only when a top-level window opens or closes, and asks new folder windows to take tabs.
- `NativeSnap.exe`: the hidden snap receiver. It asks Windows to snap the window and keeps Windows' arranged state while adjusting the frame.
- `Win10PatchOptions.exe`: the Options window. It runs only while open.
- `ExplorerMvpSetup.exe`: per-user setup, update and removal.

## Manual check after installing

1. Open Explorer. Widen and narrow it: tabs share the title bar when they fit and move into an extra row when they do not. Check the Quick Access Toolbar and the address bar, then open two folders in separate tabs and switch between them.
2. Open twenty tabs with Ctrl+T: the strip scrolls and keeps the selected tab visible. Drag a tab along the strip to reorder it.
3. Right-click a tab → Options. Turn each feature off and on; Cancel must discard changes that were not applied.
4. On an ordinary resizable window, swipe Left → Down → Down. It should stay visible in the bottom-left quarter. Right selects the right half, Up the top-right quarter, another Up maximizes, Down returns to the top-right quarter.
5. Check that no tray icon appears. Drag, maximize and restore Explorer and use its usual file operations.
6. Snap two windows to opposite sides and drag Windows' middle divider. Snap another window into a corner and check that it reuses the resulting boundary.

## More

- [CHANGELOG.md](../CHANGELOG.md): changes and known limits per release.
- `legacy/`: version 1.0, based on QTTabBar.
