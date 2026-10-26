$ErrorActionPreference = 'Stop'
$script:AppDir = Join-Path $env:LOCALAPPDATA 'Programs\Win10NativePatch'
$script:StateDir = Join-Path $env:LOCALAPPDATA 'Win10NativePatch'
$script:StateFile = Join-Path $script:StateDir 'installation.clixml'
$script:TouchpadPath = 'Software\Microsoft\Windows\CurrentVersion\PrecisionTouchPad'
$script:ProductCode = '{8E3584AF-23BA-4E2E-B490-8B58D7FB180D}'
function Test-QTInstalled {
    $installer=New-Object -ComObject WindowsInstaller.Installer
    try { return $installer.ProductState($script:ProductCode) -eq 5 -and $installer.ProductInfo($script:ProductCode,'VersionString') -eq '1.6.4' }
    catch { return $false }
    finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) | Out-Null }
}
function Get-QTProduct {
    foreach ($path in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall','HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall','HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall') {
        Get-ChildItem -LiteralPath $path -ErrorAction SilentlyContinue | ForEach-Object {
            $p = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
            if ($p.DisplayName -like 'QTTabBar*') { $p }
        }
    }
}
function Read-Value([string]$Path,[string]$Name) {
    $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($Path)
    try {
        if ($key -and $key.GetValueNames() -contains $Name) {
            return @{ Exists=$true; Kind=$key.GetValueKind($Name).ToString(); Value=$key.GetValue($Name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
        }
        return @{ Exists=$false; Kind=''; Value=$null }
    } finally { if ($key) { $key.Dispose() } }
}
function Same-Value($A,$B) {
    if ($A.Exists -ne $B.Exists) { return $false }
    if (-not $A.Exists) { return $true }
    return $A.Kind -eq $B.Kind -and (@(Compare-Object -ReferenceObject @($A.Value) -DifferenceObject @($B.Value)).Count -eq 0)
}
function Save-State {
    $temporary=Join-Path $script:StateDir ('journal-'+[Guid]::NewGuid().ToString('N')+'.tmp')
    try {
        $script:State | Export-Clixml -LiteralPath $temporary -Depth 12
        if (Test-Path -LiteralPath $script:StateFile) { [IO.File]::Replace($temporary,$script:StateFile,($script:StateFile+'.bak')) }
        else { [IO.File]::Move($temporary,$script:StateFile) }
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
}
function Read-State {
    try { return Import-Clixml -LiteralPath $script:StateFile }
    catch {
        if (Test-Path -LiteralPath ($script:StateFile+'.bak')) {
            Write-Warning 'Recovering the previous atomic registry journal backup.'
            return Import-Clixml -LiteralPath ($script:StateFile+'.bak')
        }
        throw
    }
}
function Set-Tracked([string]$Path,[string]$Name,$Value,[string]$Kind='DWord') {
    $before=Read-Value $Path $Name
    $after=@{ Exists=$true; Kind=$Kind; Value=$Value }
    if (Same-Value $before $after) { return }
    $entry=@{Path=$Path;Name=$Name;Before=$before;After=$after}
    $script:State.Changes += $entry
    Save-State
    $key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($Path)
    try { $key.SetValue($Name,$Value,[Microsoft.Win32.RegistryValueKind]::$Kind); $key.Flush() } finally { $key.Dispose() }
    if (-not (Same-Value (Read-Value $Path $Name) $after)) { throw "Registry readback failed: $Path\$Name" }
}
function Restore-Tracked([switch]$RegistrationOnly,[switch]$SkipRegistration) {
    $changes=@($script:State.Changes)
    [array]::Reverse($changes)
    foreach ($entry in $changes) {
        $isRegistration=$entry.Path -eq 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Win10NativePatch'
        if (($RegistrationOnly -and -not $isRegistration) -or ($SkipRegistration -and $isRegistration)) { continue }
        $current=Read-Value $entry.Path $entry.Name
        if (-not (Same-Value $current $entry.After)) { Write-Host "Preserved subsequently changed setting: $($entry.Name)"; continue }
        $key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($entry.Path)
        try {
            if ($entry.Before.Exists) { $kind=[Microsoft.Win32.RegistryValueKind][Enum]::Parse([Microsoft.Win32.RegistryValueKind],$entry.Before.Kind); $key.SetValue($entry.Name,$entry.Before.Value,$kind) }
            else { $key.DeleteValue($entry.Name,$false) }
            $key.Flush()
        } finally { $key.Dispose() }
    }
}
function Invoke-Msi([string]$Arguments,[string]$LogName) {
    $log=Join-Path $script:StateDir $LogName
    $argsWithLog="$Arguments /qn /norestart REBOOT=ReallySuppress MSIRESTARTMANAGERCONTROL=Disable /L*v `"$log`""
    Write-Host 'Windows will request administrator permission for the Explorer extension.'
    $p=Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\msiexec.exe') -ArgumentList $argsWithLog -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    if ($p.ExitCode -notin 0,3010) { throw "Explorer extension installer failed ($($p.ExitCode)). Log: $log" }
    return $p.ExitCode
}
function Set-ExplorerTabBar([bool]$Show) {
    $shell=New-Object -ComObject Shell.Application
    $count=0
    try {
        foreach ($window in @($shell.Windows())) {
            try {
                if ([IO.Path]::GetFileName($window.FullName) -ine 'explorer.exe') { continue }
                $window.ShowBrowserBar('{D2BF470E-ED1C-487F-A333-2BD8835EB6CE}',$Show,$null)
                $count++
            } catch { Write-Warning "Explorer toolbar activation: $($_.Exception.Message)" }
        }
    } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) | Out-Null }
    return $count
}
function Refresh-Gestures {
    $cache=Join-Path $script:AppDir 'TouchpadCache.exe'
    $result=& $cache --refresh
    $cacheExit=$LASTEXITCODE
    $result | Set-Content -LiteralPath (Join-Path $script:StateDir 'touchpad-refresh.json') -Encoding UTF8
    if ($cacheExit -ne 0) { Write-Warning 'Gesture settings are saved. Sign out and sign in to activate them on this Windows build.'; return $false }
    return $true
}
