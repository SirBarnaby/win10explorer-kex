# Manual testing before the Explorer gate

These are tests of the built native components, not an accepted release installation. Keep `NATIVE-STATUS.md` with the reports. Explorer tabs remain unavailable until the isolated gate passes; the existing QTTabBar installation is preserved.

## Check the Options dialog without changing anything

Open `build-native-verified/Release/Win10PatchOptions.exe`, or the same file in the development bundle. Visit Features, Explorer, Snapping and Diagnostics, check text clipping and keyboard navigation, and close with **Cancel**. No Apply is necessary for this inspection. In a development folder there is no installed Start-menu entry yet.

The earlier computer-use capture attempts failed, so screenshots or layout findings from this inspection would be useful. Note the Windows display scaling and theme with any finding.

## Safest real-window snap test: separate keyboard route

This leaves the existing v1 helper and actual touchpad assignments intact.

1. Open native Options. Leave **Explorer tabs** and **Touchpad snap gestures** off.
2. Enable **Alternate keyboard shortcuts**. On Snapping select **Ctrl + Alt + Shift**, with Windows **unchecked**, and keep Left/Up/Right/Down as the direction keys. This avoids v1's Ctrl+Alt+Windows+Arrow route.
3. Select **Apply**. The status must confirm keyboard input is enabled; a conflict should produce an error and preserve the previous working state.
4. With an ordinary resizable window selected, use Ctrl+Alt+Shift+Arrow. Test Left, Left again, then Right to center. From floating, test Right → Up → Up for a stable top-right quarter. Press Left once to center again, then Left → Down → Down for a stable bottom-left quarter. Use the opposite horizontal direction to center before starting a sequence on the other side. Down must never minimize.
5. Test all four corners, manual move/resize between sequences, maximized → Down restore, and an application with a large minimum window size. Ordinary Win+Arrow must retain Windows behavior.
6. When finished, turn **Alternate keyboard shortcuts off** and Apply. The native helper should exit and its native startup value should disappear. The v1 helper/touchpad route remains as it was.

Applying keyboard settings creates the versioned native settings record and, while enabled, the native helper's startup entry. Keep the development folder in place until the keyboard test is disabled. Do not leave an experimental binding enabled across sign-out merely to test persistence before the installer/sign-in gate is ready.

## Optional temporary test through the existing swipe keys

Only use this route if you specifically want to test actual swipe geometry before full native gesture migration. The old touchpad assignments already emit Ctrl+Alt+Windows+Arrow; the new handler can receive that same sequence through its **keyboard** route.

1. Exit the **old** v1 Native Snap helper from its own tray menu. Do not uninstall anything or edit Windows touchpad settings. Its original startup entry remains for recovery.
2. In native Options leave **Touchpad snap gestures off**, enable **Alternate keyboard shortcuts**, and choose Ctrl+Alt+Windows with Shift off and the four arrow keys. Apply must confirm successful registration. If a conflict remains, cancel and restart the old helper.
3. Try the three-finger sequences. Left → Down → Down should stay in the bottom-left corner. Right → Up should reach the top-right corner. This tests the new geometry through the old emitted keys; it does not validate the final private F13–F16 gesture transport or its restoration.
4. Disable the native keyboard route and Apply when done. Then restart the original helper using the existing **Native Snap (Win10 Patch)** Start-menu shortcut. Do not sign out with both helpers configured for the same key combination; the first receiver would win registration and the other would fail.

Do not enable the development **Touchpad snap gestures** switch to substitute for installer migration. Full migration must import the original v1 ownership journal first. The native installer implements that sequence, but installation/cancellation/repair/restoration remain acceptance tests.

## Reports

`Verification` in the development bundle contains the 38 snap checks, 34 configuration/restore checks, 8 migration checks, 12 Explorer model/metadata checks, 5 release rejection checks and the read-only gesture probe. Those reports are deliberately explicit that physical gestures and clean-VM Explorer acceptance have not passed.

Keep notes of the exact input sequence, expected rectangle, actual rectangle/visibility, application, monitor/scaling and whether the window was previously maximized or manually moved. Do not treat a screenshot of Options as proof of native Explorer integration.
