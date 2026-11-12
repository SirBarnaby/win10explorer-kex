param([switch]$PreflightOnly,[switch]$RecoverOwnedQT)
. (Join-Path $PSScriptRoot 'Common.ps1')
$os=Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
if (-not [Environment]::Is64BitOperatingSystem -or [int]$os.CurrentBuild -lt 19041 -or [int]$os.CurrentBuild -ge 22000 -or $os.InstallationType -ne 'Client') {
    throw 'This package requires Windows 10 x64, version 2004 through 22H2. Windows 11 and Windows Server are not supported.'
}
if (-not [Environment]::Is64BitProcess) { throw 'Run Install.cmd with 64-bit Windows PowerShell.' }
$net=Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full' -ErrorAction SilentlyContinue
if ($net.Release -lt 528040) { throw '.NET Framework 4.8 is required. Install it from Microsoft, then run this installer again.' }
$payload=Join-Path $PSScriptRoot 'payload'
$manifest=Join-Path $PSScriptRoot 'SHA256SUMS.txt'
if (-not (Test-Path -LiteralPath $manifest)) { throw 'Package hash manifest is missing.' }
foreach ($line in Get-Content -LiteralPath $manifest) {
    if ($line -notmatch '^([a-f0-9]{64})  (.+)$') { throw 'Malformed package manifest.' }
    $expected=$Matches[1]; $relative=$Matches[2]
    $full=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot $relative))
    if (-not $full.StartsWith(([IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')+'\'),[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid manifest path.' }
    if ((Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash -ine $expected) { throw "Package integrity check failed: $relative" }
}
$reuseOwnedQT=$false
if (Test-Path -LiteralPath $script:StateFile) {
    $script:State=Read-State
    if ($script:State.Status -eq 'Installed') {
        Write-Host "Already installed at $script:AppDir. Run Uninstall.cmd before changing versions."
        exit 0
    }
    if ($RecoverOwnedQT -and $script:State.PlannedQT -and $script:State.Error -eq 'The Explorer extension registration was not found after MSI installation.' -and (Test-QTInstalled)) {
        # Recover only our recorded initial ARP-view verification defect, never adopt an unrelated install.
        $reuseOwnedQT=$true
    }
    if ($script:State.Status -ne 'Uninstalled') { throw "A previous installation needs recovery. Run Uninstall.cmd first. State: $script:StateFile" }
}
if (-not $reuseOwnedQT -and (@(Get-QTProduct).Count -gt 0 -or (Test-Path -LiteralPath 'Registry::HKEY_CLASSES_ROOT\CLSID\{D2BF470E-ED1C-487F-A333-2BD8835EB6CE}'))) {
    throw 'An existing QTTabBar installation was found. This package will not replace or upgrade it.'
}
if ((Test-Path -LiteralPath $script:AppDir) -and @(Get-ChildItem -LiteralPath $script:AppDir -Force).Count -gt 0) { throw "An unrecognized program folder exists: $script:AppDir" }
$probe=& (Join-Path $payload 'TouchpadCache.exe') --probe
$probeCode=$LASTEXITCODE
$hasTouchpad=Test-Path -LiteralPath "HKCU:\$script:TouchpadPath"
if ($hasTouchpad -and $probeCode -ne 0) { Write-Warning 'Live gesture refresh is unavailable on this build. Changes will activate at next sign-in.' }
$hotkeyProcess=Start-Process -FilePath (Join-Path $payload 'NativeSnap.exe') -ArgumentList '--check-hotkeys' -WindowStyle Hidden -Wait -PassThru
if ($hotkeyProcess.ExitCode -ne 0) { throw 'The snap shortcuts are already used by another program. Close the conflicting window manager and try again.' }
Write-Host "Verified: $($os.ProductName) $($os.DisplayVersion), x64; .NET 4.8; package files; snap shortcuts."
if ($PreflightOnly) { Write-Host "Touchpad cache probe: $probe"; exit 0 }
New-Item -ItemType Directory -Path $script:StateDir,$script:AppDir -Force | Out-Null
if (Test-Path -LiteralPath $script:StateFile) { Copy-Item -LiteralPath $script:StateFile -Destination (Join-Path $script:StateDir ('previous-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.clixml')) }
$script:State=@{Version='1.0.0';Status='Installing';Changes=@();PlannedQT=$reuseOwnedQT;QTOwned=$reuseOwnedQT;Started=(Get-Date).ToString('o');WindowsBuild="$($os.CurrentBuild).$($os.UBR)";Touchpad=$hasTouchpad}
Save-State
try {
    Copy-Item -LiteralPath (Join-Path $payload 'NativeSnap.exe'),(Join-Path $payload 'TouchpadCache.exe') -Destination $script:AppDir
    foreach ($file in 'Common.ps1','Uninstall.ps1','README.txt') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $script:AppDir }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses') -Destination $script:AppDir -Recurse
    # Record every value BEFORE writing it. Uninstall restores only values still matching ours.
    $base='Software\QTTabBar'
    Set-Tracked $base 'WindowAlpha' '255' 'String'
    $zeros=@{
        Lang=@('UseLangFile','BuiltInLangSelectedIndex');
        Window=@('CaptureNewWindows','CaptureWeChatSelection','RestoreSession','RestoreOnlyLocked','CloseBtnClosesSingleTab','CloseBtnClosesUnlocked','TrayOnClose','TrayOnMinimize','AutoHookWindow','AutoEnableExperimental','OpenExplorerMaximized');
        Tabs=@('MultipleTabRows','DragOverTabOpensSDT','ShowSubDirTipOnTab','ShowNavButtons','RenameAmbTabs','NeverOpenSame');
        Tips=@('ShowSubDirTips','SubDirTipsPreview','SubDirTipsFiles','ShowTooltipPreviews','ShowPreviewInfo');
        Skin=@('UseTabSkin','UseRebarImage','SkinAutoColorChangeClose','DrawHorizontalExplorerBarBgColor','DrawVerticalExplorerBarBgColor','TabTitleShadows');
        Tweaks=@('BackspaceUpLevel','WrapArrowKeySelection','HorizontalScroll');
        Keys=@('UseTabSwitcher')
    }
    foreach ($category in $zeros.Keys) { foreach ($name in $zeros[$category]) { Set-Tracked "$base\Config\$category" $name 0 } }
    foreach ($name in 'ShowFolderIcon','ShowCloseButtons','NeedPlusButton') { Set-Tracked "$base\Config\Tabs" $name 1 }
    Set-Tracked "$base\Config\Lang" 'BuiltInLang' 'English' 'String'
    Set-Tracked "$base\Config\Lang" 'LangFile' '' 'String'
    foreach ($name in 'TabImageFile','RebarImageFile') { Set-Tracked "$base\Config\Skin" $name '' 'String' }
    Set-Tracked "$base\Config\Plugin" 'Enabled' '[]' 'String'
    Set-Tracked "$base\Config\Mouse" 'MarginActions' '[]' 'String'
    $shortcuts=New-Object int[] 50
    $shortcuts[4]=1179657; $shortcuts[5]=1245193; $shortcuts[9]=1179732; $shortcuts[10]=1179726; $shortcuts[12]=1179735; $shortcuts[17]=1245268
    Set-Tracked "$base\Config\Keys" 'Shortcuts' ($shortcuts | ConvertTo-Json -Compress) 'String'
    $script:State.PlannedQT=$true; Save-State
    $msiExit=0
    if (-not $reuseOwnedQT) { $msiExit=Invoke-Msi "/i `"$(Join-Path $payload 'QTTabBar.1.6.4.msi')`" TRANSFORMS=`"$(Join-Path $payload 'QTTabBar.SafeInstall.mst')`" TRANSFORMSSECURE=1" 'explorer-install.log' }
    $script:State.RebootRequired=($msiExit -eq 3010); Save-State
    if (-not (Test-QTInstalled)) { throw 'Windows Installer did not confirm the pinned Explorer extension after installation.' }
    $script:State.QTOwned=$true; Save-State
    $activated=Set-ExplorerTabBar $true
    # Enabling the band through Explorer persists its toolbar choice. No imitation shell/Win+E override.
    if ($activated -eq 0) {
        Start-Process -FilePath (Join-Path $env:WINDIR 'explorer.exe') -ArgumentList ('"'+[Environment]::GetFolderPath('MyDocuments')+'"')
        Start-Sleep -Milliseconds 1200
        $activated=Set-ExplorerTabBar $true
    }
    if ($activated -eq 0) { Write-Warning 'Enable QTTabBar in Explorer: View > Options dropdown > QTTabBar.' }
    $exe=Join-Path $script:AppDir 'NativeSnap.exe'
    $shortcut=Join-Path ([Environment]::GetFolderPath('Programs')) 'Native Snap (Win10 Patch).lnk'
    if (-not (Test-Path -LiteralPath $shortcut)) {
        $script:State.Shortcut=$shortcut; Save-State
        $wsh=New-Object -ComObject WScript.Shell
        try {
            $link=$wsh.CreateShortcut($shortcut);$link.TargetPath=$exe;$link.WorkingDirectory=$script:AppDir
            $link.Description='Reliable directional window snapping';$link.IconLocation="$exe,0";$link.Save()
        } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($wsh) | Out-Null }
    }
    $run='Software\Microsoft\Windows\CurrentVersion\Run'
    Set-Tracked $run 'Win10NativeSnap' ('"'+$exe+'"') 'String'
    $p=Start-Process -FilePath $exe -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 500
    if ($p.HasExited) { throw "Snap helper exited at startup ($($p.ExitCode))." }
    if ($hasTouchpad) {
        $vk=@{Left=0x25;Up=0x26;Right=0x27;Down=0x28}
        foreach ($direction in 'Left','Up','Right','Down') {
            Set-Tracked $script:TouchpadPath "ThreeFinger${direction}KeyParams" (($vk[$direction] -shl 16) -bor 0x15)
            Set-Tracked $script:TouchpadPath "ThreeFinger${direction}" 65535
        }
        Set-Tracked $script:TouchpadPath 'ThreeFingerSlideEnabled' 65535
        $script:State.LiveGestures=Refresh-Gestures
    }
    $uninstall='Software\Microsoft\Windows\CurrentVersion\Uninstall\Win10NativePatch'
    Set-Tracked $uninstall 'DisplayName' 'Windows 10 Explorer Tabs and Reliable Snap' 'String'
    Set-Tracked $uninstall 'DisplayVersion' '1.0.0' 'String'
    Set-Tracked $uninstall 'InstallLocation' $script:AppDir 'String'
    Set-Tracked $uninstall 'DisplayIcon' $exe 'String'
    Set-Tracked $uninstall 'UninstallString' ('"'+$env:SystemRoot+'\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "'+$script:AppDir+'\Uninstall.ps1"') 'String'
    Set-Tracked $uninstall 'NoModify' 1; Set-Tracked $uninstall 'NoRepair' 1
    $script:State.Status='Installed'; $script:State.ExplorerWindowsActivated=$activated; Save-State
    Write-Host 'Installed. Explorer tab-bar activation was requested; the snap helper starts at sign-in.'
    Write-Host 'Three-finger swipe or Ctrl+Alt+Win+arrow: left/right snap, opposite direction centers, up maximizes, down centers. Repeating down never minimizes.'
    Write-Host "Uninstall from Apps & features, or run Uninstall.cmd. Recovery journal: $script:StateFile"
    if ($script:State.RebootRequired) { Write-Warning 'Windows Installer requested a restart to finish setup. Restart Windows when convenient.' }
} catch {
    $failure=$_.Exception.Message
    $script:State.Status='Failed'; $script:State.Error=$failure; Save-State
    Write-Warning "Installation did not finish: $failure. Restoring the recorded settings."
    try {
        $rollback=Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $PSScriptRoot 'Uninstall.ps1')+'"') -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $script:StateDir 'rollback-output.log') -RedirectStandardError (Join-Path $script:StateDir 'rollback-error.log')
        if ($rollback.ExitCode -ne 0) { throw 'Automatic rollback could not finish.' }
        Write-Host 'Previous settings restored.'
    } catch { Write-Warning "Recovery remains available: run Uninstall.cmd. Logs and journal: $script:StateDir" }
    [Console]::Error.WriteLine("Installation failed: $failure")
    exit 1
}
