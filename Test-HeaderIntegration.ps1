[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][UInt64]$Window,
 [string]$BinaryDirectory=(Join-Path $PSScriptRoot 'build-mvp\Release'),
 [string]$Report=(Join-Path $PSScriptRoot 'evidence\header-command-overlap.json')
)
$ErrorActionPreference='Stop'
$BinaryDirectory=(Resolve-Path -LiteralPath $BinaryDirectory).ProviderPath
if(-not $Window){throw 'Select one actual Explorer folder window.'}
$Report=[IO.Path]::GetFullPath($Report)
if($Report-match '["\r\n]'){throw 'Unexpected quote or newline in report path.'}
$parent=Split-Path -Parent $Report
if($parent){New-Item -ItemType Directory -Path $parent -Force | Out-Null}
$probe=Join-Path $BinaryDirectory 'ExplorerHeaderProbe.exe'
$process=Start-Process -FilePath $probe -ArgumentList @([string]$Window,('"'+$Report+'"')) -WindowStyle Hidden -PassThru
if(-not $process.WaitForExit(30000)){
 # Only the probe this script created may be stopped; Explorer is never stopped.
 $process.Kill()
 throw 'Read-only header inspection timed out; command coexistence remains unverified.'
}
if($process.ExitCode-notin 0,1){throw "Header inspection failed ($($process.ExitCode))."}
$data=Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
$data | Select-Object window,tabsActive,ribbonCount,complete,collisionCount,noCommandOverlap
if(-not $data.tabsActive){throw 'No active tab strip was found; this is not a passing integration test.'}
if(-not $data.complete){throw 'Native command inspection was incomplete; this is not a passing integration test.'}
if(-not $data.noCommandOverlap){
 $names=@($data.nativeCommands | Where-Object collision | ForEach-Object name)
 throw ('Explorer tabs cover native commands: '+($names -join ', '))
}
