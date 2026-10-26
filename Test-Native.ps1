[CmdletBinding()]
param([string]$BinaryDirectory=(Join-Path $PSScriptRoot 'build-native-verified\Release'))
$ErrorActionPreference='Stop'
$evidence=Join-Path $PSScriptRoot 'evidence\native-v2'
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$cases=@(
 @{Exe='NativeSnap.exe';Args=@('--self-test',(Join-Path $evidence 'snap.json'))},
 @{Exe='Win10PatchOptions.exe';Args=@('--self-test',(Join-Path $evidence 'config.json'))},
 @{Exe='NativePatchSetup.exe';Args=@('--self-test',(Join-Path $evidence 'migration.json'))},
 @{Exe='NativePatchModelTests.exe';Args=@((Join-Path $BinaryDirectory 'ExplorerTabs.dll'),(Join-Path $evidence 'explorer-model.json'))},
 @{Exe='Win10PatchOptions.exe';Args=@('--probe',(Join-Path $evidence 'gesture-probe.json'))}
)
foreach ($case in $cases) {
 # Paths originate in this workspace; quotes are passed as actual argument delimiters.
 $arguments=($case.Args | ForEach-Object { if ($_ -match '["\r\n]') {throw 'Unexpected quote in test argument.'};'"'+$_+'"' }) -join ' '
 $p=Start-Process -FilePath (Join-Path $BinaryDirectory $case.Exe) -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
 if ($p.ExitCode) { throw "$($case.Exe) returned $($p.ExitCode). See $evidence" }
}
Get-ChildItem -LiteralPath $evidence -Filter '*.json' | ForEach-Object { [pscustomobject]@{Report=$_.Name;Data=(Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json)} }
