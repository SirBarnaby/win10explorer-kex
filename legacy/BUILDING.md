# Rebuilding the helpers and bundle

The package includes complete helper sources and the unmodified QTTabBar source
for the pinned release. NativeSnap's code changes are Apache-2.0, its original
icon artwork is supplied with those sources, and TouchpadCache retains the
AppDock MIT license. Keep the licenses and exact upstream reference snapshots.

Build on Windows 10 x64 with .NET Framework 4.8 and a MinGW-w64 C++17 compiler:

```powershell
.\Build-NativeSnap.ps1 -Compiler 'C:\mingw64\bin\g++.exe'
.\Build-TouchpadCache.ps1
.\bin\NativeSnap.exe --self-test "$pwd\evidence\native-snap-self-test.json"
.\bin\TouchpadCache.exe --probe
.\Test-Installer.ps1
```

The native build is static and imports only Windows/UCRT DLLs. Inspect imports
with `objdump -p bin\NativeSnap.exe`. The embedded manifest is per-monitor-v2
aware and runs at the caller's integrity level. The icon generator uses Python's
standard library: `python tools\Generate-NativeSnapIcon.py`.

For package construction, use the repository layout of this workspace: helper
sources in `src`, binaries in `bin`, evidence in `evidence`, upstream files in
`vendor`, and WiX 3.14.1 binaries in `tools\wix314`. Retrieve the pinned QTTabBar
v1.6.4 release described in Research.md, verify its published SHA-256, then use
WiX `dark.exe -x` first on the bootstrapper and then on its embedded MSI. These
are extraction operations; do not run the bootstrapper. The package ships that
unmodified MSI alongside a separate, reproducible transform.

`Build-Package.ps1` builds the transform through Windows Installer's database
API, copies source and licenses, and creates SHA256SUMS.txt. It requires the
checked-out matching upstream QTTabBar source for `git archive`; alternatively
use the included `QTTabBar-v1.6.4-source.zip` as its corresponding source.
The transform removes the three forced-restart schedule entries and four shared
toolbar-key deletion markers documented in Research.md. Do not alter unrelated
MSI rows. Validate its application to a temporary copy of the original MSI.

Run `package\Install.ps1 -PreflightOnly` with system Windows PowerShell 5.1.
This validates all package hashes and requirements without installing. For an
installation test, use Install.cmd from a normal user account and approve the
Windows administrator prompt. An existing QTTabBar installation is a deliberate
preflight stop. Test uninstall/reinstall in a disposable Windows 10 VM when
extending the installer, including UAC cancellation and later user setting
changes. Do not use automated geometry checks as a claim of physical gesture,
mixed-DPI, or all-application coverage.
