[CmdletBinding()]
param(
 # The Windows Sandbox results folder the lab guest writes to (C:\NativeLabResults).
 [string]$Results=(Join-Path $PSScriptRoot 'evidence\header-rebuild-20261104\renderer-production\guest-final-20261110\results'),
 # Where the guest outputs this receipt relies on are copied under stable names.
 [string]$EvidenceDirectory=(Join-Path $PSScriptRoot 'evidence\refine-20261110\u10\guest'),
 [string]$BinaryDirectory=(Join-Path $PSScriptRoot 'build-mvp\Release'),
 [string]$SetupPath=(Join-Path $PSScriptRoot 'build-mvp-setup\Release\ExplorerMvpSetup.exe'),
 [string]$Review=(Join-Path $PSScriptRoot 'evidence\refine-20261110\u10\review.json'),
 [string]$Out=(Join-Path $PSScriptRoot 'evidence\native-v2\header-acceptance.json'),
 [string]$Label='u10'
)
# Writes the native acceptance receipt Build-Mvp.ps1 packages against. Every
# check is computed from the guest's own result files; every shipped binary must
# be the one the guest tested; sources and evidence are hash-bound. Exits 1 and
# writes success=false when anything is missing or failed.
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
function Relative([string]$Path){(Resolve-Path -LiteralPath $Path).ProviderPath.Substring($root.Length+1)}
function Read-Json([string]$Path){Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json}
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}

# Guest outputs, copied once under names that later runs cannot overwrite.
$sources=[ordered]@{
 'final-combined-run.json'="$Results\final-combined-run.json"
 'final-checkpoints.json'="$Results\final-checkpoints.json"
 'snap-generic.json'="$Results\snap-generic.json"
 'snap-explorer-pair.json'="$Results\snap-explorer-pair.json"
 'snap-pair-native-content-0.json'="$Results\snap-pair-native-content-0.json"
 'snap-pair-native-content-1.json'="$Results\snap-pair-native-content-1.json"
 'setup-rehearsal.json'="$Results\setup-rehearsal.json"
 'renderer-faults.json'="$Results\refine\refine-$Label\renderer-faults.json"
 'u10-suite.json'="$Results\u10-suite.json"
 'setup-self-test.json'="$Results\refine\refine-$Label\setup-self-test.json"
 'renderer-modes-run.json'="$Results\refine\refine-$Label\renderer-modes-run.json"
 'caption-ink-run.json'="$Results\refine\refine-$Label\caption-ink-run.json"
 'options.json'="$Results\refine\refine-${Label}host\options.json"
 'starter.json'="$Results\refine\refine-${Label}host\starter.json"
 'footprint.json'="$Results\refine\footprint-$Label.json"
}
foreach($mode in 'switching','overflow','reorder','drag','timing'){
 $tag=$Label+$mode.Substring(0,1)
 $sources["run-$mode.json"]="$Results\refine\run-$tag-$mode.json"
 $sources["behavior-$mode.json"]="$Results\refine\behavior-$mode.json"
 $sources["timing-$mode.log"]="$Results\refine\timing-$tag.log"
}
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
# Re-runs are optional; each one has its own record (u10-suite-rerun-<time>.json).
$reruns=@(Get-ChildItem -LiteralPath $Results -Filter 'u10-suite-rerun-*.json' -File | Sort-Object Name | ForEach-Object Name)
foreach($name in $reruns){$sources[$name]=Join-Path $Results $name}
$missing=@($sources.Values | Where-Object {-not(Test-Path -LiteralPath $_ -PathType Leaf)})
if($missing.Count){throw "Guest results are missing:`n"+($missing -join "`n")}
foreach($name in $sources.Keys){Copy-Item -LiteralPath $sources[$name] -Destination (Join-Path $EvidenceDirectory $name) -Force}
$g=@{};foreach($name in $sources.Keys){if($name -like '*.json'){$g[$name]=Read-Json (Join-Path $EvidenceDirectory $name)}}
$suite=$g['u10-suite.json']
$point=@{};foreach($c in @($g['final-checkpoints.json'])){$point[$c.name]=$c.exitCode}
function Passed([string[]]$names){foreach($n in $names){if(-not $point.ContainsKey($n) -or $point[$n] -ne 0){return $false}};$true}
function RunOk([string]$mode){$r=$g["run-$mode.json"];$b=$g["behavior-$mode.json"];(-not $r.timedOut) -and $r.exitCode -eq 0 -and $b.success -eq $true -and -not @($b.checks | Where-Object {-not $_.passed}).Count}
function ChecksOf([string]$mode){$b=$g["behavior-$mode.json"];'{0}/{1}' -f @($b.checks | Where-Object passed).Count,@($b.checks).Count}

