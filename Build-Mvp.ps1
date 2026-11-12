[CmdletBinding()]
param(
 [string]$BinaryDirectory=(Join-Path $PSScriptRoot 'build-mvp\Release'),
 [string]$SetupBuildDirectory=(Join-Path $PSScriptRoot 'build-mvp-setup'),
 [switch]$SkipBuild,
 # Builds only the single-file ExplorerMvpSetup.exe from the current binaries,
 # for testing before acceptance. Packaging (the default) never rebuilds it.
 [switch]$StageSetup,
 [string]$HeaderAcceptance=(Join-Path $PSScriptRoot 'evidence\native-v2\header-acceptance.json')
)
$ErrorActionPreference='Stop'
$BinaryDirectory=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BinaryDirectory)
$SetupBuildDirectory=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($SetupBuildDirectory)
$payloadDirectory=Join-Path $SetupBuildDirectory 'payload-files'
$singleSetup=Join-Path $SetupBuildDirectory 'Release\ExplorerMvpSetup.exe'
# Embedded in the setup in this order; the setup itself is the eighth package file.
$embedded=@('ExplorerTabs.dll','ExplorerMvpHost.exe','NativeSnap.exe','Win10PatchOptions.exe','Source.zip','LICENSE.txt','README.txt')
$binaries=@('ExplorerTabs.dll','ExplorerMvpHost.exe','NativeSnap.exe','Win10PatchOptions.exe')

function New-SourceZip([string]$Destination){
 $work=Join-Path ([IO.Path]::GetTempPath()) ('ExplorerMvpSource-'+[guid]::NewGuid().ToString('N'))
 $source=Join-Path $work 'Source'
 New-Item -ItemType Directory -Path $source | Out-Null
 try{
  foreach($file in 'Build-Native.ps1','Test-Native.ps1','Build-Mvp.ps1','Test-Mvp.ps1','Test-MvpExplorer.ps1','Test-HeaderIntegration.ps1','MVP-README.md','MVP-STATUS.md','LICENSE.native-v2.txt'){
   Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $source
  }
  $native=Join-Path $source 'native';New-Item -ItemType Directory -Path $native | Out-Null
  Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'native') -File | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $native}
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native\vendor') -Destination $native -Recurse
  $legacy=Join-Path $source 'src';New-Item -ItemType Directory -Path $legacy | Out-Null
  foreach($file in 'NativeSnap.ico','NativeSnapPaused.ico'){Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('src\'+$file)) -Destination $legacy}
  $notices=Join-Path $source 'vendor';New-Item -ItemType Directory -Path $notices | Out-Null
  foreach($file in 'AppDock.LICENSE','RectangleWin.LICENSE','PowerToys.LICENSE','WindowsTouchpadSettings.reference.cs','TouchpadWindowsCache.reference.cs'){
   Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('vendor\'+$file)) -Destination $notices
  }
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vendor\rectanglewin-reference') -Destination $notices -Recurse
  Compress-Archive -LiteralPath $source -DestinationPath $Destination -CompressionLevel Optimal
 }finally{Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue}
}

if($StageSetup){
 if(-not $SkipBuild){& (Join-Path $PSScriptRoot 'Build-Native.ps1') -BuildDirectory (Split-Path -Parent $BinaryDirectory)}
 foreach($file in $binaries){if(-not(Test-Path -LiteralPath (Join-Path $BinaryDirectory $file) -PathType Leaf)){throw "Missing MVP binary: $file"}}
 if(Test-Path -LiteralPath $payloadDirectory){Remove-Item -LiteralPath $payloadDirectory -Recurse -Force}
 New-Item -ItemType Directory -Path $payloadDirectory | Out-Null
 foreach($file in $binaries){Copy-Item -LiteralPath (Join-Path $BinaryDirectory $file) -Destination $payloadDirectory}
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'MVP-README.md') -Destination (Join-Path $payloadDirectory 'README.txt')
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE.native-v2.txt') -Destination (Join-Path $payloadDirectory 'LICENSE.txt')
 New-SourceZip (Join-Path $payloadDirectory 'Source.zip')
 $cmake=(Get-Command cmake -ErrorAction SilentlyContinue).Source;if(-not $cmake){$cmake='C:\mingw64\bin\cmake.exe'}
 & $cmake -S (Join-Path $PSScriptRoot 'native') -B $SetupBuildDirectory -G 'Visual Studio 17 2022' -A x64 ('-DMVP_PAYLOAD_DIR='+($payloadDirectory -replace '\\','/')) | Out-Null
 if($LASTEXITCODE){throw 'Configuring the single-file setup failed.'}
 & $cmake --build $SetupBuildDirectory --config Release --target ExplorerMvpSetup | Out-Null
 if($LASTEXITCODE -or -not(Test-Path -LiteralPath $singleSetup)){throw 'Building the single-file setup failed.'}
 Write-Output $singleSetup
 return
}

