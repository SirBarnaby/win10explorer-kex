param([ValidateSet('inspect','snap-model','snap-integration','header-experiment','header-product','header-fault','header-transient','header-sta','header-sta-proof','models')][string]$Action)
$ErrorActionPreference='Stop'
if($env:USERNAME-ne 'WDAGUtilityAccount'){throw 'Guest-only lab command.'}
if($Action-eq 'header-sta-proof'){
 Copy-Item -LiteralPath 'C:\NativeLabPayload\HeaderStaProof.exe' -Destination 'C:\NativeLab' -Force
 $process=Start-Process -FilePath 'C:\NativeLab\HeaderStaProof.exe' -ArgumentList 'C:\NativeLabResults\header-sta-proof.json' -WindowStyle Hidden -PassThru
 if(-not $process.WaitForExit(20000)){$process.Kill();throw 'STA lifecycle proof timed out.'}
 [ordered]@{exitCode=$process.ExitCode;sha256=(Get-FileHash -LiteralPath 'C:\NativeLab\HeaderStaProof.exe' -Algorithm SHA256).Hash;completed=(Get-Date).ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath 'C:\NativeLabResults\header-sta-proof-complete.json' -Encoding UTF8
 return
}
function Write-RunIdentity([string]$Directory){
 $hashes=[ordered]@{}
 Get-ChildItem -LiteralPath $Directory -File | Where-Object {$_.Extension-in '.exe','.dll'} | ForEach-Object {$hashes[$_.Name]=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
 [ordered]@{action=$Action;binarySha256=$hashes;versionQueryAdapted=($Action-in 'header-product','header-fault','header-sta');baseBuild=(Get-CimInstance Win32_OperatingSystem).BuildNumber;recorded=(Get-Date).ToString('o')} |
  ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path 'C:\NativeLabResults' ($Action+'-identity.json')) -Encoding UTF8
}
if($Action-eq 'models'){
 $run=Join-Path 'C:\NativeLab' ('models-'+(Get-Date -Format 'HHmmss'))
 New-Item -ItemType Directory -Path $run | Out-Null
 Get-ChildItem -LiteralPath 'C:\NativeLabPayload' -File | Where-Object {$_.Extension-in '.exe','.dll'-or $_.Name-in 'Test-Native.ps1','Test-Mvp.ps1'} | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $run}
 Write-RunIdentity $run
 & (Join-Path $run 'Test-Mvp.ps1') -BinaryDirectory $run | ConvertTo-Json | Set-Content -LiteralPath 'C:\NativeLabResults\models-summary.json' -Encoding UTF8
 New-Item -ItemType Directory -Path 'C:\NativeLabResults\models' -Force | Out-Null
 Get-ChildItem -LiteralPath (Join-Path $run 'evidence\native-v2') -File | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination 'C:\NativeLabResults\models' -Force}
 return
}
if($Action-in 'header-experiment','header-product','header-fault','header-transient','header-sta'){
 $ready=Get-Content -LiteralPath 'C:\NativeLabResults\ready.json' -Raw | ConvertFrom-Json
 if(@($ready.components | Where-Object {-not $_.matchesHost}).Count){throw 'Guest components differ from the exact lab image.'}
 $headerRun=Join-Path 'C:\NativeLab' ('header-'+[guid]::NewGuid().ToString('N'))
 New-Item -ItemType Directory -Path $headerRun | Out-Null
 foreach($file in 'HeaderLab.exe','HeaderLabHook.dll','ExplorerMvpHost.exe','ExplorerHeaderProbe.exe','ExplorerTabs.dll'){Copy-Item -LiteralPath (Join-Path 'C:\NativeLabPayload' $file) -Destination $headerRun}
 Write-RunIdentity $headerRun
 if($Action-in 'header-product','header-fault','header-transient','header-sta'){
  [byte[]]$config=[byte[]]::new(64)
  [BitConverter]::GetBytes([uint32]64).CopyTo($config,0);[BitConverter]::GetBytes([uint32]2).CopyTo($config,4)
  [BitConverter]::GetBytes([uint32]1).CopyTo($config,8);[BitConverter]::GetBytes([uint32]11).CopyTo($config,12)
  for($i=0;$i-lt 4;$i++){[BitConverter]::GetBytes([uint32](37+$i)).CopyTo($config,16+4*$i)}
  New-Item -Path 'HKCU:\Software\Win10NativePatch\v2' -Force | Out-Null
  New-ItemProperty -LiteralPath 'HKCU:\Software\Win10NativePatch\v2' -Name Config -PropertyType Binary -Value $config -Force | Out-Null
 }
 if($Action-eq 'header-sta'){
  Start-Process -FilePath 'C:\Windows\explorer.exe' -ArgumentList '/n,C:\Windows'
  Start-Sleep -Milliseconds 500
  Start-Process -FilePath 'C:\Windows\explorer.exe' -ArgumentList '/n,C:\Windows\System32'
  Start-Sleep -Milliseconds 1000
 }
 $probe=Start-Process -FilePath (Join-Path $headerRun 'ExplorerMvpHost.exe') -ArgumentList @('--probe','C:\NativeLabResults\explorer-baseline.json') -WindowStyle Hidden -Wait -PassThru
 $state=Get-Content -LiteralPath 'C:\NativeLabResults\explorer-baseline.json' -Raw | ConvertFrom-Json
 $window=@($state.windows)[0].hwnd
 $other=$null
 if($Action-eq 'header-sta'){
  $group=$state.windows | Group-Object pid | Where-Object {@($_.Group.thread | Select-Object -Unique).Count-ge 2} | Select-Object -First 1
  if(-not $group){throw 'Two Explorer frames in distinct STAs of one process are required.'}
  $window=$group.Group[0].hwnd;$other=@($group.Group | Where-Object {$_.thread-ne $group.Group[0].thread})[0].hwnd
 }
 if(-not $window){throw 'No guest Explorer frame.'}
 $arguments=@([string]$window,'C:\NativeLabResults');if($Action-eq 'header-product'){$arguments+='--product'}elseif($Action-eq 'header-fault'){$arguments+='--fault'}elseif($Action-eq 'header-transient'){$arguments+='--transient'}elseif($Action-eq 'header-sta'){$arguments+=@('--sta',[string]$other)}
 if($Action-eq 'header-fault'){
  $sessionKey='HKCU:\Software\Win10NativePatch\v2\ExplorerTabs'
  $priorSession=(Get-ItemProperty -LiteralPath $sessionKey -Name LastSession -ErrorAction SilentlyContinue).LastSession
  [byte[]]$sessionMarker=1..32
  New-Item -Path $sessionKey -Force | Out-Null
  New-ItemProperty -LiteralPath $sessionKey -Name LastSession -PropertyType Binary -Value $sessionMarker -Force | Out-Null
 }
 $process=Start-Process -FilePath (Join-Path $headerRun 'HeaderLab.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru
 if(-not $process.WaitForExit(20000)){$process.Kill();throw 'Header experiment timed out; dispose this guest before another test.'}
 if($Action-eq 'header-fault'){
  $afterSession=(Get-ItemProperty -LiteralPath $sessionKey -Name LastSession -ErrorAction SilentlyContinue).LastSession
  $fallback=Get-Content -LiteralPath 'C:\NativeLabResults\header-fallback.json' -Raw | ConvertFrom-Json
  $preserved=($afterSession-and [Convert]::ToBase64String([byte[]]$afterSession)-eq [Convert]::ToBase64String($sessionMarker))
  $fallback | Add-Member -NotePropertyName sessionPreserved -NotePropertyValue ([bool]$preserved)
  $fallback.success=$fallback.success-and $preserved
  $fallback | ConvertTo-Json | Set-Content -LiteralPath 'C:\NativeLabResults\header-fallback.json' -Encoding UTF8
  if($priorSession){New-ItemProperty -LiteralPath $sessionKey -Name LastSession -PropertyType Binary -Value $priorSession -Force | Out-Null}else{Remove-ItemProperty -LiteralPath $sessionKey -Name LastSession -ErrorAction SilentlyContinue}
 }
 [ordered]@{action=$Action;exitCode=$process.ExitCode;completed=(Get-Date).ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath 'C:\NativeLabResults\header-complete.json' -Encoding UTF8
 return
}
$binary=if($Action-eq 'inspect'){'ExplorerMvpHost.exe'}elseif($Action-eq 'snap-integration'){'SnapIntegrationTests.exe'}else{'NativeSnap.exe'}
if($Action-eq 'snap-integration'){Copy-Item -LiteralPath 'C:\NativeLabPayload\NativeSnap.exe' -Destination 'C:\NativeLab' -Force}
Copy-Item -LiteralPath (Join-Path 'C:\NativeLabPayload' $binary) -Destination 'C:\NativeLab' -Force
Write-RunIdentity 'C:\NativeLab'
$report=if($Action-eq 'inspect'){'explorer-baseline.json'}elseif($Action-eq 'snap-integration'){'snap-integration.json'}else{'snap-model.json'}
$verb=if($Action-eq 'inspect'){'--probe'}else{'--self-test'}
$arguments=if($Action-eq 'snap-integration'){@((Join-Path 'C:\NativeLabResults' $report))}else{@($verb,(Join-Path 'C:\NativeLabResults' $report))}
if($Action-eq 'snap-integration'){
 $settings='HKCU:\Software\Win10NativePatch\v2'
 $before=(Get-ItemProperty -LiteralPath $settings -Name Config -ErrorAction SilentlyContinue).Config
 [byte[]]$config=[byte[]]::new(64)
 [BitConverter]::GetBytes([uint32]64).CopyTo($config,0);[BitConverter]::GetBytes([uint32]2).CopyTo($config,4)
 [BitConverter]::GetBytes([uint32]4).CopyTo($config,8);[BitConverter]::GetBytes([uint32]11).CopyTo($config,12)
 for($i=0;$i-lt 4;$i++){[BitConverter]::GetBytes([uint32](37+$i)).CopyTo($config,16+4*$i)}
 New-Item -Path $settings -Force | Out-Null
 New-ItemProperty -LiteralPath $settings -Name Config -PropertyType Binary -Value $config -Force | Out-Null
 $receiver=Start-Process -FilePath 'C:\NativeLab\NativeSnap.exe' -WindowStyle Hidden -PassThru
 Start-Sleep -Milliseconds 500
}
try {
$process=Start-Process -FilePath (Join-Path 'C:\NativeLab' $binary) -ArgumentList $arguments -WindowStyle Hidden -PassThru
if(-not $process.WaitForExit(60000)){$process.Kill();throw 'The guest diagnostic timed out.'}
[ordered]@{action=$Action;exitCode=$process.ExitCode;completed=(Get-Date).ToString('o')} | ConvertTo-Json |
 Set-Content -LiteralPath (Join-Path 'C:\NativeLabResults' ($Action+'-complete.json')) -Encoding UTF8
} finally {
 if($Action-eq 'snap-integration'){
  Start-Process -FilePath 'C:\NativeLab\NativeSnap.exe' -ArgumentList '--stop' -WindowStyle Hidden -Wait | Out-Null
  if($before){New-ItemProperty -LiteralPath $settings -Name Config -PropertyType Binary -Value $before -Force | Out-Null}
  else{Remove-ItemProperty -LiteralPath $settings -Name Config -ErrorAction SilentlyContinue}
 }
}
