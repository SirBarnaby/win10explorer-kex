[CmdletBinding()]
param([string]$BinaryDirectory=(Join-Path $PSScriptRoot 'build-mvp\Release'))
$ErrorActionPreference='Stop'
$sandbox=Join-Path $env:WINDIR 'System32\WindowsSandbox.exe'
if(-not(Test-Path -LiteralPath $sandbox)){throw 'Windows Sandbox is not available. Complete the Windows feature restart first.'}
$session=Join-Path $PSScriptRoot ('evidence\sandbox\'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$payload=Join-Path $session 'payload'
$results=Join-Path $session 'results'
New-Item -ItemType Directory -Path $payload,$results | Out-Null
foreach($name in 'ExplorerTabs.dll','ExplorerMvpHost.exe','ExplorerHeaderProbe.exe','NativeSnap.exe','Win10PatchOptions.exe'){
 Copy-Item -LiteralPath (Join-Path $BinaryDirectory $name) -Destination $payload
}
foreach($optional in 'SnapIntegrationTests.exe','HeaderLab.exe','HeaderLabHook.dll','SnapLayoutTests.exe','NativePatchModelTests.exe','NativePatchSetup.exe','ExplorerMvpSetup.exe'){
 if(Test-Path -LiteralPath (Join-Path $BinaryDirectory $optional)){Copy-Item -LiteralPath (Join-Path $BinaryDirectory $optional) -Destination $payload}
}
foreach($script in 'Test-Native.ps1','Test-Mvp.ps1'){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $payload}
$baseline=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'evidence\header-installation-baseline-20261102.json') -Raw | ConvertFrom-Json
[IO.File]::WriteAllBytes((Join-Path $payload 'qat-items.bin'),[Convert]::FromBase64String($baseline.qatItemsBase64))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'sandbox\Initialize-Lab.ps1') -Destination $payload
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'sandbox\Invoke-LabRequest.ps1') -Destination $payload
$components=@('explorer.exe','System32\ExplorerFrame.dll','System32\UIRibbon.dll','System32\user32.dll','System32\win32u.dll','System32\twinui.pcshell.dll')
$hostComponents=foreach($relative in $components){
 $file=Get-Item -LiteralPath (Join-Path $env:WINDIR $relative)
 [ordered]@{path=$relative;version=$file.VersionInfo.FileVersion;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
}
$hostComponents | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $payload 'host-components.json') -Encoding UTF8
$files=@(Get-ChildItem -LiteralPath $payload -File | ForEach-Object {
 [ordered]@{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
[ordered]@{created=(Get-Date).ToString('o');payload=$payload;results=$results;files=$files} |
 ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $session 'manifest.json') -Encoding UTF8
$payloadXml=[Security.SecurityElement]::Escape($payload)
$resultsXml=[Security.SecurityElement]::Escape($results)
$configuration=@"
<Configuration>
 <Networking>Disable</Networking>
 <ClipboardRedirection>Disable</ClipboardRedirection>
 <AudioInput>Disable</AudioInput>
 <VideoInput>Disable</VideoInput>
 <PrinterRedirection>Disable</PrinterRedirection>
 <MemoryInMB>4096</MemoryInMB>
 <MappedFolders>
  <MappedFolder><HostFolder>$payloadXml</HostFolder><SandboxFolder>C:\NativeLabPayload</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
  <MappedFolder><HostFolder>$resultsXml</HostFolder><SandboxFolder>C:\NativeLabResults</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
 </MappedFolders>
 <LogonCommand><Command>powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File C:\NativeLabPayload\Initialize-Lab.ps1</Command></LogonCommand>
</Configuration>
"@
$configPath=Join-Path $session 'NativeLayout.wsb'
$configuration | Set-Content -LiteralPath $configPath -Encoding UTF8
$process=Start-Process -FilePath $sandbox -ArgumentList ('"'+$configPath+'"') -PassThru
[ordered]@{session=$session;processId=$process.Id;configuration=$configPath} | ConvertTo-Json
