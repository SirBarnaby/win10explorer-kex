# Explorer MVP 0.1

Windows 10 22H2 x64 (build 19045).

Run **ExplorerMvpSetup.exe**, choose title-bar tabs and/or touchpad snapping, and select **Install / Update**. Setup installs for your Windows account. It does not replace Windows system files or require administrator access. Open **Explorer MVP → Options** from Start, or right-click a title-bar tab and select **Options**.

## Included

- Folder tabs in the real Explorer window. They share the caption when space permits; narrow windows get an allocated row above the address bar. The original Quick Access Toolbar commands stay usable. Ctrl+T creates a tab, Ctrl+W closes one, Ctrl+Shift+T reopens one, and Ctrl+Tab switches tabs.
- Three-finger Left/Right selects that screen half. Up/Down chooses the top/bottom corner of the selected side. Up again from either top corner maximises; Down returns to that corner. Down again at a bottom corner keeps it visible. Up/Down on an unsnapped floating window leaves it unchanged. These gestures never centre or minimise a window.
- Native **Explorer Options** with independent switches for tabs, touchpad snapping and optional keyboard shortcuts, plus session restoration and diagnostics.
- Windows' shared resize divider works between complementary snapped windows. Its chosen horizontal split is remembered per display and reused by subsequent side and corner snaps, including after the receiver restarts.
- No notification-area icon. Windows+Arrow retains its Windows behaviour.

On this project's earlier QTTabBar installation, the new tabs replace the lower tab bar while active. QTTabBar files remain available for recovery. Independently installed extensions are left alone.

## What this MVP is

The tabs are child controls of Explorer's window. A hidden starter attaches the module to supported Explorer folder threads. Extra row space is reserved through the native Ribbon height notification; native commands are never cut out. A hidden receiver invokes Windows Snap and preserves its arranged state while adjusting frame geometry. Options and Setup run only when opened. This is an Explorer extension, not a Microsoft tab backport.

Install only on the stated Windows version. Header integration additionally checks the exact supported UIRibbon.dll image (SHA256 941DCFCD96B3D9799945A5AB1B4D4B7D6859682A0AF9DF38223568D7F3C46A2F); a different image leaves ordinary Explorer chrome intact. This PC's build is 19045.7807. Clean-PC, mixed-DPI and multiple-monitor hardware acceptance has not been completed. Touchpad assignments may require signing out and back in if Windows cannot refresh its gesture cache immediately; Options reports the pending state. Physical gestures still require a manual test.

## Quick check

1. Open Explorer. Widen and narrow it: tabs share the caption when they fit and move into an extra allocated row when needed. Check the real Quick Access Toolbar and address bar, then open two folders in separate tabs and switch between them.
2. Right-click a top tab → Options. Turn each feature off and on independently; Cancel must discard unapplied changes.
3. On an ordinary resizable window, swipe Left → Down → Down. It should stay visible in the bottom-left quarter. Right should select the right half, Up should choose the top-right quarter, and another Up should maximise. Down should return to the top-right quarter.
4. Check that no new tray icon appears. Test dragging, maximising and restoring Explorer and using its usual file operations.
5. Snap two windows to opposite sides and drag Windows' middle divider. Snap another window into a corner and verify that it reuses the resulting horizontal boundary.

## Disable and remove

Use **Explorer MVP → Change or remove** or Windows Apps & Features. Setup first restores owned touchpad assignments and disables tabs. If gesture restoration is pending, sign out/in before removal. An Explorer module can stay loaded until its folder process exits; Setup reports incomplete removal rather than forcing Explorer to close or claiming files were removed. Close Explorer folder windows and retry removal when requested.

Source and licence notices are included in Source.zip. The derived distribution is GPL-3.0-or-later, with the original upstream notices retained.
