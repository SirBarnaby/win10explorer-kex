. (Join-Path $PSScriptRoot 'Common.ps1')
if (-not (Test-Path -LiteralPath $script:StateFile)) { Write-Host 'No installation journal found. Nothing was changed.'; exit 0 }
$script:State=Read-State
if ($script:State.Status -eq 'Uninstalled') { Write-Host 'Already uninstalled.'; exit 0 }
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit Windows PowerShell.' }
try {
    if ($script:State.PlannedQT -and (Test-QTInstalled)) {
        $null=Set-ExplorerTabBar $false
        $msiExit=Invoke-Msi "/x $script:ProductCode" 'explorer-uninstall.log'
        $script:State.UninstallRebootRequired=($msiExit -eq 3010); Save-State
    }
    # Restore gestures before stopping their receiver, including original absent values/types.
    Restore-Tracked -SkipRegistration
    if ($script:State.Touchpad -and (Test-Path -LiteralPath (Join-Path $script:AppDir 'TouchpadCache.exe'))) { $null=Refresh-Gestures }
    $exe=Join-Path $script:AppDir 'NativeSnap.exe'
    if (Test-Path -LiteralPath $exe) {
        $stop=Start-Process -FilePath $exe -ArgumentList '--stop' -WindowStyle Hidden -Wait -PassThru
        if ($stop.ExitCode -ne 0) { throw 'The snap helper did not stop. Close it from its tray menu and run uninstall again.' }
        Start-Sleep -Milliseconds 400
    }
    if ($script:State.Shortcut -and (Test-Path -LiteralPath $script:State.Shortcut)) {
        $wsh=New-Object -ComObject WScript.Shell
        try {
            $link=$wsh.CreateShortcut($script:State.Shortcut)
            if ($link.TargetPath -ieq $exe) { Remove-Item -LiteralPath $script:State.Shortcut }
        } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($wsh) | Out-Null }
    }
    # Delete only our named program directory after checking its exact resolved destination.
    $expected=[IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs\Win10NativePatch')).TrimEnd('\')
    if ([IO.Path]::GetFullPath($script:AppDir).TrimEnd('\') -ine $expected) { throw 'Unexpected uninstall path.' }
    if (Test-Path -LiteralPath $script:AppDir) { Remove-Item -LiteralPath $script:AppDir -Recurse -Force }
    Restore-Tracked -RegistrationOnly
    $script:State.Status='Uninstalled'; $script:State.Uninstalled=(Get-Date).ToString('o'); Save-State
    Write-Host "Uninstalled. Backups and logs remain at $script:StateDir. Explorer finishes unloading its extension at the next sign-in."
    if ($script:State.UninstallRebootRequired) { Write-Warning 'Windows Installer requested a restart to finish removal. Restart Windows when convenient.' }
} catch { Write-Error "Uninstall needs another attempt: $($_.Exception.Message). Backup journal retained at $script:StateFile"; exit 1 }