# The binaries shipped are exactly the binaries the guest exercised.
$shipped=[ordered]@{}
foreach($file in 'ExplorerTabs.dll','ExplorerMvpHost.exe','NativeSnap.exe','Win10PatchOptions.exe'){$shipped[$file]=Hash (Join-Path $BinaryDirectory $file)}
$shipped['ExplorerMvpSetup.exe']=Hash $SetupPath
$tested=@{};foreach($b in @($g['final-combined-run.json'].releaseHashes)){$tested[$b.name]=$b.sha256}
$mismatch=@($shipped.Keys | Where-Object {$tested[$_] -ne $shipped[$_]})
foreach($name in 'run-switching.json','run-overflow.json','run-reorder.json','run-drag.json','run-timing.json','renderer-modes-run.json','caption-ink-run.json'){
 $dll=@($g[$name].binaries | Where-Object name -eq 'ExplorerTabs.dll')[0]
 if(-not $dll -or $dll.sha256 -ne $shipped['ExplorerTabs.dll']){$mismatch+="ExplorerTabs.dll in $name"}
}
$hostTests=Read-Json (Join-Path $root 'evidence\native-v2\mvp-tested-binaries.json')
$hostSetup=Read-Json (Join-Path $root 'evidence\native-v2\mvp-setup.json')
foreach($file in $shipped.Keys){if($hostTests.binarySha256.$file -ne $shipped[$file]){$mismatch+="$file in host tests"}}
if($mismatch.Count){throw "Tested and shipped binaries differ: $($mismatch -join ', ')"}

