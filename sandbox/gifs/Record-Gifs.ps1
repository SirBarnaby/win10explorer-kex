# Entry point for the lab dispatcher ({"action":"script","file":"Record-Gifs.ps1"}).
# Runs the recorder in its own hidden process so each run compiles afresh and
# cannot take the dispatcher down.
if($env:USERNAME-ne 'WDAGUtilityAccount'){throw 'Disposable guest only.'}
New-Item -ItemType Directory C:\NativeLabResults\gifs -Force|Out-Null
Remove-Item C:\NativeLabResults\gifs\status.json,C:\NativeLabResults\gifs\done.json -ErrorAction SilentlyContinue
$p=Start-Process powershell.exe -ArgumentList @('-NoLogo','-NoProfile','-STA','-ExecutionPolicy','Bypass','-File','C:\NativeLabPayload\Gif-Main.ps1') -WindowStyle Hidden -PassThru -RedirectStandardError C:\NativeLabResults\gifs\stderr.txt -RedirectStandardOutput C:\NativeLabResults\gifs\stdout.txt
$ok=$p.WaitForExit(900000);if(-not $ok){$p.Kill()}
[ordered]@{exit=$(if($ok){$p.ExitCode}else{-1});timedOut=(-not $ok);time=(Get-Date).ToString('o')}|ConvertTo-Json|Set-Content C:\NativeLabResults\gifs\done.json -Encoding UTF8
