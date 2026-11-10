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
$reject=Start-Process -FilePath (Join-Path $BinaryDirectory 'Win10PatchOptions.exe') -ArgumentList @('--owner','0') -WindowStyle Hidden -Wait -PassThru
if($reject.ExitCode-ne 2){throw 'Options did not reject an invalid Explorer owner.'}
$reject=Start-Process -FilePath (Join-Path $BinaryDirectory 'ExplorerMvpHost.exe') -ArgumentList @('--window','0') -WindowStyle Hidden -Wait -PassThru
if($reject.ExitCode-ne 2){throw 'The starter did not reject an invalid Explorer window.'}
$files=@('ExplorerTabs.dll','ExplorerMvpHost.exe','NativeSnap.exe','Win10PatchOptions.exe','ExplorerMvpSetup.exe')
$hashes=[ordered]@{}
foreach($file in $files){$path=if($file-eq 'ExplorerMvpSetup.exe'){$SetupPath}else{Join-Path $BinaryDirectory $file};$hashes[$file]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
[ordered]@{success=$true;binarySha256=$hashes;invalidTargetsRejected=$true;physicalGesturesTested=$false;liveSettingsModified=$false} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidence 'mvp-tested-binaries.json') -Encoding UTF8
foreach($name in 'snap.json','split-layout.json','config.json','explorer-model.json','migration.json','mvp-setup.json'){
 $data=Get-Content -LiteralPath (Join-Path $evidence $name) -Raw | ConvertFrom-Json
 [pscustomobject]@{Report=$name;Success=$data.success;Checks=if($data.total){$data.total}else{@($data.checks).Count}}
}
