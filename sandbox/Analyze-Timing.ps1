# Summarises a lab timing log written by ExplorerTabs.dll (TimingLog) and the
# HeaderBehaviorTests "timing" fixture. Both write "<us> <event> <a> <b>" lines
# on the same QPC clock. Host-only, read-only; prints JSON.
param([Parameter(Mandatory)][string]$Log,[string]$Out)
$ErrorActionPreference='Stop'
$events=foreach($line in Get-Content -LiteralPath $Log){
    $f=$line -split ' '
    if($f.Count -ge 4){[pscustomobject]@{t=[double]$f[0]/1000.0;e=$f[1];a=[uint64]$f[2];b=[uint64]$f[3]}}
}
$events=@($events|Sort-Object t)
function Next([double]$from,[double]$until,[string]$name,[scriptblock]$where={$true}){
    foreach($x in $events){if($x.t -ge $from -and $x.t -lt $until -and $x.e -eq $name -and (& $where $x)){return $x}}
}
function All([double]$from,[double]$until,[string]$name){@($events|Where-Object{$_.t -ge $from -and $_.t -lt $until -and $_.e -eq $name})}
function Since($event,[double]$origin){if($event){[math]::Round($event.t-$origin,1)}else{$null}}
function Stats($values){
    $v=@($values|Where-Object{$null -ne $_}|Sort-Object)
    if(-not $v.Count){return $null}
    [ordered]@{n=$v.Count;median=[math]::Round($v[[int][math]::Floor(($v.Count-1)/2)],1);p90=[math]::Round($v[[int][math]::Min($v.Count-1,[math]::Ceiling($v.Count*0.9)-1)],1);min=[math]::Round($v[0],1);max=[math]::Round($v[-1],1)}
}
$inputs=@($events|Where-Object{$_.e -like 'input-*'})
function Switches([string]$kind,[scriptblock]$filter={$true}){
    $rows=@()
    for($i=0;$i -lt $inputs.Count;$i++){
        $x=$inputs[$i]
        if($x.e -ne $kind -or -not (& $filter $x)){continue}
        $until=[double]::MaxValue
        if($i+1 -lt $inputs.Count){$until=$inputs[$i+1].t}
        $activate=Next $x.t $until 'activate'
        $highlight=$null
        if($activate){$id=$activate.b;$highlight=Next $x.t $until 'render' {param($r)$r.b -eq $id}}
        $restores=@(All $x.t $until 'restore')
        $saves=@(All $x.t $until 'save-begin')
        $saveEnds=@(All $x.t $until 'save-end')
        $saveMs=0.0
        for($k=0;$k -lt [math]::Min($saves.Count,$saveEnds.Count);$k++){$saveMs+=$saveEnds[$k].t-$saves[$k].t}
        $firstRestore=$null;$lastRestore=$null
        if($restores.Count){$firstRestore=$restores[0];$lastRestore=$restores[-1]}
        $rows+=[pscustomobject]@{
            highlight=Since $highlight $x.t
            beforeNavigate=Since (Next $x.t $until 'beforenav') $x.t
            navigateComplete=Since (Next $x.t $until 'navcomplete') $x.t
            documentComplete=Since (Next $x.t $until 'doccomplete') $x.t
            commit=Since (Next $x.t $until 'commit') $x.t
            firstRestore=Since $firstRestore $x.t
            lastRestore=Since $lastRestore $x.t
            restoreCount=$restores.Count
            renders=@(All $x.t $until 'render').Count
            layouts=@(All $x.t $until 'layout').Count
            saves=$saves.Count
            saveMs=[math]::Round($saveMs,2)
        }
    }
    $rows
}
function Summary($rows){
    $o=[ordered]@{samples=@($rows).Count}
    foreach($k in 'highlight','beforeNavigate','navigateComplete','documentComplete','commit','firstRestore','lastRestore','restoreCount','renders','layouts','saves','saveMs'){$o[$k]=Stats ($rows|ForEach-Object{$_.$k})}
    $o
}
$phases=@($events|Where-Object{$_.e -eq 'phase'})
$many=[ordered]@{}
foreach($p in $phases|Where-Object{$_.a -ge 8}){
    $next=@($phases|Where-Object{$_.t -gt $p.t})[0]
    $end=[double]::MaxValue
    if($next){$end=$next.t}
    $start=$p.t
    $many["tabs$($p.a)"]=Summary (Switches 'input-switch-many' {param($x)$x.t -ge $start -and $x.t -lt $end})
}
$dragBegin=@($events|Where-Object{$_.e -eq 'drag-begin'})[0];$dragEnd=@($events|Where-Object{$_.e -eq 'drag-end'})[0]
$drag=$null
if($dragBegin -and $dragEnd){
    $seconds=($dragEnd.t-$dragBegin.t)/1000
    $drag=[ordered]@{seconds=[math]::Round($seconds,2);renders=@(All $dragBegin.t $dragEnd.t 'render').Count;layouts=@(All $dragBegin.t $dragEnd.t 'layout').Count;saves=@(All $dragBegin.t $dragEnd.t 'save-begin').Count}
    $drag.rendersPerSecond=[math]::Round($drag.renders/$seconds,1)
}
$result=[ordered]@{
    log=(Resolve-Path -LiteralPath $Log).Path
    tabSwitch=Summary (Switches 'input-switch')
    plainNavigation=Summary (Switches 'input-plain')
    newTab=Summary (Switches 'input-new')
    manyTabs=$many
    windowMove=$drag
}
$json=$result|ConvertTo-Json -Depth 6
if($Out){Set-Content -LiteralPath $Out -Value $json -Encoding UTF8}
$json
