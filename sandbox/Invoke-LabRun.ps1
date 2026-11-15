<#
.SYNOPSIS
 Runs Sandbox lab stages on a fresh guest, start to finish, with no one at the
 keyboard: start the guest, run the stages, file the records, close the guest.

.DESCRIPTION
 Host side. The guest is the renderer-production lab (Renderer.wsb), whose
 logon script (Proof-Guest.ps1) polls results\request.json.

 1. Refuses to start while another Sandbox runs (it asks an earlier lab guest
    to close itself first).
 2. Moves the old contents of the results folder to results-archive\<time>, so
    no record from an earlier guest can be mistaken for this run's.
 3. Starts Windows Sandbox through WMI, so the guest is not in the job of the
    process that called this script.
 4. Waits for ready.json and checks that every guest component matches the host.
 5. Runs the whole suite (-Stages all) or only the named stages, and prints each
    stage result as it lands.
 6. Copies the results folder to -EvidenceDirectory, with lab-run.json: timings,
    stage results and the hashes of every binary and script the guest used.
 7. Asks the guest to close itself (Close-Guest.ps1) and waits until it is gone.

 Exit code: 0 all stages passed, 1 a stage failed, 2 the lab itself failed.

.EXAMPLE
 .\sandbox\Invoke-LabRun.ps1 -Stages final-combined,caption-ink -EvidenceDirectory evidence\lab-speed\try1
#>
param(
 [string[]]$Stages=@('all'),
 [Parameter(Mandatory)][string]$EvidenceDirectory,
 [int]$ReadyTimeoutSeconds=300,
 # The guest enforces each stage's own limit (up to 25 min); this is the
 # host's last line of defence if the guest stops answering altogether.
 [int]$RunTimeoutMinutes=90,
 [switch]$KeepGuest
)
$ErrorActionPreference='Stop'
$lab=Join-Path $PSScriptRoot '..\evidence\header-rebuild-20261104\renderer-production\guest-20261109'|Resolve-Path|Select-Object -ExpandProperty Path
$payload="$lab\payload";$results="$lab\results";$wsb="$lab\Renderer.wsb"
$known='setup-rehearsal','final-combined','setup-self-test','strip-switching','strip-overflow','strip-reorder','strip-drag','strip-timing','renderer-modes','caption-ink','options','footprint','starter','migration','keepalive'
$full=$Stages.Count-eq 1-and $Stages[0]-eq 'all'
if(-not $full){$unknown=@($Stages|Where-Object {$_-notin $known});if($unknown.Count){throw "Unknown stage(s): $($unknown-join ', '). Known: $($known-join ', ')"}}
$EvidenceDirectory=[IO.Path]::GetFullPath((Join-Path (Get-Location) $EvidenceDirectory))
if(Test-Path $EvidenceDirectory){throw "Evidence directory already exists: $EvidenceDirectory"}

function Say([string]$text){Write-Host ('[{0}] {1}' -f (Get-Date).ToString('HH:mm:ss'),$text)}
function Get-Guest{@(Get-Process -Name WindowsSandbox,WindowsSandboxClient -ErrorAction SilentlyContinue)}
# The guest reads request.json as soon as it appears: write it whole, then rename.
function Send-Request([string]$file){
 $temp="$results\request.tmp"
 ([ordered]@{action='script';file=$file}|ConvertTo-Json)|Set-Content $temp -Encoding UTF8
 Move-Item $temp "$results\request.json" -Force
}
function Wait-GuestGone([int]$seconds){
 $deadline=(Get-Date).AddSeconds($seconds)
 while((Get-Date)-lt $deadline){if(-not(Get-Guest).Count){return $true};Start-Sleep -Seconds 1}
 -not(Get-Guest).Count
}
function Close-LabGuest{
 if(-not(Get-Guest).Count){return 0}
 $start=Get-Date;Send-Request 'Close-Guest.ps1'
 if(Wait-GuestGone 120){return [int]((Get-Date)-$start).TotalSeconds}
 -1
}
function Get-Hashes([string]$directory,[string[]]$include){
 $map=[ordered]@{}
 Get-ChildItem $directory -File -Include $include -Recurse:$false -ErrorAction SilentlyContinue|Sort-Object Name|
  ForEach-Object {$map[$_.Name]=(Get-FileHash $_.FullName).Hash}
 $map
}

