# Windows 10 native patch: decisions and evidence

Research date: 25 October 2026. Observed host: Windows 10 Pro 22H2 x64,
19045.7807, .NET Framework release 533509. Scope: Explorer tabs in the real
Explorer and predictable directional window gestures, with an offline installer.

## Existing solutions searched before implementation

| Project | Maintenance / compatibility evidence | Fit |
| --- | --- | --- |
| [indiff QTTabBar](https://github.com/indiff/qttabbar/releases/tag/v1.6.4) | v1.6.4 published 19 October 2026; upstream explicitly documents Win10 classic-toolbar activation; GPL-3.0-or-later | Selected: actual Explorer deskband, source available. Exact-host runtime verification is recorded separately below. |
| [Original QTTabBar](https://qttabbar.wikidot.com/) | Official page lists 1043 stable (2020), 2048 beta2 (2021); no later release verified; linked stable ZIP returned 404 during research | Older freeware distribution with unclear redistribution grant; not selected. |
| [ShellTabs](https://github.com/shefben/shelltabs) | Active native C++ Explorer extension; explicitly Win10; no license grant found in inspected repository | Close technical fit, unsuitable for our redistributable derivative without permission. |
| [Groupy 2](https://www.stardock.com/products/groupy/) | Maintained commercial Win10-compatible software | Tabs group separate windows rather than adding Explorer folder tabs; requires a separate license. |
| [WindowTabs](https://github.com/standard-software/WindowTabs) | MIT; current release ss_2026.10.23; Win10/11 | Groups windows; broader than Explorer tabs. |
| [Windhawk Explorer single window tabs](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/explorer-single-window-tabs.wh.cpp) | Source explicitly requires Windows 11 22H2 or later | Uses an existing Win11 tab engine; cannot add it to Win10. Other searched tab mods likewise did not establish a Win10 engine. |
| [PowerToys FancyZones](https://learn.microsoft.com/en-us/windows/powertoys/fancyzones) | Maintained Microsoft project; Win10 support | Strong custom-zone manager, but does not directly implement our opposite-direction/center and no-minimize gesture contract. |
| [AltSnap](https://github.com/RamonUnch/AltSnap) | Native Win10-supported open source | Rich mouse-led window manipulation; not an exact gesture state-machine solution. |
| [RectangleWin](https://github.com/ahmetb/RectangleWin) | Native Windows hotkey manager, Apache-2.0; inspected commit 90f74bb3be8685c94703403919068742ea576318, 16 November 2025 | Closest small base. Adapt its half geometry/eligibility approach; replace cycling with stable state transitions and add restoration, DPI/frame handling, packaging and tests. |

Search coverage included GitHub repositories/releases/issues, Reddit/forum
results, Windhawk's official catalog and Nexus/extension-store searches. No
suitable Win10 backport of Microsoft's Windows 11 Explorer tab engine was
established. This is a bounded search result, not proof none can ever exist.
The existing imported ExplorerTabs.exe is an independent ExplorerBrowser host
and was excluded because it does not modify the actual explorer.exe window.

## Explorer release identity and installer repair

Pinned QTTabBar source commit: 8e3e4a6d27af1f365b9e2643a73758ec8ca0386d.
Official release asset: QTTabBar.Setup.1.6.4.exe, 2,554,504 bytes.
GitHub-published SHA-256, verified after download:
`4af866b5b13f09fe993a33502515798803ec4a5bd159063389e1f57679904ca6`.
The original installer is unsigned. Its embedded MSI is extracted with official
WiX 3.14.1 dark.exe, without installing or running the bootstrapper.

MSI product code: `{8E3584AF-23BA-4E2E-B490-8B58D7FB180D}`.
Build-Package.ps1 generates a separate transform, leaving upstream MSI unchanged:

* Remove KillExplorer, WaitAfterKill and StartExplorer from InstallExecuteSequence.
* Remove the four Registry-table marker rows at shared HKLM
  `Software\Microsoft\Internet Explorer\Toolbar` whose Name is `*`.
  Preserve QTTabBar's individual GUID values in both registry views.
* Install/uninstall with Restart Manager shutdown disabled and no automatic reboot.

The actual extracted MSI confirms these rows. [Registry marker semantics](https://learn.microsoft.com/en-us/windows/win32/msi/registry-table)
explain why removing only the unrelated RemoveRegistry table would not fix the
uninstall hazard. Existing QTTabBar installs are rejected rather than relying on
the upstream's inconsistent upgrade property names.

Fresh profile: English; only the tab bar enabled; standard tab shortcuts; no
new-window capture, preview popups, optional plugins, background hooks, or
blank-area gestures. `WindowAlpha` is REG_SZ `255`, matching the source's cast
and avoiding the issue described in [upstream #457](https://github.com/indiff/qttabbar/issues/457).
QTTabBar still forces its legacy folder-view layout; it is not a strictly
tabs-only modification, and its toolbar placement differs from Win11's title bar.

## Gestures and geometry

Observed original effective assignments: Left=10, Up=11, Right=12, Down=13,
ThreeFingerSlideEnabled=65535; KeyParams default to zero. These are direct
Windows snap/maximize/minimize actions. Repeated down minimization is an
existing action's behavior, not evidence of a faulty physical touchpad.

The replacement uses Windows' custom shortcut assignments:
Left `0x00250015`, Up `0x00260015`, Right `0x00270015`, Down `0x00280015`;
each action and ThreeFingerSlideEnabled equals 65535.
KeyParams high word is virtual key, low word Ctrl=1, Win=4, Alt=16.
NativeSnap registers the corresponding Ctrl+Alt+Win+arrows through RegisterHotKey.
Plain Win+arrows remain assigned to Windows. Four-finger settings are untouched.

Encoding and checked cache-refresh implementation adapted from MIT-licensed
[AppDock.at365](https://github.com/yukichi-7sai/AppDock.at365/blob/main/dotnet/AppDock.InputHost/TouchpadWindowsCache.cs),
cross-checked against [Ergopti](https://github.com/adrienm7/ergopti/blob/main/static/ergopti_plus/windows/modules/gestures/precision_touchpad_registry.toml).
The cache helper validates PE section bounds, x64 architecture, unique descriptor
IDs in installed twinui.dll, and custom-action descriptors in installed
SettingsHandlers_nt.dll before using the internal settings cache COM interface.
No hardcoded setting-ID range, DLL modification, driver restart or PnP reset is used.
Unsupported builds fall back to the next user sign-in.

Host twinui.dll SHA-256:
`384cf937d902e01c198fdc1ff877a4da30b679413f34621b31cbb392e0de575d`.
REA PE resource inventory Evidence ID:
`ev_4212d362c8d1acf83a7ae59e682159d65f85bf459cc831336a111d50ba806acd`.
That record establishes artifact identity and resource inventory only. The
descriptor parser and COM readback separately establish this host's settings
profile: slide mode ID11; left/action/key IDs14/15, up16/17, right18/19, down20/21.
The read-only probe returned all original actions matching the registry.

Microsoft documents [invisible GetWindowRect resize borders and DPI virtualization](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowrect).
Our geometry uses per-monitor-v2 awareness, DWM visible frame bounds and the
monitor work area; it does not assume taskbar height or zero-origin coordinates.
The original first-swipe offset cause has not been independently reproduced;
border compensation addresses a plausible contributor without claiming diagnosis.

## Validation boundaries

NativeSnap's isolated Windows self-test report is included in evidence/ when
packaging is complete: all 21 checks passed on this host. It operates on dedicated test windows, not the user's
existing apps. Synthetic geometry covers negative and odd-sized coordinates;
only attached real monitors can be tested as physical displays here.
Installer preflight verifies supported OS/.NET, file hashes, absence of an
existing QTTabBar installation and availability of all four hotkeys.
Installation and uninstall logs and registry restoration journal are retained
per user. Physical touchpad gestures still require the user's actual hand input;
automated geometry/cache tests do not prove every touchpad or application.
The six installer-journal/deserializer checks passed in Windows PowerShell 5.1,
including original value types, absence restoration, preserving later user edits,
and atomic backup recovery. Applying the actual MST to a temporary MSI verified
the forced-restart actions were absent, shared-key deletion was removed, and all
four individual toolbar GUID rows were preserved. Native hook DLL imports were
inspected: only Windows libraries, with no separate Visual C++ runtime required.
