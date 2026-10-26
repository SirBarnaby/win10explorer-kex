[CmdletBinding()]
param(
 [string]$BinaryDirectory=(Join-Path $PSScriptRoot 'build-native-verified\Release'),
 [string]$AcceptanceReport,
 [switch]$Release,
 [switch]$ValidateOnly
)
$ErrorActionPreference='Stop'
$required=@('clean22H2x64','thisPcCutover','titleBarOwningSta','captionDragResizeRibbon','normalMaximizedSnapped','lightDarkHighContrast','scaling100To200MixedDpi','tabNavigationStateAndFileOperations','networkVirtualSearchCloud','dragDetachAccessibilityOverflow','configurationMatrix8AndSignIn','physicalGesturesMultiMonitorSleepResume','installCancelRepairUpgradeRemoveInterrupt','startupBreakerAndEmergencyDisable','qtRetirementPreservesSnap','noIdlePolling','thousandOperationsNoContinuingGrowth')
if ($Release -or $ValidateOnly) {
 if (-not $AcceptanceReport) { throw 'Release requires a completed acceptance report with measured evidence. No installer was produced.' }
 $proof=Get-Content -LiteralPath $AcceptanceReport -Raw | ConvertFrom-Json
 if ($proof.schemaVersion -ne 1) {throw 'Unknown acceptance schema.'}
 foreach ($name in $required) {if ($proof.$name -isnot [bool] -or -not $proof.$name) {throw "Release blocked: $name has not passed."}}
 if ($proof.explorerDllSha256 -ne (Get-FileHash -LiteralPath (Join-Path $BinaryDirectory 'ExplorerTabs.dll') -Algorithm SHA256).Hash) {throw 'Acceptance does not match the built Explorer DLL.'}
 foreach ($binary in 'ExplorerTabs.dll','NativeSnap.exe','Win10PatchOptions.exe','NativePatchSetup.exe') {
  if ($proof.binarySha256.$binary -ne (Get-FileHash -LiteralPath (Join-Path $BinaryDirectory $binary) -Algorithm SHA256).Hash) {throw "Acceptance does not match $binary."}
 }
 if (-not $proof.testedBuilds -or @($proof.testedBuilds | Where-Object {$_ -notmatch '^19045\.[0-9]+$'}).Count) {throw 'Record the exact tested 19045 cumulative-update builds.'}
 if ([string]::IsNullOrWhiteSpace($proof.matchedWorkload)) {throw 'Matched performance workload is not recorded.'}
 foreach ($metric in @(@('tenIdleTabsAddedPrivateMiB',20),@('extensionProcessingP95Ms',50),@('fileOperationRegressionPercent',5))) {
  $value=$proof.($metric[0]);if ($null -eq $value -or $value -isnot [ValueType] -or $value -lt 0 -or $value -gt $metric[1]) {throw "Release blocked: $($metric[0]) did not meet its measured target."}
 }
 if (@($proof.evidenceFiles).Count -lt 4) {throw 'Attach appearance, performance, reliability and setup evidence.'}
 $evidenceRoot=[IO.Path]::GetFullPath((Split-Path -Parent (Resolve-Path -LiteralPath $AcceptanceReport).Path))
 foreach ($item in $proof.evidenceFiles) {
  $path=[IO.Path]::GetFullPath((Join-Path $evidenceRoot $item.path))
  if (-not $path.StartsWith($evidenceRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {throw 'Evidence must stay within the report folder.'}
  if ($item.sha256 -ne (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash) {throw "Evidence checksum mismatch: $($item.path)"}
 }
}
if ($ValidateOnly) {Write-Output 'Acceptance evidence is complete and matches the built binaries. No package was created.';return}
$label=if($Release){'Windows10-Native-Patch-2.0.0-x64'}else{'Windows10-Native-Patch-2.0.0-DEVELOPMENT-x64'}
$out=Join-Path $PSScriptRoot ('dist\'+$label)
if (Test-Path -LiteralPath $out) {throw "Output already exists; choose/archive the old bundle explicitly: $out"}
New-Item -ItemType Directory -Path $out -Force | Out-Null
foreach ($exe in 'NativePatchSetup.exe','Win10PatchOptions.exe','NativeSnap.exe','ExplorerTabs.dll','NativePatchModelTests.exe') {Copy-Item -LiteralPath (Join-Path $BinaryDirectory $exe) -Destination $out}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NATIVE-README.md') -Destination (Join-Path $out 'README.txt')
$license=Join-Path $PSScriptRoot 'LICENSE.native-v2.txt';if(-not(Test-Path -LiteralPath $license)){$license=Join-Path $PSScriptRoot 'LICENSE.txt'}
Copy-Item -LiteralPath $license -Destination (Join-Path $out 'LICENSE.txt')
$source=Join-Path $out 'Source'
New-Item -ItemType Directory -Path $source | Out-Null
$nativeSource=Join-Path $source 'native';New-Item -ItemType Directory -Path $nativeSource | Out-Null
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'native') | Where-Object {$_.Name -notlike 'build*'} | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $nativeSource -Recurse}
foreach ($file in 'Build-Native.ps1','Test-Native.ps1','Test-ReleaseGate.ps1','Build-NativeBundle.ps1','NATIVE-README.md','NATIVE-STATUS.md','MANUAL-TESTING.md','SANDBOX-REQUEST.md') {Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $source}
$notices=Join-Path $source 'vendor';New-Item -ItemType Directory -Path $notices | Out-Null
foreach ($file in 'AppDock.LICENSE','RectangleWin.LICENSE','PowerToys.LICENSE','WindowsTouchpadSettings.reference.cs','TouchpadWindowsCache.reference.cs') {Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('vendor\'+$file)) -Destination $notices}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vendor\rectanglewin-reference') -Destination $notices -Recurse
Copy-Item -LiteralPath (Join-Path $out 'LICENSE.txt') -Destination $source
Compress-Archive -LiteralPath $source -DestinationPath (Join-Path $out 'Source.zip') -CompressionLevel Optimal
$verification=Join-Path $out 'Verification';New-Item -ItemType Directory -Path $verification | Out-Null
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'evidence\native-v2') -File -Filter '*.json' | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $verification}
foreach($log in 'native-build-final.log','native-dependencies.txt') {Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('evidence\'+$log)) -Destination $verification}
if ($Release) {
 Copy-Item -LiteralPath $AcceptanceReport -Destination (Join-Path $out 'acceptance.json')
 $wix=Join-Path $PSScriptRoot 'tools\wix314'
 & (Join-Path $wix 'candle.exe') -nologo -arch x64 ('-dBinaryDir='+$BinaryDirectory) ('-dBundleDir='+$out) '-dAcceptance=1' -out (Join-Path $out 'NativePatch.wixobj') (Join-Path $PSScriptRoot 'native\Installer.wxs')
 if ($LASTEXITCODE) {throw 'MSI compilation failed.'}
 & (Join-Path $wix 'light.exe') -nologo -out (Join-Path $out 'Windows10NativePatch.msi') (Join-Path $out 'NativePatch.wixobj')
 if ($LASTEXITCODE) {throw 'MSI linking/validation failed.'}
} else {
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native\acceptance.template.json') -Destination (Join-Path $out 'acceptance.NOT-TESTED.json')
 'DEVELOPMENT ONLY. No MSI is included. Explorer is not registered or deployed. See NATIVE-STATUS.md in Source.' | Set-Content -LiteralPath (Join-Path $out 'NOT-A-RELEASE.txt') -Encoding UTF8
}
$manifest=Get-ChildItem -LiteralPath $out -File -Recurse | ForEach-Object { '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash,$_.FullName.Substring($out.Length+1) }
$manifest | Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Encoding ASCII
Compress-Archive -LiteralPath $out -DestinationPath ($out+'.zip') -CompressionLevel Optimal
$zipHash=Get-FileHash -LiteralPath ($out+'.zip') -Algorithm SHA256
('{0}  {1}' -f $zipHash.Hash,([IO.Path]::GetFileName($out+'.zip'))) | Set-Content -LiteralPath ($out+'.zip.sha256') -Encoding ASCII
$zipHash | Format-List
