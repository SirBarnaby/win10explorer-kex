# Guest-only toolkit for recording README GIFs: screen capture with the pointer
# drawn in, real pointer/keyboard input through SendInput, and MSAA lookups.
$ErrorActionPreference='Stop'
if($env:USERNAME-ne 'WDAGUtilityAccount'){throw 'Disposable guest only.'}
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing,Accessibility -TypeDefinition @'
using System;using System.Collections.Concurrent;using System.Collections.Generic;using System.Diagnostics;using System.Drawing;using System.Drawing.Imaging;using System.IO;using System.Runtime.InteropServices;using System.Text;using System.Threading;
public static class GifRec{
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int l,t,r,b;}
 [StructLayout(LayoutKind.Sequential)] public struct POINT{public int x,y;}
 [StructLayout(LayoutKind.Sequential)] struct CURSORINFO{public int cbSize;public int flags;public IntPtr hCursor;public POINT pt;}
 [StructLayout(LayoutKind.Sequential)] struct ICONINFO{public bool fIcon;public int xHot;public int yHot;public IntPtr mask;public IntPtr color;}
 [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT{public int dx,dy;public uint data,flags,time;public IntPtr extra;}
 [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT{public ushort vk,scan;public uint flags,time;public IntPtr extra;}
 [StructLayout(LayoutKind.Explicit)] struct INPUT{[FieldOffset(0)]public uint type;[FieldOffset(8)]public MOUSEINPUT mi;[FieldOffset(8)]public KEYBDINPUT ki;}
 delegate bool EnumProc(IntPtr h,IntPtr p);
 [DllImport("user32.dll")] static extern bool GetCursorInfo(ref CURSORINFO c);
 [DllImport("user32.dll")] static extern bool GetIconInfo(IntPtr h,out ICONINFO i);
 [DllImport("user32.dll")] static extern bool DrawIconEx(IntPtr dc,int x,int y,IntPtr icon,int w,int h,int step,IntPtr brush,int flags);
 [DllImport("user32.dll")] static extern IntPtr LoadCursorW(IntPtr inst,IntPtr id);
 [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
 [DllImport("user32.dll",SetLastError=true)] static extern uint SendInput(uint n,INPUT[] i,int size);
 [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
 [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
 [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f,IntPtr p);
 [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int hh,uint flags);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int cmd);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a,uint b,bool attach);
 [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool SystemParametersInfoW(uint a,uint b,IntPtr c,uint d);
 [DllImport("user32.dll",EntryPoint="SystemParametersInfoW")] public static extern bool SpiGet(uint a,uint b,ref int c,uint d);
 [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h,int attr,out RECT r,int size);
 [DllImport("oleacc.dll")] static extern int AccessibleObjectFromWindow(IntPtr h,uint id,ref Guid iid,[MarshalAs(UnmanagedType.Interface)] out object acc);

 // ---- windows ----
 public static string Cls(IntPtr h){var s=new StringBuilder(160);GetClassNameW(h,s,160);return s.ToString();}
 public static string Text(IntPtr h){var s=new StringBuilder(260);GetWindowTextW(h,s,260);return s.ToString();}
 public static List<long> Top(string cls){var l=new List<long>();EnumWindows(delegate(IntPtr h,IntPtr p){if(IsWindowVisible(h)&&Cls(h)==cls)l.Add(h.ToInt64());return true;},IntPtr.Zero);return l;}
 public static string AllTop(){var sb=new StringBuilder();EnumWindows(delegate(IntPtr h,IntPtr p){if(IsWindowVisible(h)){RECT r;GetWindowRect(h,out r);sb.Append(h.ToInt64()+"|"+Cls(h)+"|"+Text(h)+"|"+r.l+","+r.t+","+r.r+","+r.b+"\n");}return true;},IntPtr.Zero);return sb.ToString();}
 public static long Child(IntPtr parent,string prefix){long found=0;EnumChildWindows(parent,delegate(IntPtr h,IntPtr p){if(found==0&&IsWindowVisible(h)&&Cls(h).StartsWith(prefix))found=h.ToInt64();return true;},IntPtr.Zero);return found;}
 public static int[] Frame(IntPtr h){RECT r;if(DwmGetWindowAttribute(h,9,out r,16)!=0)GetWindowRect(h,out r);return new int[]{r.l,r.t,r.r-r.l,r.b-r.t};}
 public static int[] Rect(IntPtr h){RECT r;GetWindowRect(h,out r);return new int[]{r.l,r.t,r.r-r.l,r.b-r.t};}
 public static bool Activate(IntPtr h){
  IntPtr fg=GetForegroundWindow();uint pid;uint mine=GetCurrentThreadId(),other=GetWindowThreadProcessId(fg,out pid),target=GetWindowThreadProcessId(h,out pid);
  if(other!=0&&other!=mine)AttachThreadInput(mine,other,true);if(target!=mine&&target!=other)AttachThreadInput(mine,target,true);
  BringWindowToTop(h);SetForegroundWindow(h);
  if(other!=0&&other!=mine)AttachThreadInput(mine,other,false);if(target!=mine&&target!=other)AttachThreadInput(mine,target,false);
  Thread.Sleep(150);return GetForegroundWindow()==h;
 }
 // ---- MSAA: one line per child: id|role|x|y|w|h|name ----
 public static string Acc(IntPtr h){
  Guid iid=new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");object o;int hr=AccessibleObjectFromWindow(h,0xFFFFFFFC,ref iid,out o);
  if(hr!=0||o==null)return "ERR "+hr;
  Accessibility.IAccessible a=(Accessibility.IAccessible)o;var sb=new StringBuilder();int n=0;try{n=a.accChildCount;}catch(Exception e){return "ERR count "+e.Message;}
  for(int i=1;i<=n;i++){
   try{int l,t,w,hh;a.accLocation(out l,out t,out w,out hh,i);string name="";try{name=a.get_accName(i);}catch{}
    string role="";try{role=Convert.ToString(a.get_accRole(i));}catch{}
    sb.Append(i+"|"+role+"|"+l+"|"+t+"|"+w+"|"+hh+"|"+(name??"").Replace("\n"," ").Replace("|","/")+"\n");}
   catch(Exception e){
    // Menus and tab controls hand out child objects instead of simple ids.
    try{object c=a.get_accChild(i);Accessibility.IAccessible ca=c as Accessibility.IAccessible;if(ca!=null){int l,t,w,hh;ca.accLocation(out l,out t,out w,out hh,0);string name="";try{name=ca.get_accName(0);}catch{}string role="";try{role=Convert.ToString(ca.get_accRole(0));}catch{}
      sb.Append(i+"|"+role+"|"+l+"|"+t+"|"+w+"|"+hh+"|"+(name??"").Replace("\n"," ").Replace("|","/")+"\n");}else sb.Append(i+"|err|0|0|0|0|"+e.Message.Replace("\n"," ")+"\n");}
    catch(Exception e2){sb.Append(i+"|err|0|0|0|0|"+e2.Message.Replace("\n"," ")+"\n");}
   }
  }
  return sb.ToString();
 }
 // ---- input ----
 static void Send(INPUT i){var a=new INPUT[]{i};SendInput(1,a,Marshal.SizeOf(typeof(INPUT)));}
 static void MouseAbs(int x,int y){int w=GetSystemMetrics(0),h=GetSystemMetrics(1);var i=new INPUT();i.type=0;i.mi.dx=(int)Math.Round(x*65535.0/(w-1));i.mi.dy=(int)Math.Round(y*65535.0/(h-1));i.mi.flags=0x8001;Send(i);}
 static void MouseFlag(uint flags,uint data){var i=new INPUT();i.type=0;i.mi.flags=flags;i.mi.data=data;Send(i);}
 public static void Jump(int x,int y){MouseAbs(x,y);}
 // Eased pointer travel, as a hand would move it.
 public static void Move(int x,int y,int ms){
  POINT p;GetCursorPos(out p);var sw=Stopwatch.StartNew();
  while(true){double t=Math.Min(1.0,sw.ElapsedMilliseconds/(double)Math.Max(1,ms));double e=t<0.5?4*t*t*t:1-Math.Pow(-2*t+2,3)/2;
   MouseAbs((int)Math.Round(p.x+(x-p.x)*e),(int)Math.Round(p.y+(y-p.y)*e));if(t>=1.0)break;Thread.Sleep(8);}
 }
 public static void Down(){MouseFlag(0x0002,0);}
 public static void Up(){MouseFlag(0x0004,0);}
 public static void Click(){Down();Thread.Sleep(70);Up();}
 public static void RightClick(){MouseFlag(0x0008,0);Thread.Sleep(70);MouseFlag(0x0010,0);}
 public static void Wheel(int delta){MouseFlag(0x0800,unchecked((uint)delta));}
 static void Key(ushort vk,bool up){var i=new INPUT();i.type=1;i.ki.vk=vk;bool ext=(vk>=0x21&&vk<=0x28)||vk==0x5B;i.ki.flags=(up?2u:0u)|(ext?1u:0u);Send(i);}
 // Chord: all keys down in order, then up in reverse.
 public static void Chord(params int[] vks){foreach(int v in vks){Key((ushort)v,false);Thread.Sleep(25);}Thread.Sleep(40);for(int k=vks.Length-1;k>=0;k--){Key((ushort)vks[k],true);Thread.Sleep(25);}}
 // ---- capture ----
 static Thread grab,write;static volatile bool run;static ConcurrentQueue<KeyValuePair<int,Bitmap>> queue;static string dir;static int cx,cy,cw,ch,interval;
 public static int Frames,Missed,CursorHidden;
 public static void Start(int x,int y,int w,int h,int fps,string folder){
  cx=x;cy=y;cw=w;ch=h;interval=1000/fps;dir=folder;Directory.CreateDirectory(dir);queue=new ConcurrentQueue<KeyValuePair<int,Bitmap>>();Frames=0;Missed=0;CursorHidden=0;run=true;
  grab=new Thread(Grab);grab.Priority=ThreadPriority.AboveNormal;write=new Thread(Write);grab.Start();write.Start();
 }
 public static void Stop(){run=false;grab.Join();write.Join();}
 static void Grab(){
  var sw=Stopwatch.StartNew();int last=-1;IntPtr arrow=LoadCursorW(IntPtr.Zero,new IntPtr(32512));
  while(run){
   int slot=(int)(sw.ElapsedMilliseconds/interval);
   if(slot<=last){Thread.Sleep(2);continue;}
   if(slot>last+1)Missed+=slot-last-1;last=slot;
   var b=new Bitmap(cw,ch,PixelFormat.Format24bppRgb);
   using(var g=Graphics.FromImage(b)){
    g.CopyFromScreen(cx,cy,0,0,new Size(cw,ch),CopyPixelOperation.SourceCopy);
    var ci=new CURSORINFO();ci.cbSize=Marshal.SizeOf(typeof(CURSORINFO));
    if(GetCursorInfo(ref ci)){
     IntPtr cur=ci.hCursor;if((ci.flags&1)==0||cur==IntPtr.Zero){CursorHidden++;cur=arrow;}
     int hx=0,hy=0;ICONINFO ii;if(GetIconInfo(cur,out ii)){hx=ii.xHot;hy=ii.yHot;if(ii.mask!=IntPtr.Zero)DeleteObject(ii.mask);if(ii.color!=IntPtr.Zero)DeleteObject(ii.color);}
     IntPtr dc=g.GetHdc();DrawIconEx(dc,ci.pt.x-cx-hx,ci.pt.y-cy-hy,cur,0,0,0,IntPtr.Zero,3);g.ReleaseHdc(dc);
    }
   }
   queue.Enqueue(new KeyValuePair<int,Bitmap>(slot,b));Frames++;
  }
 }
 static void Write(){
  KeyValuePair<int,Bitmap> item;
  while(run||!queue.IsEmpty){
   if(queue.TryDequeue(out item)){item.Value.Save(Path.Combine(dir,"f"+item.Key.ToString("0000")+".png"),ImageFormat.Png);item.Value.Dispose();}
   else Thread.Sleep(5);
  }
 }
 public static void Shot(string path){int w=GetSystemMetrics(0),h=GetSystemMetrics(1);using(var b=new Bitmap(w,h,PixelFormat.Format24bppRgb)){using(var g=Graphics.FromImage(b)){g.CopyFromScreen(0,0,0,0,new Size(w,h));}b.Save(path,ImageFormat.Png);}}
}
'@

$script:Payload='C:\NativeLabPayload';$script:Out='C:\NativeLabResults\gifs';$script:Local='C:\GifRec'
New-Item -ItemType Directory $Out,$Local -Force|Out-Null
$script:Shell=New-Object -ComObject Shell.Application
$script:Log=[System.Collections.Generic.List[string]]::new()
function Note([string]$text){$Log.Add(('{0:HH:mm:ss.fff} {1}' -f (Get-Date),$text));$Log|Set-Content "$Out\log.txt" -Encoding UTF8}
function Hw([long]$h){[IntPtr]$h}
function Acc([long]$hwnd){
 $raw=[GifRec]::Acc((Hw $hwnd));if($raw.StartsWith('ERR')){Note "acc $hwnd $raw";return @()}
 @($raw -split "`n"|Where-Object{$_}|ForEach-Object{$p=$_ -split '\|',7;[pscustomobject]@{Id=[int]$p[0];Role=$p[1];X=[int]$p[2];Y=[int]$p[3];W=[int]$p[4];H=[int]$p[5];Name=$p[6];Cx=[int]$p[2]+[int]([int]$p[4]/2);Cy=[int]$p[3]+[int]([int]$p[5]/2)}})
}
function Strip([long]$frame){[GifRec]::Child((Hw $frame),'Win10NativePatch.ExplorerTabs')}
function StripAcc([long]$frame){$s=Strip $frame;if(-not $s){return @()};Acc $s}
# Visible tabs, left to right.
function Tabs([long]$frame){@(StripAcc $frame|Where-Object{$_.Role-eq '37'-and $_.W-gt 0}|Sort-Object X)}
function Part([long]$frame,[string]$name){@(StripAcc $frame|Where-Object{$_.Name-eq $name-and $_.W-gt 0}|Sort-Object X)}
function Frames{@([GifRec]::Top('CabinetWClass'))}
function Win([long]$frame){@($Shell.Windows()|Where-Object{[long]$_.HWND-eq $frame})[0]}
function Wait-Until([scriptblock]$test,[int]$ms=5000){$until=[DateTime]::UtcNow.AddMilliseconds($ms);while([DateTime]::UtcNow-lt $until){if(& $test){return $true};Start-Sleep -Milliseconds 100};$false}
function Attach([long]$frame){
 $report="$Local\attach-$frame.json"
 $p=Start-Process "$RunDir\HeaderLab.exe" -ArgumentList @([string]$frame,$report,'--attach-only') -WindowStyle Hidden -PassThru
 if(-not $p.WaitForExit(15000)){$p.Kill();throw "attach timed out for $frame"}
 if($p.ExitCode){throw "attach exit $($p.ExitCode) for $frame"}
}
function New-Frame([string]$path,[int]$x,[int]$y,[int]$w,[int]$h,[switch]$NoAttach){
 $before=Frames;Start-Process C:\Windows\explorer.exe -ArgumentList ('/n,'+$path)
 $frame=0;[void](Wait-Until {$script:nf=@(Frames|Where-Object{$_-notin $before})[0];[bool]$script:nf} 10000);$frame=[long]$script:nf
 if(-not $frame){throw "no frame for $path"}
 Start-Sleep -Milliseconds 900
 [void][GifRec]::SetWindowPos((Hw $frame),[IntPtr]::Zero,$x,$y,$w,$h,0x0014)
 Start-Sleep -Milliseconds 300
 if(-not $NoAttach){Attach $frame;Start-Sleep -Milliseconds 600}
 $frame
}
function Focus([long]$frame){$ok=[GifRec]::Activate((Hw $frame));if(-not $ok){Note "activate failed for $frame; foreground is $([GifRec]::GetForegroundWindow())"};$ok}
function Add-Tab([long]$frame,[string]$path){
 [void](Focus $frame);$script:tabCountBefore=@(StripAcc $frame|Where-Object{$_.Role-eq '37'}).Count
 [GifRec]::Chord(0x11,0x54) # Ctrl+T
 [void](Wait-Until {(StripAcc $frame|Where-Object{$_.Role-eq '37'}).Count-gt $script:tabCountBefore} 3000)
 Start-Sleep -Milliseconds 350
 $w=Win $frame;$w.Navigate2($path)
 $leaf=Split-Path $path -Leaf
 [void](Wait-Until {try{(Win $frame).LocationName-eq $leaf}catch{$false}} 4000)
 Start-Sleep -Milliseconds 450
}
function Go([int]$x,[int]$y,[int]$ms=550){[GifRec]::Move($x,$y,$ms)}
function Go-To($part,[int]$ms=550){[GifRec]::Move($part.Cx,$part.Cy,$ms)}
function Pause([int]$ms){Start-Sleep -Milliseconds $ms}
function Reset-Desktop{
 try{& "$Payload\Reset-GuestDesktop.ps1" -Label gif}catch{Note "reset: $($_.Exception.Message)"}
 foreach($f in Frames){[void][GifRec]::PostMessageW((Hw $f),0x0010,[IntPtr]::Zero,[IntPtr]::Zero)}
 Start-Sleep -Milliseconds 700
 # The lab's own console is not part of any scene.
 foreach($c in [GifRec]::Top('ConsoleWindowClass')){[void][GifRec]::ShowWindow((Hw $c),0)}
}
function Start-Rec([string]$scene,[int[]]$rect,[int]$fps=25){
 $script:SceneDir=Join-Path $Local $scene;if(Test-Path $SceneDir){Remove-Item $SceneDir -Recurse -Force}
 $script:SceneRect=$rect;$script:SceneFps=$fps
 [GifRec]::Start($rect[0],$rect[1],$rect[2],$rect[3],$fps,$SceneDir)
}
function Stop-Rec([string]$scene,[hashtable]$extra=@{}){
 [GifRec]::Stop()
 $target=Join-Path $Out $scene;if(Test-Path $target){Remove-Item $target -Recurse -Force};New-Item -ItemType Directory $target|Out-Null
 Copy-Item "$SceneDir\*.png" $target
 $info=[ordered]@{scene=$scene;rect=$SceneRect;fps=$SceneFps;frames=[GifRec]::Frames;missed=[GifRec]::Missed;cursorHidden=[GifRec]::CursorHidden;foreground=[GifRec]::Cls([GifRec]::GetForegroundWindow());time=(Get-Date).ToString('o')}
 foreach($k in $extra.Keys){$info[$k]=$extra[$k]}
 $info|ConvertTo-Json -Depth 5|Set-Content "$target\scene.json" -Encoding UTF8
 Note "scene $scene frames=$([GifRec]::Frames) missed=$([GifRec]::Missed) cursorHidden=$([GifRec]::CursorHidden)"
}
