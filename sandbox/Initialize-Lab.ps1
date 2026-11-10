$ErrorActionPreference='Stop'
$results='C:\NativeLabResults'
try {
 $os=Get-CimInstance Win32_OperatingSystem
 if($env:USERNAME-ne 'WDAGUtilityAccount' -or [int]$os.BuildNumber-lt 19041 -or [int]$os.BuildNumber-gt 19045){
  throw ('This lab requires a disposable Windows 10 Sandbox. Observed user={0}, computer={1}, build={2}.' -f $env:USERNAME,$env:COMPUTERNAME,$os.BuildNumber)
 }
 $local='C:\NativeLab'
 New-Item -ItemType Directory -Path $local -Force | Out-Null
 Get-ChildItem -LiteralPath 'C:\NativeLabPayload' -File |
  Where-Object {$_.Extension-in '.exe','.dll'} |
  ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $local}
 $build=Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
 if(Test-Path -LiteralPath 'C:\NativeLabPayload\qat-items.bin'){
  New-Item -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Ribbon' -Force | Out-Null
  New-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Ribbon' -Name QatItems -PropertyType Binary -Value ([IO.File]::ReadAllBytes('C:\NativeLabPayload\qat-items.bin')) -Force | Out-Null
 }
 $components=foreach($expected in (Get-Content -LiteralPath 'C:\NativeLabPayload\host-components.json' -Raw | ConvertFrom-Json)){
  $file=Get-Item -LiteralPath (Join-Path $env:WINDIR $expected.path)
  $hash=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
  [ordered]@{path=$expected.path;version=$file.VersionInfo.FileVersion;sha256=$hash;matchesHost=($hash-eq $expected.sha256)}
 }
 [ordered]@{
  ready=$true;computer=$env:COMPUTERNAME;user=$env:USERNAME
  build=[string]$os.BuildNumber;ubr=$build.UBR;version=$os.Version
  explorerVersion=(Get-Item -LiteralPath 'C:\Windows\explorer.exe').VersionInfo.FileVersion
  ribbonVersion=(Get-Item -LiteralPath 'C:\Windows\System32\UIRibbon.dll').VersionInfo.FileVersion
  time=(Get-Date).ToString('o');patchInstalled=$false;components=@($components)
 } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $results 'ready.json') -Encoding UTF8
 Start-Process -FilePath 'C:\Windows\explorer.exe' -ArgumentList 'C:\Windows'
 while($true){
  $request=Join-Path $results 'request.json'
  if(Test-Path -LiteralPath $request){
   $action=(Get-Content -LiteralPath $request -Raw | ConvertFrom-Json).action
   Remove-Item -LiteralPath $request
   try { & 'C:\NativeLabPayload\Invoke-LabRequest.ps1' -Action $action }
   catch {
    [ordered]@{action=$action;error=$_.Exception.Message;completed=(Get-Date).ToString('o')} | ConvertTo-Json |
     Set-Content -LiteralPath (Join-Path $results 'request-error.json') -Encoding UTF8
   }
  }
  Start-Sleep -Milliseconds 500
 }
} catch {
 [ordered]@{ready=$false;error=$_.Exception.Message;time=(Get-Date).ToString('o')} |
  ConvertTo-Json | Set-Content -LiteralPath (Join-Path $results 'ready.json') -Encoding UTF8
 throw
}
