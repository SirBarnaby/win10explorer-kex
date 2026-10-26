[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][UInt64]$Window,
 [string]$BinaryDirectory=(Join-Path $PSScriptRoot 'build-mvp\Release'),
 [switch]$KeepOpen
)
$ErrorActionPreference='Stop'
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$lab=Join-Path $PSScriptRoot ('evidence\mvp-explorer\'+$stamp)
New-Item -ItemType Directory -Path $lab -Force | Out-Null
foreach($name in 'ExplorerTabs.dll','ExplorerMvpHost.exe','Win10PatchOptions.exe','NativeSnap.exe') {
 Copy-Item -LiteralPath (Join-Path $BinaryDirectory $name) -Destination $lab
}
$key='HKCU:\Software\Win10NativePatch\v2'
$record=Get-ItemProperty -LiteralPath $key -Name Config -ErrorAction SilentlyContinue
$before=if($record){[byte[]]$record.Config}else{$null}
@{existed=($null-ne $before);configBase64=if($before){[Convert]::ToBase64String($before)}else{''}} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $lab 'settings-before.json') -Encoding UTF8
if($before -and $before.Length-ne 64){throw 'The existing settings format is unexpected.'}
[byte[]]$test=if($before){$before.Clone()}else{[byte[]]::new(64)}
if(-not $before){[BitConverter]::GetBytes([uint32]64).CopyTo($test,0);[BitConverter]::GetBytes([uint32]2).CopyTo($test,4);[BitConverter]::GetBytes([uint32]11).CopyTo($test,12);$keys=@(37,38,39,40);for($i=0;$i-lt 4;$i++){[BitConverter]::GetBytes([uint32]$keys[$i]).CopyTo($test,16+$i*4)}}
[BitConverter]::GetBytes([uint32]1).CopyTo($test,8)
$hostPath=Join-Path $lab 'ExplorerMvpHost.exe'
if(Get-Process ExplorerMvpHost -ErrorAction SilentlyContinue){throw 'An Explorer MVP starter is already running. Close its test first.'}
$started=$false
try {
 New-Item -Path $key -Force | Out-Null
 New-ItemProperty -LiteralPath $key -Name Config -PropertyType Binary -Value $test -Force | Out-Null
 Start-Process -FilePath $hostPath -ArgumentList @('--window',[string]$Window) -WindowStyle Hidden | Out-Null
 $started=$true
 Start-Sleep -Milliseconds 2500
 $report=Join-Path $lab 'explorer.json'
 $p=Start-Process -FilePath $hostPath -ArgumentList @('--probe',('"'+$report+'"')) -WindowStyle Hidden -Wait -PassThru
 if($p.ExitCode){throw 'Explorer inspection failed.'}
 $state=Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
 $target=@($state.windows | Where-Object {$_.hwnd-eq $Window})
 $tabs=if($target.Count-eq 1){@($target[0].children | Where-Object {$_.class-eq 'Win10NativePatch.ExplorerTabs.v2'})}else{@()}
 $sameFrame=$tabs.Count-eq 1-and $tabs[0].parent-eq $Window
 $topRow=$sameFrame-and $tabs[0].rect[1]-ge $target[0].outer[1]-and $tabs[0].rect[3]-le ($target[0].outer[1]+$target[0].captionButtons[3]+2)
 $result=[ordered]@{success=($target.Count-eq 1-and $target[0].tabsActive-and $sameFrame-and $topRow);window=$Window;shellPid=$state.shellPid;explorerPid=if($target.Count){$target[0].pid}else{0};realExplorerChild=$sameFrame;captionRow=$topRow;attachResult=if($target.Count){$target[0].attachResult}else{0};rejected=if($target.Count){$target[0].rejected}else{0};physicalGesturesTested=$false;visualAppearanceVerified=$false;dllSha256=(Get-FileHash -LiteralPath (Join-Path $lab 'ExplorerTabs.dll')).Hash;lab=$lab}
 $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $lab 'result.json') -Encoding UTF8
 $result | ConvertTo-Json
 if(-not $result.success){throw 'The real Explorer title-bar check did not pass. See the recorded result.'}
} finally {
 if(-not $KeepOpen -or -not $result.success){
  if($started){Start-Process -FilePath $hostPath -ArgumentList '--stop' -WindowStyle Hidden -Wait | Out-Null}
  $current=(Get-ItemProperty -LiteralPath $key -Name Config -ErrorAction SilentlyContinue).Config
  if($current-and [Convert]::ToBase64String($current)-eq [Convert]::ToBase64String($test)){
   if($before){New-ItemProperty -LiteralPath $key -Name Config -PropertyType Binary -Value $before -Force | Out-Null}
   else{Remove-ItemProperty -LiteralPath $key -Name Config -ErrorAction SilentlyContinue}
  }
 }
}
