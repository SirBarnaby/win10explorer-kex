# Records the README GIF scenes in the guest. Scenes to run come from
# C:\NativeLabPayload\gif-run.json: {"scenes":["probe","tabs-overview",...]}.
$ErrorActionPreference='Stop'
. C:\NativeLabPayload\Gif-Lib.ps1
$plan=Get-Content "$Payload\gif-run.json" -Raw|ConvertFrom-Json
$status=[ordered]@{started=(Get-Date).ToString('o');scenes=[ordered]@{}}
function Save-Status{$status|ConvertTo-Json -Depth 6|Set-Content "$Out\status.json" -Encoding UTF8}
try{
 # ---- environment (guest only) ----
 $script:RunDir=Join-Path C:\NativeLab ('gif-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $RunDir|Out-Null
 Copy-Item "$Payload\proof\HeaderLab.exe","$Payload\proof\HeaderLabHook.dll" $RunDir
 Copy-Item "$Payload\final\ExplorerTabs.dll","$Payload\final\Win10PatchOptions.exe","$Payload\final\NativeSnap.exe","$Payload\final\ExplorerMvpHost.exe" $RunDir
 $status.binaries=@(Get-ChildItem $RunDir -File|ForEach-Object{[ordered]@{name=$_.Name;sha256=(Get-FileHash $_.FullName).Hash}})
 # Remote sessions switch client-area animation off; a desktop PC has it on.
 $anim=0;[void][GifRec]::SpiGet(0x1042,0,[ref]$anim,0);[void][GifRec]::SystemParametersInfoW(0x1043,0,[IntPtr]1,2)
 $after=0;[void][GifRec]::SpiGet(0x1042,0,[ref]$after,0);$status.clientAreaAnimation=@($anim,$after)
 # Tabs on, motion on, session restore off.
 $settings='HKCU:\Software\Win10NativePatch\v2';New-Item $settings -Force|Out-Null
 [byte[]]$config=[byte[]]::new(64)
 [BitConverter]::GetBytes([uint32]64).CopyTo($config,0);[BitConverter]::GetBytes([uint32]2).CopyTo($config,4)
 [BitConverter]::GetBytes([uint32]5).CopyTo($config,8);[BitConverter]::GetBytes([uint32]11).CopyTo($config,12)
 for($i=0;$i-lt 4;$i++){[BitConverter]::GetBytes([uint32](37+$i)).CopyTo($config,16+4*$i)}
 New-ItemProperty $settings -Name Config -PropertyType Binary -Value $config -Force|Out-Null
 Remove-Item "$settings\ExplorerTabs" -Recurse -Force -ErrorAction SilentlyContinue
 # Demo folders with ordinary names.
 $home2=$env:USERPROFILE;$docs="$home2\Documents";$pics="$home2\Pictures";$music="$home2\Music";$videos="$home2\Videos";$downloads="$home2\Downloads"
 foreach($d in 'Projects','Invoices','Notes'){New-Item -ItemType Directory "$docs\$d" -Force|Out-Null}
 foreach($f in 'Budget 2026.xlsx','Meeting notes.docx','Packing list.txt','Report draft.docx','Roadmap.pptx'){if(-not(Test-Path "$docs\$f")){[IO.File]::WriteAllText("$docs\$f",'demo')}}
 $colors=@('#2E86AB','#F6C85F','#6F4E7C','#9FD356','#CA472F','#0B84A5');$n=0
 foreach($name in 'Harbour','Meadow','Dusk','Orchard','Canyon','Lagoon'){
  $file="$pics\$name.png";if(-not(Test-Path $file)){$b=New-Object Drawing.Bitmap 640,420;$g=[Drawing.Graphics]::FromImage($b)
   $brush=New-Object Drawing.Drawing2D.LinearGradientBrush ([Drawing.Rectangle]::new(0,0,640,420)),([Drawing.ColorTranslator]::FromHtml($colors[$n])),([Drawing.Color]::White),45
   $g.FillRectangle($brush,0,0,640,420);$g.Dispose();$b.Save($file);$b.Dispose()};$n++}
 foreach($f in 'Morning run.mp3','Road trip mix.mp3','Rainy day.mp3'){if(-not(Test-Path "$music\$f")){[IO.File]::WriteAllText("$music\$f",'demo')}}
 foreach($f in 'Birthday.mp4','Garden timelapse.mp4'){if(-not(Test-Path "$videos\$f")){[IO.File]::WriteAllText("$videos\$f",'demo')}}
 foreach($f in 'setup-guide.pdf','photos.zip','invoice-1042.pdf'){if(-not(Test-Path "$downloads\$f")){[IO.File]::WriteAllText("$downloads\$f",'demo')}}
 $many=@('Accounts','Archive','Backups','Blog','Books','Camera roll','Clients','Contracts','Designs','Drafts','Garden','Holiday 2025','House','Insurance','Letters','Manuals','Maps','Receipts','Recipes','Reference','Scans','School','Screenshots','Taxes','Templates','Travel','Wallpapers','Work')
 foreach($m in $many){New-Item -ItemType Directory "C:\Demo\$m" -Force|Out-Null;if(-not(Test-Path "C:\Demo\$m\Readme.txt")){[IO.File]::WriteAllText("C:\Demo\$m\Readme.txt",'demo')}}
 # Plain folders for the demos: Pictures, Music and Videos add Explorer's own
 # contextual title-bar tab, which moves the strip on every switch.
 $pics='C:\Demo\Travel';$music='C:\Demo\Recipes';$videos='C:\Demo\Work'
 foreach($f in 'Itinerary.docx','Tickets.pdf','Hotel booking.pdf','Packing list.txt'){[IO.File]::WriteAllText("$pics\$f",'demo')}
 foreach($f in 'Bread.docx','Soup.docx','Weeknight dinners.xlsx'){[IO.File]::WriteAllText("$music\$f",'demo')}
 foreach($f in 'Timesheet.xlsx','Presentation.pptx','Notes.txt'){[IO.File]::WriteAllText("$videos\$f",'demo')}
 Remove-Item "$pics\Readme.txt","$music\Readme.txt","$videos\Readme.txt" -ErrorAction SilentlyContinue
 $screenW=[GifRec]::GetSystemMetrics(0);$screenH=[GifRec]::GetSystemMetrics(1);$status.screen=@($screenW,$screenH)
 $workH=$screenH-40
 Save-Status

 function Scene([string]$name,[scriptblock]$body){
  if($plan.scenes -notcontains $name){return}
  Note "begin $name";$err=$null
  try{Reset-Desktop;& $body}catch{$err=$_.Exception.Message+' @ '+$_.InvocationInfo.ScriptLineNumber;Note "FAILED $name $err";try{[GifRec]::Stop()}catch{}}
  try{[GifRec]::Shot("$Out\end-$name.png")}catch{}
  $status.scenes[$name]=[ordered]@{error=$err;end=(Get-Date).ToString('o')};Save-Status
 }

 Scene 'probe' {
  $f=New-Frame $docs 160 70 880 600
  Add-Tab $f $pics
  $status.probe=[ordered]@{frame=$f;strip=(Strip $f);focus=(Focus $f);frameRect=[GifRec]::Frame((Hw $f));acc=@(StripAcc $f);windows=([GifRec]::AllTop() -split "`n")}
  Save-Status
  $r=[GifRec]::Frame((Hw $f));[GifRec]::Jump($r[0]+400,$r[1]+350)
  Start-Rec 'probe' $r
  Pause 300;$t=Tabs $f;Go-To $t[0];[GifRec]::Click();Pause 900;Go-To $t[1];[GifRec]::Click();Pause 900
  Stop-Rec 'probe'
 }

 Scene 'tabs-overview' {
  $f=New-Frame $docs 160 70 880 600
  Add-Tab $f $pics
  [void](Focus $f);$t=Tabs $f;Go-To $t[0] 200;[GifRec]::Click();Pause 900
  $r=[GifRec]::Frame((Hw $f));[GifRec]::Jump($r[0]+470,$r[1]+400);Pause 300;[void](Focus $f)
  Start-Rec 'tabs-overview' $r
  Pause 500
  $new=(Part $f 'New tab')[0];Go-To $new 650;Pause 150;[GifRec]::Click();Pause 1100
  $t=Tabs $f;Go-To $t[1] 500;Pause 120;[GifRec]::Click();Pause 1200
  $t=Tabs $f;Go-To $t[0] 450;Pause 120;[GifRec]::Click();Pause 1100
  $t=Tabs $f;Go-To $t[2] 500;Pause 120;[GifRec]::Click();Pause 900
  $close=@(StripAcc $f|Where-Object{$_.Name-like 'Close *'-and $_.W-gt 0}|Sort-Object X)[-1];Go-To $close 350;Pause 250;[GifRec]::Click();Pause 500
  Go ($r[0]+470) ($r[1]+400) 600;Pause 500
  Stop-Rec 'tabs-overview' @{tabsAtEnd=@(Tabs $f|ForEach-Object{$_.Name})}
 }

 Scene 'tab-reorder' {
  $f=New-Frame $docs 160 70 880 600
  foreach($p in $pics,$music,$videos,$downloads){Add-Tab $f $p}
  $r=[GifRec]::Frame((Hw $f));[GifRec]::Jump($r[0]+470,$r[1]+400);Pause 300;[void](Focus $f)
  $before=@(Tabs $f|ForEach-Object{$_.Name})
  Start-Rec 'tab-reorder' $r
  Pause 500
  $t=Tabs $f;Go-To $t[0] 600;Pause 150;[GifRec]::Down();Pause 150
  Go ($t[3].Cx+10) $t[0].Cy 1500;Pause 350;[GifRec]::Up();Pause 800
  $t=Tabs $f;Go-To $t[4] 450;Pause 150;[GifRec]::Down();Pause 150
  Go ($t[1].Cx-10) $t[4].Cy 1300;Pause 350;[GifRec]::Up();Pause 600
  Go ($r[0]+470) ($r[1]+400) 600;Pause 500
  Stop-Rec 'tab-reorder' @{before=$before;after=@(Tabs $f|ForEach-Object{$_.Name})}
 }

 Scene 'tab-overflow' {
  $f=New-Frame "C:\Demo\$($many[0])" 160 70 880 600
  foreach($m in $many[1..($many.Count-1)]){Add-Tab $f "C:\Demo\$m"}
  $r=[GifRec]::Frame((Hw $f));[void](Focus $f)
  # Start from the left end of the strip.
  $s=Strip $f;$sr=[GifRec]::Rect((Hw $s));[GifRec]::Jump($sr[0]+[int]($sr[2]/2),$sr[1]+[int]($sr[3]/2));Pause 200
  for($i=0;$i-lt 40;$i++){[GifRec]::Wheel(120);Pause 40};Pause 600
  $t=Tabs $f;if($t.Count){Go-To $t[0] 200;[GifRec]::Click();Pause 900}
  [GifRec]::Jump($r[0]+470,$r[1]+400);Pause 300;[void](Focus $f)
  Start-Rec 'tab-overflow' $r
  Pause 500
  Go ($sr[0]+[int]($sr[2]*0.45)) ($sr[1]+[int]($sr[3]/2)) 600;Pause 300
  for($i=0;$i-lt 6;$i++){[GifRec]::Wheel(-120);Pause 320};Pause 500
  for($i=0;$i-lt 3;$i++){[GifRec]::Wheel(120);Pause 320};Pause 400
  $right=@(Part $f 'Scroll tabs right')|Select-Object -First 1
  if($right){Go-To $right 550;Pause 150;for($i=0;$i-lt 5;$i++){[GifRec]::Click();Pause 380}}
  $t=Tabs $f;$pick=$t[[int]($t.Count/2)];Go-To $pick 500;Pause 150;[GifRec]::Click();Pause 1000
  Go ($r[0]+470) ($r[1]+400) 600;Pause 400
  Stop-Rec 'tab-overflow' @{parts=@(StripAcc $f|Where-Object{$_.Role-ne '37'-and $_.Name-notlike 'Close *'}|ForEach-Object{$_.Name});visibleTabs=(Tabs $f).Count}
 }

 Scene 'detach-probe' {
  $f=New-Frame $docs 50 40 780 470
  foreach($p in $pics,$music,$videos){Add-Tab $f $p}
  $d=[ordered]@{}
  function Try-Move([string]$label){
   [void](Focus $f);$known=Frames;$t=Tabs $f;Go-To $t[1] 300;Pause 150;[GifRec]::RightClick();Pause 700
   $menu=[long]@([GifRec]::Top('#32768'))[0];$item=@(Acc $menu|Where-Object{$_.Name-like 'Move tab*'})[0]
   Go-To $item 300;Pause 200;[GifRec]::Click();Pause 6000
   $new=@(Frames|Where-Object{$_-notin $known})
   $d[$label]=[ordered]@{newFrames=$new;newHasStrip=@($new|ForEach-Object{[bool](Strip $_)});newRects=@($new|ForEach-Object{[GifRec]::Frame((Hw $_)) -join ','});sourceTabs=@(Tabs $f|ForEach-Object{$_.Name});newTabs=@($new|ForEach-Object{(Tabs $_|ForEach-Object{$_.Name}) -join '/'})}
   [GifRec]::Shot("$Out\detach-$label.png")
  }
  Try-Move 'plain'
  Copy-Item "$Payload\refine-u10host\ExplorerMvpHost.exe" "$RunDir\LabHost.exe"
  $starter=Start-Process "$RunDir\LabHost.exe" -WindowStyle Hidden -PassThru;Pause 2000
  $d.starterAlive=(-not $starter.HasExited)
  Try-Move 'with-starter'
  $stop=Start-Process "$RunDir\LabHost.exe" -ArgumentList '--stop' -WindowStyle Hidden -PassThru;[void]$stop.WaitForExit(10000)
  $status.detach=$d;Save-Status
 }
 Scene 'tab-tear-out' {
  $f=New-Frame $docs 740 6 720 430
  foreach($p in $pics,$music,$videos){Add-Tab $f $p}
  [void](Focus $f);[GifRec]::Jump(1100,300);Pause 900
  $known=Frames
  Start-Rec 'tab-tear-out' @(0,0,$screenW,$workH) 20
  Pause 500
  $t=Tabs $f;$pick=@($t|Where-Object{$_.Name-eq 'Recipes'})[0];Go-To $pick 600;Pause 200;[GifRec]::Down();Pause 250
  Go ($pick.Cx-20) ($pick.Cy+50) 350;Go 420 520 1300;Pause 500;[GifRec]::Up()
  $sw=[Diagnostics.Stopwatch]::StartNew()
  [void](Wait-Until {@(Frames|Where-Object{$_-notin $known}).Count-gt 0} 8000)
  $b=[long]@(Frames|Where-Object{$_-notin $known})[0]
  $extra=@{newFrame=$b;appearedMs=$sw.ElapsedMilliseconds}
  if($b){
   # This guest reports build 19041, so every window here gets its tabs through
   # the lab attach, the new one included. The transfer itself is the product's.
   Pause 300;try{Attach $b}catch{$extra.attachError=$_.Exception.Message}
   [void](Wait-Until {@(Tabs $b).Count-gt 0 -and @(Tabs $f).Count-eq 3} 9000)
   $extra.transferMs=$sw.ElapsedMilliseconds;$extra.aAfterDetach=@(Tabs $f|ForEach-Object{$_.Name});$extra.bAfterDetach=@(Tabs $b|ForEach-Object{$_.Name})
   Pause 1200
   if(@(Tabs $b).Count-gt 0){
    $t=Tabs $f;$v=@($t|Where-Object{$_.Name-eq 'Work'})[0];$bt=@(Tabs $b);$last=$bt[-1]
    Go-To $v 800;Pause 200;[GifRec]::Down();Pause 250
    Go ($v.Cx-15) ($v.Cy+40) 350;Go ($last.X+$last.W+35) $last.Cy 1300;Pause 500;[GifRec]::Up();Pause 1800
    $extra.aTabs=@(Tabs $f|ForEach-Object{$_.Name});$extra.bTabs=@(Tabs $b|ForEach-Object{$_.Name})
   }
  }
  Pause 500
  Stop-Rec 'tab-tear-out' $extra
 }
 Scene 'touchpad-snap' {
  $f=New-Frame $docs 280 110 880 560
  foreach($p in $pics,$music){Add-Tab $f $p}
  $snap=Start-Process "$RunDir\NativeSnap.exe" -WindowStyle Hidden -PassThru;Pause 800
  try{
   [void](Focus $f);[GifRec]::Jump(700,430);Pause 300
   Start-Rec 'touchpad-snap' @(0,0,$screenW,$workH) 20
   Pause 700
   $steps=@(0x25,0x26,0x27,0x28,0x25)
   $trace=@()
   foreach($k in $steps){[GifRec]::Chord(0x11,0x12,0x5B,$k);Pause 1150;$trace+=,@($k,([GifRec]::Frame((Hw $f)) -join ','))}
   Pause 400
   Stop-Rec 'touchpad-snap' @{trace=$trace;receiverAlive=(-not $snap.HasExited)}
  }finally{$stop=Start-Process "$RunDir\NativeSnap.exe" -ArgumentList '--stop' -WindowStyle Hidden -PassThru;[void]$stop.WaitForExit(10000)}
 }

 Scene 'options' {
  $f=New-Frame $docs 120 50 880 640
  foreach($p in $pics,$music){Add-Tab $f $p}
  $snap=Start-Process "$RunDir\NativeSnap.exe" -WindowStyle Hidden -PassThru;Pause 800
  [void](Focus $f);$r=[GifRec]::Frame((Hw $f));[GifRec]::Jump($r[0]+470,$r[1]+420);Pause 900
  Start-Rec 'options' @(0,0,$screenW,$workH) 20
  Pause 500
  $t=Tabs $f;Go-To $t[1] 600;Pause 150;[GifRec]::RightClick();Pause 700
  $menu=[long]@([GifRec]::Top('#32768'))[0];$items=Acc $menu
  $opt=@($items|Where-Object{$_.Name-like 'Options*'})[0]
  $extra=@{menu=@($items|ForEach-Object{$_.Name})}
  if(-not $opt){throw 'Options menu item not found'}
  Go-To $opt 600;Pause 250;[GifRec]::Click()
  $sheet=0;[void](Wait-Until {$script:sheets=@([GifRec]::Top('#32770')|Where-Object{[GifRec]::Text((Hw $_))-eq 'Explorer Options'});$script:sheets.Count-gt 0} 8000)
  $sheet=[long]$script:sheets[0];if(-not $sheet){throw 'Options did not open'}
  Pause 1200
  $extra.sheetRect=[GifRec]::Rect((Hw $sheet))
  $tabCtl=[GifRec]::Child((Hw $sheet),'SysTabControl32');$pages=Acc $tabCtl;$extra.pages=@($pages|ForEach-Object{$_.Name})
  $tabsPage=@($pages|Where-Object{$_.Name-eq 'Tabs'})[0];$snapPage=@($pages|Where-Object{$_.Name-like 'Snap*'})[0]
  if($tabsPage){Go-To $tabsPage 600;Pause 150;[GifRec]::Click();Pause 1100
   $page=[GifRec]::SendMessageW((Hw $sheet),0x476,[IntPtr]::Zero,[IntPtr]::Zero);$box=[GifRec]::GetDlgItem($page,1005)
   if($box-ne [IntPtr]::Zero){$br=[GifRec]::Rect($box);Go ($br[0]+8) ($br[1]+[int]($br[3]/2)) 600;Pause 300;[GifRec]::Click();Pause 900;[GifRec]::Click();Pause 800}}
  if($snapPage){Go-To $snapPage 600;Pause 150;[GifRec]::Click();Pause 1500}
  $cancel=[GifRec]::GetDlgItem((Hw $sheet),2)
  if($cancel-ne [IntPtr]::Zero){$cr=[GifRec]::Rect($cancel);Go ($cr[0]+[int]($cr[2]/2)) ($cr[1]+[int]($cr[3]/2)) 600;Pause 200;[GifRec]::Click();Pause 800}
  Stop-Rec 'options' $extra
  $stop=Start-Process "$RunDir\NativeSnap.exe" -ArgumentList '--stop' -WindowStyle Hidden -PassThru;[void]$stop.WaitForExit(10000)
  foreach($s in @([GifRec]::Top('#32770'))){[void][GifRec]::PostMessageW((Hw $s),0x0010,[IntPtr]::Zero,[IntPtr]::Zero)}
 }
 Reset-Desktop
 $status.complete=$true
}catch{$status.fatal=$_.Exception.Message+' @ '+$_.InvocationInfo.ScriptLineNumber}
$status.finished=(Get-Date).ToString('o');Save-Status
