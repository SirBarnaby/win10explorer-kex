# Windows 10 Native Patch v2

**Development build. The native Explorer replacement has not passed its deployment gate.** The development bundle deliberately contains no MSI and does not register an Explorer extension. Running Options opens a native Windows property sheet; do not enable development features on a working desktop as a substitute for the isolated acceptance run.

The target is Windows 10 22H2 x64, build 19045. This is a C++20 Win32/COM implementation. Deployed binaries use the static C++ runtime and do not require .NET, Qt, Electron or a WebView.

## Components

- `ExplorerTabs.dll`: COM extension in the real Explorer process, with a child in the title bar and the existing Explorer shell view, ribbon, address bar and file operations. Tabs carry PIDLs, histories, selections, view state and a visible scroll anchor. Scroll restoration uses Explorer's public item-anchor API; exact pixel restoration is not guaranteed.
- `NativeSnap.exe`: independent native tray process, started only for effective snap input routes. Left/right chooses a half; from a side, up/down chooses its top/bottom quarter. Repeated down keeps the bottom corner visible. The opposite horizontal direction centers a smaller window. Up from floating maximizes; down restores/centers and never minimizes. Touchpad gestures use private Ctrl+Alt+Win+F13–F16 bindings. Optional keyboard bindings are independent. Standard Win+Arrow behavior is preserved.
- `Win10PatchOptions.exe`: native property sheet with Features, Explorer, Snapping and Diagnostics pages. Feature switches apply independently; Cancel does not apply draft changes.
- `NativePatchSetup.exe`: native frontend for a component-based Windows Installer package. It also provides `--emergency-disable`. The MSI is produced only after acceptance evidence passes the release script.

In an accepted installation, Options is always available under **Start → Windows 10 Native Patch → Options**, including when all features are off. Explorer's tab menu and the Snap tray menu also open Options. The Options process exits when its dialog closes.

## Build and test

Install Visual Studio 2022 Build Tools with the C++ workload and a Windows SDK, plus CMake 3.24 or newer. The build tools themselves may use .NET; the patch binaries do not.

```powershell
.\Build-Native.ps1
.\Test-Native.ps1
.\Build-NativeBundle.ps1
```

The isolated settings round-trip test creates and removes only `HKCU\Software\Win10NativePatch\IsolatedTest-<PID>`. A restricted execution token can deny that write; run the test in a normal user process. Geometry tests create their own disposable window. The gesture probe reads the native Windows cache without changing gesture assignments. Explorer model tests load the DLL in a test process and never register or attach it to Explorer.

The ordinary development bundle contains binaries, corresponding source, license notices and SHA-256 checksums. It does **not** install components. Do not use `regsvr32` on the working PC to bypass the deployment gate.

## Acceptance and release

See `native/acceptance.template.json` and `NATIVE-STATUS.md`. Record exact tested cumulative-update builds and the SHA-256 of the final Explorer DLL. Attach appearance, reliability, setup and matched performance evidence with checksums. Release generation refuses any missing or failed gate, mismatched DLL, missing measurement, or changed evidence file.

```powershell
.\Build-NativeBundle.ps1 -Release -AcceptanceReport .\evidence\acceptance\acceptance.json
```

The release build additionally needs WiX 3.14.1 (`tools/wix314`). WiX is a build tool, not a deployed runtime dependency. Keep the complete offline release folder for future repair. Windows Installer may request the original source folder if its cache cannot supply a file.

The installer exposes separate Explorer and Snap features. Options, recovery and corresponding source remain installed with either or both features absent. Protected binaries live in `%ProgramFiles%\Win10 Native Patch\2.0.0`; settings and ownership journals are per user. Native feature availability is read from machine registration; requested settings never imply successful activation.

## Migration and recovery

Do not run the old combined `Uninstall.ps1` to migrate this PC. Native migration reads the v1 CLIXML as bounded XML data, backs it up without overwriting an earlier recovery copy, and imports original gesture values only where current values still match v1 ownership. Changes made later in Windows are retained. The native gesture journal is flushed before Windows is modified; readback and rollback are explicit.

The old snap receiver/startup entry is retained until native input or gesture restoration is verified. Cleanup checks the exact old executable and startup value. QTTabBar retirement occurs only in an accepted release, only for the product owned by the v1 journal, and only if the effective cached MSI excludes Explorer restart actions and shared-toolbar-key deletion. It never calls the old combined uninstaller. Folder windows may require reopening or sign-in to unload an old DLL. Setup never force-kills Explorer.

Emergency disable requests all features off, restores owned gestures, and keeps the receiver available while Windows still sends its private bindings. A pending state means sign out and sign back in before removing Snap. Journals are preserved on errors. If Explorer repeatedly fails during early extension startup, its circuit breaker stops further attachment; inspect the ExplorerTabs journal under `HKCU\Software\Win10NativePatch\v2` before explicitly resetting it.

Machine-wide component removal affects every user's installed files. Each configured user must disable their gesture routes before machine-wide removal. Multi-user removal, interrupted setup and actual UAC cancellation remain release acceptance checks; they are not established by the source/model tests.

## Licenses

The v2 distribution is GPL version 3 or later; the full text is in `LICENSE.txt`. Preserved StExBar sources retain their original GPL-2.0-or-later notices. Snap geometry derives from RectangleWin (Apache-2.0), with PowerToys MIT ancestry; gesture cache research comes from AppDock (MIT). Corresponding source and all upstream notices are included. QTTabBar runtime and ShellTabs source are excluded. No Microsoft binaries are redistributed.
