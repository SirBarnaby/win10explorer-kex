# Isolated Windows 10 test environment requested

The native replacement is built, but the approved Explorer deployment gate has not passed. Please provide this environment later; there is no need to change or restart the working PC now.

## Environment

- A disposable **Windows 10 22H2 x64** desktop, OS build **19045**. Record the full cumulative-update build (for example 19045.7807). The initial target should match the working PC; add a second clean cumulative-update build before release.
- **Windows Sandbox on this Windows 10 host**, or a clean Hyper-V/VMware/VirtualBox guest. Windows 11 Sandbox would test a Windows 11 guest and would not satisfy this gate.
- The original Windows 10 Explorer ribbon, with **QTTabBar absent** and no ExplorerPatcher, StartAllBack, Windhawk Explorer mod, or other caption/shell customization. Keep ordinary Back/Forward/address-bar behavior available.
- A local administrator account inside the guest for extension registration and installer tests. Keep host credentials and personal folders out of the guest.
- At least two virtual CPUs and 4 GiB RAM; 8 GiB is preferable for repeatable workload measurements. A checkpoint/snapshot, or Sandbox's disposable reset, must be available.
- A way for the agent to inspect and interact with the **actual guest desktop**, including screenshots. The current host capture helper timed out on the Options dialog, so reliable guest screen capture is a prerequisite for appearance acceptance.
- Ability to change guest scaling through 100%, 125%, 150%, 175% and 200%, and to switch light, dark and high-contrast themes. A second display/mixed DPI is useful for the later matrix.
- A writable guest folder for logs, screenshots and test documents. Map the built package/source **read-only** from the host; if output must return through a mapping, use a separate empty evidence folder with write access. Do not map a personal home directory.
- Networking can be off for initial caption and navigation proof. Enable it later only for explicit network/cloud folder tests with disposable data.

No development tools are required just to run the native binaries. MSVC 2022 C++ Build Tools, a Windows SDK and CMake are useful if repairs must be rebuilt inside the guest. The patch binaries have no .NET, Qt, Electron or WebView runtime dependency.

Microsoft's setup prerequisites and instructions: https://learn.microsoft.com/en-us/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-install

## Package and state to bring

- `dist/Windows10-Native-Patch-2.0.0-DEVELOPMENT-x64.zip` and its `.sha256` sidecar. It contains source, licenses, checksums and verification reports. It intentionally contains no production MSI.
- The current `native/acceptance.template.json`, `NATIVE-STATUS.md` and `NATIVE-README.md`.
- A checkpoint of the clean desktop before any COM registration. We must be able to restore the clean state and compare measurements.
- A guest-only registration/configuration harness, prepared and inspected when the environment is available. **Do not register the DLL on the host, blindly run regsvr32, or copy Windows 11 Explorer binaries.**

## First session: the feasibility gate

1. Confirm guest OS/architecture/full build, clean Explorer, no QTTabBar, and working screen capture. Verify package checksums.
2. Record clean screenshots and normal Explorer geometry. Establish the checkpoint.
3. In the guest only, attach the native COM extension to its own supported Explorer folder window on the owning UI thread. Enable the explicit isolated-proof marker. The `RunProof` export refuses arbitrary external processes/threads/windows; it is not a rundll32 command.
4. Prove the tab strip is a real child of Explorer in the title/QAT area. Test native caption buttons, draggable space, resize borders, ribbon, address bar and navigation controls. Check normal, maximized and snapped layouts. Unsupported geometry must restore ordinary chrome and report the rejection.
5. Turn tabs off and back on. The active folder must remain open, ordinary chrome must return, and the saved tab session must recover.
6. Capture actual Explorer images and hit-test evidence. If caption painting, input or clipping cannot be reliable, record that blocker and stop deployment. A toolbar or an external file-browser window does not satisfy the requirement.

The existing `RunProof` checks only same-folder tab creation, the real shell connection, packet round-trip and active-folder preservation during toggling. It does not certify appearance, performance, all navigation, or file operations.

## Later sessions after feasibility passes

- New/close/duplicate/reopen/reorder/detach/cross-window tabs, overflow, keyboard shortcuts, middle-click folder opening, MSAA/accessibility, per-tab Back/Forward, view modes, selection and visible scroll anchors. Verify Win+E and Ctrl+N still create separate windows.
- Search results, virtual folders, network locations and cloud providers, with ordinary copy/move/rename/delete operations on disposable files. Exact pixel scroll restoration is not promised by Explorer's public anchor API.
- All eight combinations of tabs/gestures/keyboard shortcuts, Restore Tabs on/off, all disabled, sign-in persistence, pending states and original-gesture restoration while preserving changes made directly in Windows.
- Installer selection, cancelled elevation, repair, upgrade, feature removal, uninstall and interrupted setup. Test Qt-only retirement from an owned v1 installation separately, retaining snapping and the recovery journal. Test multiple users before machine-wide removal is accepted.
- Startup failure breaker, emergency disable, sleep/resume and monitor/DPI changes.
- Matched clean-versus-patched workloads: ten idle tabs add at most 20 MiB private memory; added extension processing is at most 50 ms p95 excluding native enumeration; repeatable file-operation throughput regression is at most 5%; 1,000 tab operations show no continuing memory/handle/GDI growth; no idle polling or continuous rendering.

Sandbox may not deliver a real Precision Touchpad's three-finger input to the guest. Physical gestures, additional physical monitors and the final cutover therefore also need tests on a real Windows 10 PC. An isolated keyboard test is not a substitute for those checks.

## Evidence needed back

Keep OS/build identifiers, artifact SHA-256 values, screenshots, diagnostics, test logs, workload descriptions and raw measurements together. Complete the acceptance record only for tests actually performed. Record limitations and failures explicitly. The release builder also checks hashes of all four production binaries and the attached evidence files; rebuilding a binary invalidates that acceptance.

Current blockers and completed tests are listed in `NATIVE-STATUS.md`. The working PC's Explorer and v1 gesture assignments are intentionally unchanged.