$modes=$g['renderer-modes-run.json'];$ink=$g['caption-ink-run.json'];$foot=$g['footprint.json'];$starter=$g['starter.json']
$self=$g['setup-self-test.json'];$overflow=$g['behavior-overflow.json'];$reorder=$g['behavior-reorder.json']
$idle=$foot.idle.ExplorerMvpHost;$churn=$foot.churn.ExplorerMvpHost
$checks=[ordered]@{
 nativeContent=(Passed 'native-content-0','native-content-1') -and $modes.nativeContentExit -eq 0
 physicalInput=Passed 'physical'
 material=$modes.faultExit -eq 0 -and $modes.modeExit -eq 0 -and $modes.fixtureExit -eq 0
 accessibility=Passed 'expanded','menu','edit'
 lifecycle=Passed 'detach','drag','lifetime','graphics'
 snapping=(Passed 'snap-generic') -and $g['snap-generic.json'].success -eq $true
 combinedExplorerSnapping=(Passed 'snap-explorer-pair') -and $g['snap-explorer-pair.json'].success -eq $true
 setupRehearsal=$g['setup-rehearsal.json'].success -eq $true
 switching=(RunOk 'switching') -and (RunOk 'timing')
 overflow=(RunOk 'overflow') -and $overflow.tabs -ge 120
 motion=(RunOk 'reorder') -and (RunOk 'drag') -and $reorder.motionOnMs -eq 200 -and $reorder.motionOffMs -eq 0 -and $g['options.json'].passed -eq $true
 colours=$ink.exit -eq 0
 footprint=$null -ne $idle -and $idle.cpuPercent -le 0.05 -and $churn.cpuPercent -le 2 -and $starter.passed -eq $true
 singleFileSetup=$self.success -eq $true -and $self.embeddedFiles -eq 7 -and $hostSetup.support -eq 'caption' -and $hostTests.tamperedPayloadRejected -eq $true -and $hostTests.componentPinsMatch -eq $true
}
$basis=[ordered]@{
 nativeContent="final-combined native-content-0/1 and renderer-modes nativeContentExit=$($modes.nativeContentExit) on these binaries"
 physicalInput='final-combined physical stage: guest SendInput pointer and keyboard path. Host touchpad hardware was not exercised.'
 material="renderer-modes harness: native=$($modes.nativeContentExit) faults=$($modes.faultExit) modes=$($modes.modeExit) state=$($modes.fixtureExit)"
 accessibility='behavior expanded/menu/edit stages'
 lifecycle='behavior detach/drag/lifetime and graphics-loss stages'
 snapping="snap-generic success=$($g['snap-generic.json'].success)"
 combinedExplorerSnapping="snap-explorer-pair success=$($g['snap-explorer-pair.json'].success) with two real Explorer frames carrying the active header"
 setupRehearsal='setup-rehearsal: install, update with multi-tab windows, tabs off/on, cancelled stop, legacy block'
 switching="switching $(ChecksOf 'switching') checks; timing fixture $(ChecksOf 'timing'); logs evidence/refine-20261110/u10/guest/timing-*.log"
 overflow="overflow $(ChecksOf 'overflow') checks with $($overflow.tabs) tabs at $($overflow.dpi) DPI, tab width $($overflow.tabWidth) px"
 motion="reorder $(ChecksOf 'reorder') (motion on $($reorder.motionOnMs) ms, off $($reorder.motionOffMs) ms), physical drag $(ChecksOf 'drag'), Animate tabs option mirrors config"
 colours="caption-ink harness exit $($ink.exit)"
 footprint="starter idle $($idle.cpuMs) ms CPU over $($foot.idleSeconds) s, $([math]::Round($idle.privateBytes/1MB,2)) MB private; $($churn.cpuPercent)% CPU under $($foot.churnIterations) top-level window creations; starter attach scenarios passed=$($starter.passed)"
 singleFileSetup="guest self-test $($self.passed)/$($self.total), $($self.embeddedFiles) embedded files verified (the guest reports base build 19041, so its support line is '$($self.support)'); on this PC setup reports support=$($hostSetup.support); host tamper test rejected a flipped byte; setup pins equal runtime pins"
}
# A stage counts by its latest attempt (by end time): the full run, then any
# re-run of it. Earlier failures stay listed under attempts.
if(-not $suite.complete){throw 'The full suite did not complete.'}
$attempts=@($suite.stages|ForEach-Object {$_|Add-Member -NotePropertyName run -NotePropertyValue 'full' -PassThru})
foreach($name in $reruns){
 if(-not $g[$name].complete){throw "The re-run $name did not complete."}
 $attempts+=@($g[$name].stages|ForEach-Object {$_|Add-Member -NotePropertyName run -NotePropertyValue $name -PassThru})
}
$attempts=@($attempts | Sort-Object {[DateTimeOffset]::Parse([string]$_.end)})
$latest=[ordered]@{};foreach($a in $attempts){$latest[$a.name]=$a}
$failedStages=@($latest.Values | Where-Object {$_.error})
# A pass that follows a failure is not a clean pass: the receipt names each such
# stage with its tally. Each failure's diagnosis is in attempt*\what-happened.json.
$recovered=@($attempts | Group-Object name | Where-Object {@($_.Group | Where-Object error).Count} | ForEach-Object {"$($_.Name) ($(@($_.Group | Where-Object error).Count) of $($_.Count) attempts failed)"})
$reviewData=Read-Json $Review
$sourceFiles=@(Get-ChildItem -LiteralPath (Join-Path $root 'native') -File | Sort-Object Name | ForEach-Object {$_.FullName})
$sourceFiles+=foreach($file in 'Build-Native.ps1','Build-Mvp.ps1','Test-Native.ps1','Test-Mvp.ps1','Test-HeaderIntegration.ps1','New-HeaderAcceptance.ps1'){Join-Path $root $file}
$sourceSha=[ordered]@{};foreach($file in $sourceFiles){$sourceSha[(Relative $file)]=Hash $file}
$evidence=@(Get-ChildItem -LiteralPath $EvidenceDirectory -File | Sort-Object Name | ForEach-Object {[ordered]@{path=Relative $_.FullName;sha256=Hash $_.FullName}})
$evidence+=[ordered]@{path=Relative $Review;sha256=Hash $Review}
# Stalled and failed attempts before the passing runs, with their diagnoses: the
# text records only (screenshots and pre-fix test programs stay in the repository).
# Directories first: with -Recurse, a wildcard in -Path filters file names instead.
$evidence+=@(Get-ChildItem -LiteralPath (Split-Path -Parent $EvidenceDirectory) -Directory -Filter 'attempt*' | Get-ChildItem -File -Recurse | Where-Object {$_.Extension -in '.json','.jsonl','.log','.txt'} | Sort-Object FullName | ForEach-Object {[ordered]@{path=Relative $_.FullName;sha256=Hash $_.FullName}})
$success=-not @($checks.Values | Where-Object {$_ -ne $true}).Count -and -not $failedStages.Count -and $reviewData.status -eq 'complete'
$receipt=[ordered]@{
 schemaVersion=2
 success=$success
 date=(Get-Date).ToString('o')
 target="Windows 10 22H2 x64 (19045.$((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').UBR)); Sandbox guest reports base $($suite.guestBuild) with native components matching this host"
 binarySha256=$shipped
 checks=$checks
 basis=$basis
 review=[ordered]@{status=$reviewData.status;receipt=(Relative $Review);verdict=$reviewData.verdict}
 failedStages=@($failedStages | ForEach-Object {[ordered]@{name=$_.name;error=$_.error}})
 stages=@($attempts | ForEach-Object {[ordered]@{name=$_.name;run=$_.run;passed=-not $_.error;error=$_.error;seconds=$_.seconds}})
 limitations=@(
  'Touchpad gestures verified through the receiver hotkey path, not physical hardware'
  'Sandbox guest is base build 19041 with byte-identical native components; the lab starter variant skips only the build-number check'
  'Per-window Explorer memory with tabs attached is not measured: the footprint fixture''s windows did not attach tabs in the guest'
  'Sandbox timing varies by tens of milliseconds between runs'
  if($recovered.Count){"Passed after earlier failed attempts: $($recovered -join ', '). Diagnoses: $(Relative (Split-Path -Parent $EvidenceDirectory))\attempt*\what-happened.json"}
 )
 sourceSha256=$sourceSha
 evidence=$evidence
}
New-Item -ItemType Directory -Path (Split-Path -Parent $Out) -Force | Out-Null
$receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Out -Encoding UTF8
foreach($name in $checks.Keys){[pscustomobject]@{Check=$name;Passed=$checks[$name];Basis=$basis[$name]}}
if(-not $success){Write-Error ("Acceptance failed. Failed stages: "+(@($failedStages | ForEach-Object name) -join ', '));exit 1}
