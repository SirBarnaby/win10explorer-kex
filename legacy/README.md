# Legacy: version 1.0 (QTTabBar-based)

These files belong to the first version of this project, which packaged QTTabBar 1.6.4 together with a snap helper. The current version replaces it with its own tab strip (see the README at the repository root). Nothing in the current build uses these files.

They were moved here from the repository root unchanged. The scripts still expect to sit at the root, next to `src\`, `bin\` and `package\`, so copy them back there before running them.

- `README.txt`: the user guide shipped with version 1.0.
- `BUILDING.md`, `Research.md`: how version 1.0 was built, and the upstream sources it pinned.
- `Install.*`, `Uninstall.*`, `Common.ps1`, `Test-Installer.ps1`: the version 1.0 installer.
- `Build-Package.ps1`, `Build-NativeSnap.ps1`, `Build-TouchpadCache.ps1`: the version 1.0 build scripts.
