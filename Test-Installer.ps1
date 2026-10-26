. (Join-Path $PSScriptRoot 'Common.ps1')
$testRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'evidence\journal-tests'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$script:StateDir=$testRoot
$script:StateFile=Join-Path $testRoot 'state.clixml'
$script:State=@{Status='test';Changes=@()}
$keyPath='Software\Win10NativePatch-InstallerTest-'+[Guid]::NewGuid().ToString('N')
$key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($keyPath)
$key.SetValue('Existing',41,[Microsoft.Win32.RegistryValueKind]::DWord)
$key.SetValue('Text','original',[Microsoft.Win32.RegistryValueKind]::String)
$key.SetValue('Bytes',[byte[]](1,2,3),[Microsoft.Win32.RegistryValueKind]::Binary)
$key.Dispose()
$checks=@{}
try {
    Set-Tracked $keyPath 'Existing' 42
    Set-Tracked $keyPath 'Absent' 1
    Set-Tracked $keyPath 'Text' 'ours' 'String'
    Set-Tracked $keyPath 'Bytes' ([byte[]](4,5)) 'Binary'
    $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($keyPath,$true)
    $key.SetValue('Text','user-changed',[Microsoft.Win32.RegistryValueKind]::String);$key.Dispose()
    Restore-Tracked
    $checks.RestoresOriginalDword=(Read-Value $keyPath 'Existing').Value -eq 41
    $checks.RestoresAbsence=-not (Read-Value $keyPath 'Absent').Exists
    $checks.PreservesSubsequentUserChange=(Read-Value $keyPath 'Text').Value -eq 'user-changed'
    $bytes=Read-Value $keyPath 'Bytes'
    $checks.RestoresBinaryTypeAndContent=$bytes.Kind -eq 'Binary' -and ($bytes.Value -join ',') -eq '1,2,3'
    $script:State.Status='older';Save-State;$script:State.Status='newer';Save-State
    'deliberately invalid journal' | Set-Content -LiteralPath $script:StateFile
    $checks.RecoversAtomicBackup=(Read-State).Status -eq 'older'
    $shortcuts=New-Object int[] 50;$shortcuts[4]=1179657;$json=$shortcuts | ConvertTo-Json -Compress
    Add-Type -AssemblyName System.Runtime.Serialization
    $serializer=New-Object System.Runtime.Serialization.Json.DataContractJsonSerializer([int[]])
    $stream=New-Object IO.MemoryStream(,[Text.Encoding]::UTF8.GetBytes($json))
    $read=$serializer.ReadObject($stream);$stream.Dispose()
    $checks.ShortcutsRoundtripThroughActualQtDeserializer=$read.Count -eq 50 -and $read[4] -eq 1179657
    $result=@{Success=(@($checks.Values | Where-Object { -not $_ }).Count -eq 0);Checks=$checks}
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'evidence\installer-tests.json') -Encoding UTF8
    $result | ConvertTo-Json -Depth 5
    if (-not $result.Success) { throw 'Installer journal validation failed' }
} finally {
    if ($keyPath -notmatch '^Software\\Win10NativePatch-InstallerTest-[a-f0-9]{32}$') { throw 'Unexpected registry cleanup path' }
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($keyPath,$false)
    Get-ChildItem -LiteralPath $testRoot -File | Remove-Item -Force
    Remove-Item -LiteralPath $testRoot
}
