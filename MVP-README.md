# Explorer MVP 0.1

Windows 10 22H2 x64 (build 19045).

Run **ExplorerMvpSetup.exe**. It is one self-contained file: copy it to any PC and double-click it. Setup first says what that PC gets (tabs in the title bar, tabs in a row under the ribbon, or not supported on this Windows build), then you choose title-bar tabs and/or touchpad snapping and select **Install / Update**. Setup installs for your Windows account. It does not replace Windows system files or require administrator access. Open **Explorer MVP → Options** from Start, or right-click a title-bar tab and select **Options**.

For a scripted install run `ExplorerMvpSetup.exe --install` (or `--quiet`), optionally with `--tabs`/`--no-tabs` and `--gestures`/`--no-gestures`. `--uninstall` removes it.

The setup file is not code-signed. Windows SmartScreen may show "Windows protected your PC" the first time; select **More info → Run anyway** only for a file you built or obtained from this project. Setup checks every embedded file against its SHA-256 before installing and stops without changing anything if the file is damaged.

## Included

- Folder tabs in the real Explorer window. They share the caption when space permits; narrow windows get an allocated row above the address bar. The original Quick Access Toolbar commands stay usable. Tab text in the title bar uses the window's own title colour, including dark and accent-coloured title bars and DWMBlurGlass title colours.
- Many tabs: tabs shrink to a readable minimum, then the strip scrolls. Use the mouse wheel over the strip or the scroll arrows; the selected tab is always kept in view.
- Tabs slide when they open, close or move, on the compositor, so they stay smooth while Explorer loads a folder. Turn this off with Options → Tabs → **Animate tabs**; Windows' "Show animations in Windows" also turns it off.
- Shortcuts: Ctrl+T new tab, Ctrl+W close, Ctrl+Shift+T reopen, Ctrl+Tab / Ctrl+Shift+Tab or Ctrl+PgDn / Ctrl+PgUp switch, Ctrl+Shift+PgDn / PgUp move a tab, Ctrl+1 to 8 select a tab and Ctrl+9 the last. Drag a tab along the strip to reorder it; drag it away to move it to another Explorer window.
- Three-finger Left/Right selects that screen half. Up/Down chooses the top/bottom corner of the selected side. Up again from either top corner maximises; Down returns to that corner. Down again at a bottom corner keeps it visible. Up/Down on an unsnapped floating window leaves it unchanged. These gestures never centre or minimise a window.
- Native **Explorer Options** with independent switches for tabs, touchpad snapping and optional keyboard shortcuts, plus session restoration and diagnostics.
- Windows' shared resize divider works between complementary snapped windows. Its chosen horizontal split is remembered per display and reused by subsequent side and corner snaps, including after the receiver restarts.
- No notification-area icon. Windows+Arrow retains its Windows behaviour.

On this project's earlier QTTabBar installation, the new tabs replace the lower tab bar while active. QTTabBar files remain available for recovery. Independently installed extensions are left alone.

## What this MVP is

The tabs are child controls of Explorer's window. A hidden starter attaches the module to Explorer folder windows; it is woken only when a top-level window opens or closes, and is otherwise idle. Extra row space is reserved through the native Ribbon height notification; native commands are never cut out. A hidden receiver invokes Windows Snap and preserves its arranged state while adjusting frame geometry. Options and Setup run only when opened. This is an Explorer extension, not a Microsoft tab backport.

Install only on the stated Windows version. Header integration additionally checks the exact supported UIRibbon.dll image (SHA256 941DCFCD96B3D9799945A5AB1B4D4B7D6859682A0AF9DF38223568D7F3C46A2F); a different image leaves ordinary Explorer chrome intact. Clean-PC, mixed-DPI and multiple-monitor hardware acceptance has not been completed. Touchpad assignments may require signing out and back in if Windows cannot refresh its gesture cache immediately; Options reports the pending state. Physical gestures still require a manual test.

## Quick check

1. Open Explorer. Widen and narrow it: tabs share the caption when they fit and move into an extra allocated row when needed. Check the real Quick Access Toolbar and address bar, then open two folders in separate tabs and switch between them.
2. Open twenty tabs with Ctrl+T: the strip scrolls and keeps the selected tab visible. Drag a tab along the strip to reorder it.
3. Right-click a top tab → Options. Turn each feature off and on independently; Cancel must discard unapplied changes.
4. On an ordinary resizable window, swipe Left → Down → Down. It should stay visible in the bottom-left quarter. Right should select the right half, Up should choose the top-right quarter, and another Up should maximise. Down should return to the top-right quarter.
5. Check that no new tray icon appears. Test dragging, maximising and restoring Explorer and using its usual file operations.
6. Snap two windows to opposite sides and drag Windows' middle divider. Snap another window into a corner and verify that it reuses the resulting horizontal boundary.

## Disable and remove

Use **Explorer MVP → Change or remove** or Windows Apps & Features. Setup first restores owned touchpad assignments and disables tabs. If gesture restoration is pending, sign out/in before removal. An Explorer module can stay loaded until its folder process exits; Setup reports incomplete removal rather than forcing Explorer to close or claiming files were removed. Close Explorer folder windows and retry removal when requested.

Source and licence notices are included in Source.zip. The derived distribution is GPL-3.0-or-later, with the original upstream notices retained.