$timing=[ordered]@{started=(Get-Date).ToString('o')}
$record=[ordered]@{stages=if($full){'all'}else{$Stages};evidence=$EvidenceDirectory;timing=$timing}
$exit=2
try{
 # 1. One guest at a time.
 if((Get-Guest).Count){
  if(Test-Path "$results\ready.json"){Say 'A lab guest is still running; asking it to close.';if((Close-LabGuest)-lt 0){throw 'The running guest did not close. Close the Windows Sandbox window, then run this again.'}}
  else{throw 'Another Windows Sandbox is running. Close it, then run this again.'}
 }
 # 2. Start from an empty results folder.
 $old=@(Get-ChildItem $results -Force)
 if($old.Count){$archive="$lab\results-archive\$((Get-Date).ToString('yyyyMMdd-HHmmss'))";New-Item -ItemType Directory $archive -Force|Out-Null;$old|Move-Item -Destination $archive;Say "Moved $($old.Count) earlier result item(s) to $archive"}
 # 3. Fresh guest, outside this process's job.
 $launch=Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine="`"$env:WINDIR\System32\WindowsSandbox.exe`" `"$wsb`""}
 if($launch.ReturnValue-ne 0){throw "Windows Sandbox did not start (WMI result $($launch.ReturnValue))."}
 $launched=Get-Date;Say 'Guest starting.'
 # 4. Ready, with the exact component image.
 $ready=$null
 while(((Get-Date)-$launched).TotalSeconds-lt $ReadyTimeoutSeconds){
  if(Test-Path "$results\ready.json"){try{$ready=Get-Content "$results\ready.json" -Raw|ConvertFrom-Json;break}catch{}}
  Start-Sleep -Seconds 2
 }
 if(-not $ready){throw "The guest was not ready after $ReadyTimeoutSeconds s."}
 if(-not $ready.ready){throw "The guest failed to start the lab: $($ready.error)"}
 $mismatch=@($ready.components|Where-Object {-not $_.matchesHost})
 if($mismatch.Count){throw "Guest components differ from the host: $(($mismatch.path)-join ', '). See docs/solutions on the component gate."}
 $timing.readySeconds=[int]((Get-Date)-$launched).TotalSeconds;Say "Guest ready after $($timing.readySeconds) s; components match the host."
 # 5. Run.
 $dispatched=Get-Date
 if($full){Send-Request 'Run-U10Suite.ps1';$summaryPattern='u10-suite.json'}
 else{Set-Content "$results\rerun-stages.txt" $Stages -Encoding UTF8;Send-Request 'Run-U10Rerun.ps1';$summaryPattern='u10-suite-rerun-*.json'}
 Say "Running $(if($full){'the full suite'}else{$Stages-join ', '})."
 $summary=$null;$reported=0;$lastBeat=Get-Date
 while($true){
  if(((Get-Date)-$dispatched).TotalMinutes-gt $RunTimeoutMinutes){throw "No complete suite after $RunTimeoutMinutes min."}
  if(Test-Path "$results\request-error.json"){throw "The guest refused the request: $((Get-Content "$results\request-error.json" -Raw|ConvertFrom-Json).error)"}
  if(-not(Get-Guest).Count){throw 'The guest closed during the run.'}
  $file=Get-ChildItem $results -Filter $summaryPattern -File|Where-Object LastWriteTime -ge $dispatched.AddSeconds(-2)|Sort-Object LastWriteTime -Descending|Select-Object -First 1
  if($file){try{$summary=Get-Content $file.FullName -Raw|ConvertFrom-Json}catch{$summary=$null}}
  if($summary){
   foreach($stage in @($summary.stages)[$reported..(@($summary.stages).Count-1)]|Where-Object {$_}){
    Say ('{0,-16} {1,5} s  {2}' -f $stage.name,$stage.seconds,$(if($stage.error){"FAILED: $($stage.error)"}else{'passed'}));$reported++;$lastBeat=Get-Date
   }
   if($summary.complete){break}
  }
  if(((Get-Date)-$lastBeat).TotalSeconds-ge 60){Say "  still running ($reported stage(s) done, $([int]((Get-Date)-$dispatched).TotalMinutes) min)";$lastBeat=Get-Date}
  Start-Sleep -Seconds 3
 }
 $timing.suiteSeconds=[int]((Get-Date)-$dispatched).TotalSeconds;$timing.settle=$summary.settle
 $failed=@($summary.stages|Where-Object error)
 $record.summaryFile=$file.Name;$record.stageResults=$summary.stages
 $record.passed=-not $failed.Count-and @($summary.stages).Count-gt 0
 $exit=if($record.passed){0}else{1}
}catch{
 $record.labError=$_.Exception.Message;Say "LAB ERROR: $($_.Exception.Message)"
}finally{
 # 6. File the records, whatever happened.
 $record.guestComponents=if($ready){$ready.components}else{$null}
 $record.binaries=[ordered]@{final=Get-Hashes "$payload\final" '*.exe','*.dll';proof=Get-Hashes "$payload\proof" '*.exe','*.dll'}
 $record.scripts=Get-Hashes $payload '*.ps1','*.py'
 New-Item -ItemType Directory $EvidenceDirectory -Force|Out-Null
 & robocopy.exe $results "$EvidenceDirectory\results" /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP|Out-Null
 # 7. Close the guest.
 if(-not $KeepGuest){$timing.closeSeconds=Close-LabGuest;if($timing.closeSeconds-lt 0){Say 'The guest did not close by itself within 2 min; close the Windows Sandbox window.'}}
 $timing.finished=(Get-Date).ToString('o');$timing.totalSeconds=[int]((Get-Date)-[datetime]$timing.started).TotalSeconds
 $record.exitCode=$exit
 $record|ConvertTo-Json -Depth 8|Set-Content "$EvidenceDirectory\lab-run.json" -Encoding UTF8
 Say "Done in $([math]::Round($timing.totalSeconds/60,1)) min: $(switch($exit){0{'all stages passed'}1{'a stage failed'}default{'lab error'}}). Records: $EvidenceDirectory"
}
exit $exit
