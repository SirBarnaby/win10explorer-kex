[CmdletBinding()]
param(
 [string]$BinaryDirectory=(Join-Path $PSScriptRoot 'build-mvp\Release'),
 # The single-file setup that ships (Build-Mvp.ps1 -StageSetup); its self-test
 # also verifies every embedded payload file against its SHA-256.
 [string]$SetupPath=(Join-Path $PSScriptRoot 'build-mvp-setup\Release\ExplorerMvpSetup.exe')
)
$ErrorActionPreference='Stop'
$BinaryDirectory=(Resolve-Path -LiteralPath $BinaryDirectory).ProviderPath
if(-not(Test-Path -LiteralPath $SetupPath -PathType Leaf)){$SetupPath=Join-Path $BinaryDirectory 'ExplorerMvpSetup.exe'}
$SetupPath=(Resolve-Path -LiteralPath $SetupPath).ProviderPath
& (Join-Path $PSScriptRoot 'Test-Native.ps1') -BinaryDirectory $BinaryDirectory | Out-Null
$evidence=Join-Path $PSScriptRoot 'evidence\native-v2'
$report=Join-Path $evidence 'mvp-setup.json'
$process=Start-Process -FilePath $SetupPath -ArgumentList @('--self-test',('"'+$report+'"')) -WindowStyle Hidden -Wait -PassThru
if($process.ExitCode){throw 'The MVP setup checks failed.'}
# The setup's support check pins the same component images as the runtime
# (native/HeaderIntegration.cpp). Drift would let setup install where tabs cannot load.
$source=Join-Path $PSScriptRoot 'native'
function Get-PinnedBytes([string]$File,[string]$Pattern){
 $match=[regex]::Match((Get-Content -LiteralPath (Join-Path $source $File) -Raw),$Pattern)
 if(-not $match.Success){throw "The pinned hash is missing from ${File}: $Pattern"}
 ($match.Groups[1].Value -replace '\s','').ToLowerInvariant()
}
foreach($pin in @(@('RibbonImage','ribbonHash'),@('ThemeImage','themeHash'))){
 $setupPin=Get-PinnedBytes 'MvpSetup.cpp' ('constexpr Sha256 '+$pin[0]+'\{([^}]*)\}')
 $runtimePin=Get-PinnedBytes 'HeaderIntegration.cpp' ('constexpr BYTE '+$pin[1]+'\[\]=\{([^}]*)\}')
 if($setupPin-ne $runtimePin){throw "Setup $($pin[0]) differs from the runtime $($pin[1])."}
}
# One flipped byte inside an embedded file must fail the setup's own integrity
# check. The licence text is embedded verbatim, so it is easy to find.
$latin=[Text.Encoding]::GetEncoding(28591)
$license=[IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'LICENSE'))
$image=[IO.File]::ReadAllBytes($SetupPath)
$at=$latin.GetString($image).IndexOf($latin.GetString($license,0,[Math]::Min(256,$license.Length)),[StringComparison]::Ordinal)
$tamperRejected=$false
if($at-ge 0){
 $work=Join-Path ([IO.Path]::GetTempPath()) ('mvp-tamper-'+[guid]::NewGuid().ToString('N'))
 New-Item -ItemType Directory -Path $work | Out-Null
 try{
  $image[$at+16]=$image[$at+16] -bxor 0x20
  $copy=Join-Path $work 'ExplorerMvpSetup.exe';[IO.File]::WriteAllBytes($copy,$image)
  $tamperReport=Join-Path $work 'tampered.json'
  $tampered=Start-Process -FilePath $copy -ArgumentList @('--self-test',('"'+$tamperReport+'"')) -WindowStyle Hidden -Wait -PassThru
  $failed=@((Get-Content -LiteralPath $tamperReport -Raw | ConvertFrom-Json).failed) -join ','
  if($tampered.ExitCode-eq 0 -or $failed-ne 'embedded-payload-intact'){throw "The setup did not reject a tampered embedded file (exit $($tampered.ExitCode), failed: $failed)."}
  $tamperRejected=$true
 } finally {Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue}
}
$reject=Start-Process -FilePath (Join-Path $BinaryDirectory 'Win10PatchOptions.exe') -ArgumentList @('--owner','0') -WindowStyle Hidden -Wait -PassThru
if($reject.ExitCode-ne 2){throw 'Options did not reject an invalid Explorer owner.'}
$reject=Start-Process -FilePath (Join-Path $BinaryDirectory 'ExplorerMvpHost.exe') -ArgumentList @('--window','0') -WindowStyle Hidden -Wait -PassThru
if($reject.ExitCode-ne 2){throw 'The starter did not reject an invalid Explorer window.'}
$files=@('ExplorerTabs.dll','ExplorerMvpHost.exe','NativeSnap.exe','Win10PatchOptions.exe','ExplorerMvpSetup.exe')
$hashes=[ordered]@{}
foreach($file in $files){$path=if($file-eq 'ExplorerMvpSetup.exe'){$SetupPath}else{Join-Path $BinaryDirectory $file};$hashes[$file]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
[ordered]@{success=$true;binarySha256=$hashes;invalidTargetsRejected=$true;componentPinsMatch=$true;tamperedPayloadRejected=$tamperRejected;physicalGesturesTested=$false;liveSettingsModified=$false} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidence 'mvp-tested-binaries.json') -Encoding UTF8
foreach($name in 'snap.json','split-layout.json','config.json','explorer-model.json','migration.json','mvp-setup.json'){
 $data=Get-Content -LiteralPath (Join-Path $evidence $name) -Raw | ConvertFrom-Json
 [pscustomobject]@{Report=$name;Success=$data.success;Checks=if($data.total){$data.total}else{@($data.checks).Count}}
}
