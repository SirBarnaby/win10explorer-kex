$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$payload = Join-Path $root 'package\payload'
New-Item -ItemType Directory -Path $payload,(Join-Path $root 'package\licenses'),(Join-Path $root 'package\sources') -Force | Out-Null
$msi = Get-ChildItem -LiteralPath (Join-Path $root 'vendor\qt-extracted\AttachedContainer') -Filter '*.msi' | Select-Object -First 1
if (-not $msi) { throw 'Extract the pinned upstream QTTabBar release with WiX dark.exe first.' }
$safeMsi = Join-Path $root 'vendor\QTTabBar.safe-reference.msi'
Copy-Item -LiteralPath $msi.FullName -Destination $safeMsi -Force
$installer = New-Object -ComObject WindowsInstaller.Installer
$original = $installer.OpenDatabase($msi.FullName, 0)
$modified = $installer.OpenDatabase($safeMsi, 1)
foreach ($action in 'KillExplorer','WaitAfterKill','StartExplorer') {
    $view = $modified.OpenView("DELETE FROM ``InstallExecuteSequence`` WHERE ``Action`` = '$action'")
    $view.Execute(); $view.Close()
}
# Never remove the shared Internet Explorer Toolbar key during uninstall.
foreach ($marker in '*','-') {
    $view = $modified.OpenView("DELETE FROM ``Registry`` WHERE ``Root`` = 2 AND ``Key`` = 'Software\Microsoft\Internet Explorer\Toolbar' AND ``Name`` = '$marker'")
    $view.Execute(); $view.Close()
}
$modified.Commit()
$transform = Join-Path $payload 'QTTabBar.SafeInstall.mst'
if (Test-Path -LiteralPath $transform) { Remove-Item -LiteralPath $transform }
$null = $modified.GenerateTransform($original, $transform)
$null = $modified.CreateTransformSummaryInfo($original, $transform, 0, 0)
[Runtime.InteropServices.Marshal]::FinalReleaseComObject($modified) | Out-Null
[Runtime.InteropServices.Marshal]::FinalReleaseComObject($original) | Out-Null
[Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) | Out-Null
Copy-Item -LiteralPath $msi.FullName -Destination (Join-Path $payload 'QTTabBar.1.6.4.msi') -Force
foreach ($file in 'NativeSnap.exe','TouchpadCache.exe') { Copy-Item -LiteralPath (Join-Path $root "bin\$file") -Destination $payload -Force }
Copy-Item -LiteralPath (Join-Path $root 'vendor\qttabbar-source\LICENSE.txt') -Destination (Join-Path $root 'package\licenses\QTTabBar-GPL3.txt') -Force
Copy-Item -LiteralPath (Join-Path $root 'vendor\AppDock.LICENSE') -Destination (Join-Path $root 'package\licenses\AppDock-MIT.txt') -Force
$sourceZip=Join-Path $root 'package\sources\QTTabBar-v1.6.4-source.zip'
& git -C (Join-Path $root 'vendor\qttabbar-source') archive --format=zip "--output=$sourceZip" HEAD
if ($LASTEXITCODE -ne 0) { throw 'Source archive failed' }
Copy-Item -LiteralPath (Join-Path $root 'src') -Destination (Join-Path $root 'package\sources') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'vendor\rectanglewin-reference') -Destination (Join-Path $root 'package\sources') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'vendor\RectangleWin.LICENSE') -Destination (Join-Path $root 'package\licenses\RectangleWin-Apache2.txt') -Force
Copy-Item -LiteralPath (Join-Path $root 'vendor\PowerToys.LICENSE') -Destination (Join-Path $root 'package\licenses\PowerToys-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $root 'Build-NativeSnap.ps1'),(Join-Path $root 'Build-Package.ps1') -Destination (Join-Path $root 'package\sources') -Force
Copy-Item -LiteralPath (Join-Path $root 'Test-Installer.ps1'),(Join-Path $root 'Build-TouchpadCache.ps1') -Destination (Join-Path $root 'package\sources') -Force
Copy-Item -LiteralPath (Join-Path $root 'vendor\TouchpadWindowsCache.reference.cs'),(Join-Path $root 'vendor\WindowsTouchpadSettings.reference.cs') -Destination (Join-Path $root 'package\sources') -Force
Copy-Item -LiteralPath (Join-Path $root 'BUILDING.md') -Destination (Join-Path $root 'package\sources') -Force
New-Item -ItemType Directory -Path (Join-Path $root 'package\sources\tools') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'tools\Generate-NativeSnapIcon.py') -Destination (Join-Path $root 'package\sources\tools') -Force
Copy-Item -LiteralPath (Join-Path $root 'evidence') -Destination (Join-Path $root 'package') -Recurse -Force
foreach ($file in 'Install.cmd','Install.ps1','Uninstall.cmd','Uninstall.ps1','Common.ps1','README.txt','Research.md') {
    Copy-Item -LiteralPath (Join-Path $root $file) -Destination (Join-Path $root "package\$file") -Force
}
$files = Get-ChildItem -LiteralPath (Join-Path $root 'package') -Recurse -File | Where-Object Name -ne 'SHA256SUMS.txt' | Sort-Object FullName
$lines = foreach ($file in $files) { '{0}  {1}' -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$file.FullName.Substring((Join-Path $root 'package').Length+1) }
$lines | Set-Content -LiteralPath (Join-Path $root 'package\SHA256SUMS.txt') -Encoding ASCII
Write-Host 'Offline package assembled.'
