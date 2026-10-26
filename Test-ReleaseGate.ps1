[CmdletBinding()]
param([string]$BinaryDirectory=(Join-Path $PSScriptRoot 'build-native-verified\Release'))
$ErrorActionPreference='Stop'
$report=Join-Path $PSScriptRoot 'evidence\native-v2\release-gate.json'
$fixture=Join-Path $PSScriptRoot 'evidence\native-v2\acceptance-test-fixture.json'
$checks=@()
function Expect-Rejection([string]$Name,[string]$Expected,[string]$InputReport) {
 $rejected=$false;$message=''
 try {& (Join-Path $PSScriptRoot 'Build-NativeBundle.ps1') -BinaryDirectory $BinaryDirectory -ValidateOnly -AcceptanceReport $InputReport | Out-Null}
 catch {$rejected=$true;$message=$_.Exception.Message}
 $script:checks+=@{name=$Name;passed=($rejected -and $message -like $Expected);reason=$message}
}
Expect-Rejection 'missing_acceptance_rejected' '*requires a completed acceptance report*' ''
Expect-Rejection 'untested_titlebar_rejected' '*clean22H2x64 has not passed*' (Join-Path $PSScriptRoot 'native\acceptance.template.json')
$proof=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'native\acceptance.template.json') -Raw | ConvertFrom-Json
foreach($property in $proof.PSObject.Properties){if($property.Value -is [bool]){$property.Value=$true}}
$proof | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $fixture -Encoding UTF8
Expect-Rejection 'untested_binary_rejected' '*does not match the built Explorer DLL*' $fixture
$proof.explorerDllSha256=(Get-FileHash -LiteralPath (Join-Path $BinaryDirectory 'ExplorerTabs.dll') -Algorithm SHA256).Hash
$proof.binarySha256=@{}
foreach($binary in 'ExplorerTabs.dll','NativeSnap.exe','Win10PatchOptions.exe','NativePatchSetup.exe'){$proof.binarySha256[$binary]=(Get-FileHash -LiteralPath (Join-Path $BinaryDirectory $binary) -Algorithm SHA256).Hash}
$proof.testedBuilds=@('19045.7807');$proof.matchedWorkload='Synthetic rejection fixture; no performance test has run.'
$proof.tenIdleTabsAddedPrivateMiB=21;$proof.extensionProcessingP95Ms=0;$proof.fileOperationRegressionPercent=0
$proof | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $fixture -Encoding UTF8
Expect-Rejection 'failed_memory_target_rejected' '*tenIdleTabsAddedPrivateMiB did not meet*' $fixture
$proof.tenIdleTabsAddedPrivateMiB=0
$proof | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $fixture -Encoding UTF8
Expect-Rejection 'missing_runtime_evidence_rejected' '*Attach appearance, performance, reliability and setup evidence*' $fixture
# The fixture only proves rejection; it must never resemble completed evidence.
Remove-Item -LiteralPath $fixture
$success=@($checks | Where-Object {-not $_.passed}).Count -eq 0
@{success=$success;installerCreated=$false;syntheticRejectionTests=$true;checks=$checks} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $report -Encoding UTF8
if(-not $success){throw "Release gate rejection test failed. See $report"}
Get-Content -LiteralPath $report
