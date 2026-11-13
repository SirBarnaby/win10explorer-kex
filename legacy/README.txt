WINDOWS 10 EXPLORER TABS AND RELIABLE SNAP 1.0
===========================================

INSTALL ON THIS PC OR ANOTHER PC
1. Copy/extract the entire package folder to the other Windows 10 PC.
2. Double-click Install.cmd from your normal Windows account.
3. Approve Windows' administrator prompt for the Explorer extension.
   The snap helper and gesture settings belong to the signed-in user.

Supported: Windows 10 x64 version 2004 through 22H2, .NET Framework 4.8.
No Internet connection, compiler, PowerToys, AutoHotkey, or Windhawk is needed.
Existing QTTabBar installations are detected and left for their owner to manage.
Close any competing manager using Ctrl+Alt+Win+arrow before installing.

EXPLORER
Tabs live inside the actual explorer.exe window, using QTTabBar 1.6.4.
The bar uses Explorer's toolbar area; its appearance differs from Windows 11's
title-bar tabs. This is a native Explorer extension, not a Win11 binary backport.
The existing ribbon, file operations, shell menus, address bar, and Win+E remain
Explorer's. QTTabBar also selects Explorer's legacy folder-view layout internally.

Ctrl+T              New tab
Ctrl+W              Close tab
Ctrl+Shift+T        Reopen closed tab
Ctrl+Tab           Next tab
Ctrl+Shift+Tab     Previous tab
Ctrl+1 through 9   Select a tab
Ctrl+N / Win+E     Open a separate real Explorer window
Window X           Close the window and its tabs

If the bar does not appear: Explorer > View > Options dropdown > QTTabBar.
Tab options are available from the tab bar's right-click menu. Previews, extra
button bars, plugins, background effects, blank-area gestures, and window capture
are disabled by this package. Light/dark coloring follows Windows.

WINDOW SNAPPING
Three-finger touchpad swipes use Windows' Precision Touchpad shortcut settings.
Keyboard equivalent: hold Ctrl+Alt+Win and press an arrow.

Left               Snap to the left half; repeating left stays there.
Right              Snap to the right half; repeating right stays there.
Opposite direction Restore a smaller centered window first; repeat to snap across.
Up                 Maximize; repeating up stays maximized.
Down               Restore a smaller centered window; repeating down stays there.

Down never minimizes. To minimize, use the window's normal minimize button.
The centered size uses the previous normal window size; oversized or unknown
restore sizes use 70% of the work area. External snaps use a sensible
smaller default. Apps with a minimum size larger than a half keep their minimum
size rather than being clipped. Visible borders align with the actual monitor
work area, accounting for the taskbar and invisible Windows resize borders.

The small native helper appears in the notification area and starts at sign-in.
Its menu has Pause/resume, Help, and Exit. Reopen it from the Start menu entry
"Native Snap (Win10 Patch)" after exiting. No keyboard hooks or system DLL edits
are used. Existing plain Win+arrow shortcuts retain the Windows behavior.
Dialogs, desktop surfaces, and borderless fullscreen windows are excluded.
Apps running as administrator may refuse changes from the normal-user helper.

On this Windows build live gesture cache refresh is supported. On another build,
the installer validates the installed settings layout first. If it cannot reload
the cache safely, it asks you to sign out and sign back in. Four-finger gestures,
touchpad sensitivity, taps, and scrolling are preserved.
PCs without a Precision Touchpad can use the keyboard shortcuts.

UNINSTALL / RECOVERY
Use Settings > Apps > Windows 10 Explorer Tabs and Reliable Snap > Uninstall,
or run Uninstall.cmd. Approve the Explorer extension's administrator prompt.
Registry values are backed up with original types and absence state. Uninstall
restores settings that still match what this package wrote, preserving later
changes. The recovery journal and logs remain in:
  %LOCALAPPDATA%\Win10NativePatch
Installed helper files live in:
  %LOCALAPPDATA%\Programs\Win10NativePatch
Explorer finishes unloading the extension at the next sign-in. Save any active
file operations before signing out. The installer never force-kills Explorer.

The bundled MSI transform removes upstream's forced Explorer restart and its
uninstall rule that would delete the shared toolbar registry key. Install only
through Install.cmd so that transform and the safe defaults are applied.

VERIFICATION AND SOURCE
SHA256SUMS.txt verifies package contents for accidental corruption. It is not a
digital signature. Locally built NativeSnap and TouchpadCache are unsigned.
The sources folder contains corresponding QTTabBar source, helper source,
attribution, and build information. Licenses are included in licenses/.
Automated geometry/cache checks do not establish that a physical swipe has been
tested on every touchpad, app, DPI scale, or monitor arrangement.
