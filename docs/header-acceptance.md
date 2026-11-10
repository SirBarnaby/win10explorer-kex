# Native Explorer header and snapping acceptance — 4 November 2026

The two Windows changes are implemented and verified in Windows Sandbox. The user's desktop installation was not updated, and no Computer Use tools were used for this run.

Explorer tabs measure the real Quick Access Toolbar commands. They share the caption when space permits and otherwise occupy an extra row reserved through the native Ribbon height notification. The original commands remain present and clickable; the address bar stays below the allocated row. No native Ribbon regions are cut out. Disabling the extension restores the original layout.

Snapping enters Windows' actual arranged state before applying the remembered split. Windows' shared divider resizes both partners. Its completed horizontal adjustment is saved per display and reused for halves and corners. Another Up from a top corner maximizes; Down restores that corner. A pending divider adjustment settles before an immediate corner request can consume its preference.

## Final verification

- Session: `evidence/sandbox/20261104-143632/results/`.
- Aggregate gate: `evidence/implementation-20261104/final-acceptance.json`: **75/75 passed**, binding runtime identities to the exact release binaries.
- Model/configuration/setup checks: **172/172 passed**.
- Native two-window snapping checks: **24/24 passed**. These use the production receiver/CLI, actual Windows input and both partner frame rectangles; geometry alone cannot pass the shared-divider checks.
- Real Explorer: wide → 400-pixel narrow → wide, complete native command inspection, zero command/address-bar collisions, real Navigation pane checkbox toggle/restoration, and original geometry after detach.
- Failed accessibility inspection: the rejection actually executed; no tab strip, active marker or native callback remained, and the saved tab session was preserved.
- Two real STA threads linked to the current production header integration: two hooks created and removed by their respective owners, no wrong-thread removals and no failed native unhook calls. This component lifecycle proof does not claim two same-process Explorer STAs; that Explorer arrangement was unavailable in the guest.
- Completed Compound Engineering review: `evidence/implementation-20261104/code-review.json`, run `20261104-native-e2e94ab7`. Its four confirmed P2 findings were corrected after the review; `review-fixes.json` records the changes and final regression evidence. The original pre-fix receipt is retained verbatim.

| Component | Final SHA256 |
|---|---|
| ExplorerTabs.dll | E427A3B21CD4D6578C8393660B0DA3FF08B22C6B9B14703BC50F98A0D02681E8 |
| NativeSnap.exe | 2FFFB893E15DFF10EF054400C60B32E3EFA7E20DDD1221E4B7676EE80FAA15E6 |

Guest screenshots are `header-wide.png`, `header-expanded.png`, `header-wide-again.png` and `header-restored.png` in the session results. The guest harness captured these images directly.

## Qualification and remaining limits

The host is Windows 10 22H2 19045.7807; Sandbox reports 19041.7807. Explorer, ExplorerFrame, UIRibbon, user32, win32u and twinui.pcshell match the host byte for byte. The lab transiently adapts the production DLL's version query during attachment and immediately restores that import. The shipped DLL bytes and strict 19045 compatibility check are unchanged. This proves behavior with the matching component images in this guest, not a native 19045 guest OS or physical hardware acceptance.

Physical touchpad gestures, multiple physical displays, mixed DPI, broad dark/high-contrast appearance, sleep/resume, independent clean-PC installation, and higher-integrity/UIPI-denied input were not exercised. Focus-change cancellation passed; destroyed-target handling has source identity guards but no final runtime destruction fixture. There is no claim of public-release or desktop installation acceptance.

## Durable implementation lessons

Native arranged state must survive geometry adjustment: restoring the window first loses Windows' shared resize association. Prove both partner windows resize through the actual divider, not just that their rectangles match. Preserve a completed user resize before executing the next layout command.

Header attachment can synchronously reenter Explorer layout. Publish the controller as enabled only after header ownership is committed, and preserve the prior session if startup never becomes usable. Each out-of-context WinEvent hook must be removed on the STA that installed it.