if(-not(Test-Path -LiteralPath $HeaderAcceptance -PathType Leaf)){throw 'Complete the native header acceptance checks before packaging.'}
$header=Get-Content -LiteralPath $HeaderAcceptance -Raw | ConvertFrom-Json
if(-not $header.success){throw 'Native header acceptance has not passed.'}
foreach($gate in 'nativeContent','physicalInput','material','accessibility','lifecycle','snapping','combinedExplorerSnapping','setupRehearsal','switching','overflow','motion','colours','footprint','singleFileSetup'){
 if($header.checks.$gate -ne $true){throw "Native acceptance is incomplete: $gate"}
}
if($header.review.status -ne 'complete'){throw 'The completed code review receipt is required.'}
if(-not(Test-Path -LiteralPath $singleSetup -PathType Leaf)){throw 'Build the single-file setup first: Build-Mvp.ps1 -StageSetup.'}
# The setup embeds the payload; packaging uses exactly the tested files.
$shipped=[ordered]@{}
foreach($file in $binaries){$path=Join-Path $BinaryDirectory $file;if(-not(Test-Path -LiteralPath $path -PathType Leaf)){throw "Missing MVP binary: $file"};$shipped[$file]=$path}
$shipped['ExplorerMvpSetup.exe']=$singleSetup
foreach($file in $embedded){
 $staged=Join-Path $payloadDirectory $file;if(-not(Test-Path -LiteralPath $staged -PathType Leaf)){throw "The staged payload is incomplete: $file"}
 if($shipped.Contains($file)-and (Get-FileHash -LiteralPath $staged).Hash-ne (Get-FileHash -LiteralPath $shipped[$file]).Hash){throw "The single-file setup embeds a different $file. Run Build-Mvp.ps1 -StageSetup again."}
}
foreach($report in 'snap.json','split-layout.json','config.json','explorer-model.json','migration.json','mvp-setup.json'){
 $path=Join-Path $PSScriptRoot ('evidence\native-v2\'+$report)
 if(-not(Test-Path -LiteralPath $path)){throw "Run the MVP checks first: $report is missing."}
 $data=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
 if(-not $data.success){throw "MVP verification failed: $report"}
}
$tested=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'evidence\native-v2\mvp-tested-binaries.json') -Raw | ConvertFrom-Json
foreach($file in $shipped.Keys){if($tested.binarySha256.$file-ne (Get-FileHash -LiteralPath $shipped[$file] -Algorithm SHA256).Hash){throw "Tests do not match the current binary: $file"}}
foreach($file in $shipped.Keys){if($header.binarySha256.$file-ne (Get-FileHash -LiteralPath $shipped[$file] -Algorithm SHA256).Hash){throw "Native acceptance does not match the current binary: $file"}}
if(-not $header.sourceSha256 -or -not @($header.evidence).Count){throw 'Native acceptance source and evidence inventories are required.'}
foreach($entry in $header.sourceSha256.PSObject.Properties){
 if($entry.Value-ne (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $entry.Name) -Algorithm SHA256).Hash){throw "Source changed after native acceptance: $($entry.Name)"}
}
foreach($entry in $header.evidence){
 if($entry.sha256-ne (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $entry.path) -Algorithm SHA256).Hash){throw "Native evidence changed: $($entry.path)"}
}
$label='Explorer-MVP-0.1.0-x64-'+(Get-Date -Format 'yyyyMMdd-HHmmss')
$out=Join-Path $PSScriptRoot ('dist\'+$label)
New-Item -ItemType Directory -Path $out | Out-Null
# One file to install; the corresponding source beside it.
Copy-Item -LiteralPath $singleSetup -Destination $out
Copy-Item -LiteralPath (Join-Path $payloadDirectory 'Source.zip') -Destination $out
$verification=Join-Path $out 'Verification';New-Item -ItemType Directory -Path $verification | Out-Null
Copy-Item -LiteralPath $HeaderAcceptance -Destination (Join-Path $verification 'header-acceptance.json')
foreach($entry in $header.evidence){
 $destination=Join-Path $verification $entry.path
 New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot $entry.path) -Destination $destination
}
foreach($report in 'snap.json','split-layout.json','config.json','explorer-model.json','migration.json','mvp-setup.json','mvp-tested-binaries.json'){
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('evidence\native-v2\'+$report)) -Destination $verification
}
$inventory=foreach($file in $shipped.Keys){[ordered]@{name=$file;sha256=(Get-FileHash -LiteralPath $shipped[$file] -Algorithm SHA256).Hash;embedded=$file-ne 'ExplorerMvpSetup.exe'}}
[ordered]@{product='Explorer MVP';version='0.1.0';target='Windows 10 22H2 x64';runtime='Explorer child controls plus hidden starter and snap receiver';setup='ExplorerMvpSetup.exe (single file, payload embedded with SHA-256)';binaries=$inventory;physicalGesturesVerified=$false;cleanPcVerified=$false;fullyNativeSnapVerified=$false} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'MVP.json') -Encoding UTF8
Get-ChildItem -LiteralPath $out -File -Recurse | ForEach-Object {'{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash,$_.FullName.Substring($out.Length+1)} | Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Encoding ASCII
Compress-Archive -LiteralPath $out -DestinationPath ($out+'.zip') -CompressionLevel Optimal
'{0}  {1}' -f (Get-FileHash -LiteralPath ($out+'.zip') -Algorithm SHA256).Hash,([IO.Path]::GetFileName($out+'.zip')) | Set-Content -LiteralPath ($out+'.zip.sha256') -Encoding ASCII
Write-Output $out
