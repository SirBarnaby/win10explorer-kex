#include <windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "SnapLayout.h"

namespace {
HWND windows[2]{};
bool externalFrames=false;
using Arranged=BOOL(WINAPI*)(HWND);
Arranged arranged{};
RECT work{};
HWND initialForeground{};
std::vector<std::string> trace;
std::atomic<bool> sizing=false;
std::atomic<LONG> releasedBoundary{};
struct MotionSample { RECT frame; ULONGLONG ms; };
struct MotionRun { std::string name; ULONGLONG ms; unsigned middle; };
bool measuring=false;
ULONGLONG motionStart{};
std::vector<MotionSample> motionSamples;
std::vector<MotionRun> motionRuns;
RECT Frame(HWND h);
std::string Rect(const RECT& r);
void CALLBACK ExternalEvent(HWINEVENTHOOK,DWORD event,HWND window,LONG object,LONG,DWORD,DWORD){
    if(window!=windows[0]&&window!=windows[1])return;
    if(event==EVENT_SYSTEM_MOVESIZESTART)sizing=true;
    else if(event==EVENT_SYSTEM_MOVESIZEEND)sizing=false;
    else if(event==EVENT_OBJECT_LOCATIONCHANGE&&object==OBJID_WINDOW&&measuring&&window==windows[0])
        motionSamples.push_back({Frame(window),GetTickCount64()-motionStart});
}
bool HeaderVisible(HWND frame){
    bool visible=false;
    EnumChildWindows(frame,[](HWND child,LPARAM value)->BOOL{
        wchar_t name[128]{};GetClassNameW(child,name,128);
        if(!wcsncmp(name,L"Win10NativePatch.ExplorerTabs.v2",32)&&IsWindowVisible(child))
            *reinterpret_cast<bool*>(value)=true;
        return TRUE;
    },reinterpret_cast<LPARAM>(&visible));
    return visible&&GetPropW(frame,L"Win10NativePatch.TabsActive.v2");
}
void Pump(unsigned ms){
    ULONGLONG until=GetTickCount64()+ms;
    do{MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}while(GetTickCount64()<until);
}
LRESULT CALLBACK WindowProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_ENTERSIZEMOVE)sizing=true;if(m==WM_EXITSIZEMOVE)sizing=false;
    LRESULT result=DefWindowProcW(h,m,w,l);
    if(measuring&&h==windows[0]&&m==WM_WINDOWPOSCHANGED)motionSamples.push_back({Frame(h),GetTickCount64()-motionStart});
    return result;
}
RECT Frame(HWND h){RECT r{};if(FAILED(DwmGetWindowAttribute(h,DWMWA_EXTENDED_FRAME_BOUNDS,&r,sizeof(r))))GetWindowRect(h,&r);return r;}
bool Near(long a,long b){return abs(a-b)<=3;}
// Waits for a state instead of a fixed time: returns as soon as `condition`
// holds, or false once `ms` has passed. The old fixed wait is the bound.
template<class Condition> bool WaitUntil(Condition condition,unsigned ms){
    const ULONGLONG until=GetTickCount64()+ms;
    for(;;){if(condition())return true;if(GetTickCount64()>=until)return false;Pump(10);}
}
bool SnapAssistShown(){
    wchar_t name[64]{};GetClassNameW(GetForegroundWindow(),name,64);
    return !wcscmp(name,L"MultitaskingViewFrame")||!wcscmp(name,L"ForegroundStaging");
}
bool DismissSnapAssist(){
    // Windows 10 Snap Assist offers windows for the other half after a snap.
    // A user dismisses it before acting on another window; so does the fixture.
    if(!SnapAssistShown())return false;
    INPUT esc[2]{};for(auto& i:esc){i.type=INPUT_KEYBOARD;i.ki.wVk=VK_ESCAPE;}esc[1].ki.dwFlags=KEYEVENTF_KEYUP;
    SendInput(2,esc,sizeof(INPUT));WaitUntil([]{return !SnapAssistShown();},600);Pump(50);trace.push_back("snap assist dismissed");return true;
}
// Snap Assist opens a few hundred milliseconds after a side snap. Meeting it
// here, rather than during the next command, keeps that command's focus from
// being taken mid-way (15 Nov 2026: "--apply right" cancelled by it).
void SettleSnapAssist(unsigned ms=700){if(WaitUntil(SnapAssistShown,ms))DismissSnapAssist();}
// Windows lets a process take the foreground only in some states. Joining the
// current foreground thread's input for the call is the documented way in.
bool TakeForeground(HWND h){
    HWND current=GetForegroundWindow();const DWORD self=GetCurrentThreadId(),other=current?GetWindowThreadProcessId(current,nullptr):0;
    const bool joined=other&&other!=self&&AttachThreadInput(self,other,TRUE);
    BringWindowToTop(h);SetForegroundWindow(h);
    if(joined)AttachThreadInput(self,other,FALSE);
    return WaitUntil([h]{return GetForegroundWindow()==h;},500);
}
bool ClickToFront(HWND h){
    SetWindowPos(h,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    RECT r=Frame(h);BOOL cursor=SetCursorPos((r.left+r.right)/2,(r.top+r.bottom)/2);DWORD cursorError=GetLastError();
    INPUT click[2]{};for(auto& i:click)i.type=INPUT_MOUSE;
    click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    UINT sent=SendInput(2,click,sizeof(INPUT));DWORD inputError=GetLastError();
    POINT point{};GetCursorPos(&point);HWND hit=WindowFromPoint(point);wchar_t hitClass[100]{};GetClassNameW(hit,hitClass,100);
    std::string type;for(auto ch:hitClass){if(!ch)break;type+=ch<128?static_cast<char>(ch):'?';}
    trace.push_back("click sent="+std::to_string(sent)+" error="+std::to_string(inputError)+" cursor="+std::to_string(cursor)+" cursorError="+std::to_string(cursorError)+" x="+std::to_string(point.x)+" y="+std::to_string(point.y)+" hit="+std::to_string(reinterpret_cast<ULONG_PTR>(hit))+" class="+type);
    bool focused=sent==2&&WaitUntil([h]{return GetForegroundWindow()==h;},500);
    SetWindowPos(h,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    return focused;
}
// Puts `h` in front for the next command. Up to three attempts, each recorded
// in the trace; the product checks that follow are unchanged.
bool Focus(HWND h){
    for(int attempt=1;attempt<=3;++attempt){
        DismissSnapAssist();
        BOOL cloaked{};DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
        trace.push_back("visible="+std::to_string(IsWindowVisible(h))+" enabled="+std::to_string(IsWindowEnabled(h))+" cloaked="+std::to_string(cloaked)+" style="+std::to_string(GetWindowLongPtrW(h,GWL_STYLE))+" ex="+std::to_string(GetWindowLongPtrW(h,GWL_EXSTYLE)));
        if(TakeForeground(h)||ClickToFront(h)){if(attempt>1)trace.push_back("focus attempt="+std::to_string(attempt));return true;}
        wchar_t name[64]{};GetClassNameW(GetForegroundWindow(),name,64);std::string fg;for(wchar_t c:name){if(!c)break;fg+=c<128?static_cast<char>(c):'?';}
        trace.push_back("focus attempt="+std::to_string(attempt)+" failed fg="+fg);
    }
    return false;
}
// After a command that may move `h`: returns once its frame has changed and
// then held still for 150 ms, or after `ms` when nothing moves (a check that
// expects no movement waits the full bound, as before).
void WaitSettled(HWND h,RECT before,BOOL zoomedBefore,unsigned ms){
    const ULONGLONG start=GetTickCount64();
    if(!WaitUntil([&]{RECT now=Frame(h);return !EqualRect(&now,&before)||IsZoomed(h)!=zoomedBefore;},ms))return;
    RECT last=Frame(h);ULONGLONG still=GetTickCount64();
    while(GetTickCount64()-start<ms+600){Pump(10);RECT now=Frame(h);if(!EqualRect(&now,&last)){last=now;still=GetTickCount64();}else if(GetTickCount64()-still>=150)break;}
}
bool NativeSide(HWND h,WORD key){
    bool focused=Focus(h);
    trace.push_back("focus="+std::to_string(focused)+" foreground="+std::to_string(reinterpret_cast<ULONG_PTR>(GetForegroundWindow()))+" wanted="+std::to_string(reinterpret_cast<ULONG_PTR>(h)));
    if(!focused)return false;
    INPUT in[4]{};for(auto& i:in)i.type=INPUT_KEYBOARD;
    in[0].ki.wVk=VK_LWIN;in[1].ki.wVk=key;in[2].ki.wVk=key;in[2].ki.dwFlags=KEYEVENTF_KEYUP;in[3].ki.wVk=VK_LWIN;in[3].ki.dwFlags=KEYEVENTF_KEYUP;
    RECT before=Frame(h);const BOOL zoomed=IsZoomed(h);
    UINT sent=SendInput(4,in,sizeof(INPUT));DWORD error=GetLastError();
    WaitSettled(h,before,zoomed,1000);SettleSnapAssist();
    trace.push_back("sent="+std::to_string(sent)+" error="+std::to_string(error)+" arranged="+std::to_string(arranged(h))+" zoomed="+std::to_string(IsZoomed(h)));
    return sent==4&&arranged&&arranged(h);
}
bool Place(HWND h,RECT wanted){
    RECT outer{};GetWindowRect(h,&outer);RECT frame=Frame(h);
    return SetWindowPos(h,nullptr,wanted.left-(frame.left-outer.left),wanted.top-(frame.top-outer.top),
        wanted.right-wanted.left+(outer.right-outer.left)-(frame.right-frame.left),
        wanted.bottom-wanted.top+(outer.bottom-outer.top)-(frame.bottom-frame.top),SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE;
}
bool Mouse(DWORD flags){INPUT in{};in.type=INPUT_MOUSE;in.mi.dwFlags=flags;return SendInput(1,&in,sizeof(in))==1;}
bool MovePointer(long x,long y){
    INPUT input{};input.type=INPUT_MOUSE;
    LONG width=GetSystemMetrics(SM_CXVIRTUALSCREEN)-1,height=GetSystemMetrics(SM_CYVIRTUALSCREEN)-1;
    if(width<=0||height<=0)return false;
    input.mi.dx=static_cast<LONG>((static_cast<long long>(x-GetSystemMetrics(SM_XVIRTUALSCREEN))*65535+width/2)/width);
    input.mi.dy=static_cast<LONG>((static_cast<long long>(y-GetSystemMetrics(SM_YVIRTUALSCREEN))*65535+height/2)/height);
    input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK|MOUSEEVENTF_MOVE_NOCOALESCE;
    return SendInput(1,&input,sizeof(input))==1;
}
bool SendGestureKeys(WORD arrow);
bool DragDivider(long from,long to,WORD immediate=0){
    if(!Focus(windows[0]))return false;
    const long y=work.top+(work.bottom-work.top)/2;
    SetCursorPos(work.left+80,y);Pump(200);
    RECT startLeft=Frame(windows[0]),startRight=Frame(windows[1]);
    trace.push_back("divider requestedFrom="+std::to_string(from)+" currentLeft="+std::to_string(startLeft.right)+" currentRight="+std::to_string(startRight.left)+" target="+std::to_string(to));
    from=startLeft.right-1;
    sizing=false;
    bool delivered=false;std::atomic<bool> done=false;
    std::thread input([&]{
        MovePointer(from,y);Sleep(900);
        HWND hit=WindowFromPoint(POINT{from,y});wchar_t type[128]{};GetClassNameW(hit,type,128);std::string name;for(wchar_t c:type){if(!c)break;name+=c<128?static_cast<char>(c):'?';}
        DWORD_PTR part{};SendMessageTimeoutW(hit,WM_NCHITTEST,0,MAKELPARAM(from,y),SMTO_ABORTIFHUNG,150,&part);
        trace.push_back("divider hit="+std::to_string(reinterpret_cast<ULONG_PTR>(hit))+" class="+name+" part="+std::to_string(part));
        bool down=Mouse(MOUSEEVENTF_LEFTDOWN);
        ULONGLONG ready=GetTickCount64()+1200;while(!sizing&&GetTickCount64()<ready)Sleep(5);
        Sleep(60);GUITHREADINFO gui{sizeof(gui)};GetGUIThreadInfo(0,&gui);trace.push_back("divider capture="+std::to_string(reinterpret_cast<ULONG_PTR>(gui.hwndCapture))+" sizing="+std::to_string(sizing.load()));
        for(int step=1;step<=20;++step){MovePointer(from+(to-from)*step/20,y);Sleep(20);}
        ULONGLONG settled=GetTickCount64()+500;while(abs(Frame(windows[0]).right-to)>3&&GetTickCount64()<settled)Sleep(10);
        bool up=Mouse(MOUSEEVENTF_LEFTUP);
        // The completed resize is where Windows ends the sizing loop, not the
        // frame at the button-up instant (a window may still be applying the
        // last pointer move). A gesture cannot interleave with a live drag.
        ULONGLONG ended=GetTickCount64()+1500;while(sizing&&GetTickCount64()<ended)Sleep(5);
        releasedBoundary=Frame(windows[0]).right;
        if(immediate)SendGestureKeys(immediate);
        delivered=down&&up;done=true;
    });
    // The input thread's own steps are bounded (about 4.3 s at most); wait for
    // it to finish instead of a fixed 4.4 s, then let the frames settle.
    WaitUntil([&]{return done.load();},6000);input.join();Pump(300);
    trace.push_back("divider afterLeft="+std::to_string(Frame(windows[0]).right)+" afterRight="+std::to_string(Frame(windows[1]).left));return delivered;
}
bool PairAt(long boundary){RECT a=Frame(windows[0]),b=Frame(windows[1]);return Near(a.left,work.left)&&Near(a.right,boundary)&&Near(b.left,boundary)&&Near(b.right,work.right)&&Near(a.top,work.top)&&Near(b.top,work.top)&&Near(a.bottom,work.bottom)&&Near(b.bottom,work.bottom);}
bool SharedResize(long before,long requested){
    RECT a=Frame(windows[0]),b=Frame(windows[1]);long movement=a.right-before,wanted=requested-before;
    return arranged(windows[0])&&arranged(windows[1])&&PairAt(a.right)&&
        ((wanted<0&&movement<0)||(wanted>0&&movement>0))&&abs(movement)>=abs(wanted)/2&&b.left!=before;
}
bool Product(HWND h,const wchar_t* action){
    if(!Focus(h))return false;
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);std::wstring program=module;program=program.substr(0,program.find_last_of(L"\\"))+L"\\NativeSnap.exe";
    std::wstring command=L"\""+program+L"\" --apply "+action+L" "+std::to_wstring(reinterpret_cast<ULONG_PTR>(h));
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    if(!CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return false;
    CloseHandle(process.hThread);ULONGLONG deadline=GetTickCount64()+5000;
    while(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT&&GetTickCount64()<deadline)Pump(10);
    DWORD status=2;GetExitCodeProcess(process.hProcess,&status);if(status==STILL_ACTIVE)TerminateProcess(process.hProcess,3);
    CloseHandle(process.hProcess);
    HWND fg=GetForegroundWindow();wchar_t fgClass[64]{};GetClassNameW(fg,fgClass,64);std::string fgName;for(wchar_t c:fgClass){if(!c)break;fgName+=c<128?static_cast<char>(c):'?';}
    RECT f=Frame(h);
    trace.push_back("product status="+std::to_string(status)+" arranged="+std::to_string(arranged(h))+" frame="+Rect(f)+" fg="+fgName);
    if(status==0)SettleSnapAssist();
    return status==0;
}
bool SendGestureKeys(WORD arrow){
    INPUT keys[8]{};for(auto& i:keys)i.type=INPUT_KEYBOARD;
    WORD sequence[]={VK_CONTROL,VK_MENU,VK_LWIN,arrow,arrow,VK_LWIN,VK_MENU,VK_CONTROL};
    for(int i=0;i<8;++i){keys[i].ki.wVk=sequence[i];keys[i].ki.dwFlags=(i>=4?KEYEVENTF_KEYUP:0)|((i==3||i==4)?KEYEVENTF_EXTENDEDKEY:0);}
    return SendInput(8,keys,sizeof(INPUT))==8;
}
bool Gesture(HWND h,WORD arrow){
    if(!Focus(h))return false;
    RECT before=Frame(h);const BOOL zoomed=IsZoomed(h);
    bool sent=SendGestureKeys(arrow);WaitSettled(h,before,zoomed,1100);SettleSnapAssist(400);return sent;
}
MotionRun MeasureGesture(const char* name,WORD arrow,RECT destination){
    bool focused=Focus(windows[0]);motionSamples.clear();motionStart=GetTickCount64();measuring=true;
    bool sent=focused&&SendGestureKeys(arrow),finished=false;
    while(sent&&GetTickCount64()-motionStart<2000){
        Pump(1);RECT current=Frame(windows[0]);
        if(externalFrames)motionSamples.push_back({current,GetTickCount64()-motionStart});
        if(Near(current.left,destination.left)&&Near(current.top,destination.top)&&Near(current.right,destination.right)&&Near(current.bottom,destination.bottom)){finished=true;break;}
    }
    ULONGLONG elapsed=GetTickCount64()-motionStart;measuring=false;
    unsigned middle=0;for(const auto& sample:motionSamples)if(sample.frame.left>work.left+3&&sample.frame.right<work.right-3)++middle;
    RECT finalFrame=Frame(windows[0]);
    trace.push_back(std::string(name)+" final="+std::to_string(finalFrame.left)+","+std::to_string(finalFrame.top)+","+std::to_string(finalFrame.right)+","+std::to_string(finalFrame.bottom)+" foreground="+std::to_string(reinterpret_cast<ULONG_PTR>(GetForegroundWindow()))+" arranged="+std::to_string(arranged(windows[0])));
    MotionRun run{name,finished?elapsed:2000,middle};motionRuns.push_back(run);Pump(150);SettleSnapAssist(500);return run;
}
std::string Rect(const RECT& r){return "["+std::to_string(r.left)+","+std::to_string(r.top)+","+std::to_string(r.right)+","+std::to_string(r.bottom)+"]";}
}
int wmain(int argc,wchar_t** argv){
    // argv: output [explorerFrameA explorerFrameB [--stock]]. --stock accepts
    // Explorer frames without the tab header, to isolate stock Explorer behavior.
    const bool stock=argc==5&&!_wcsicmp(argv[4],L"--stock");
    if(argc!=2&&argc!=4&&!stock)return 2;
    wchar_t user[256]{};DWORD count=256;
    if(!GetUserNameW(user,&count)||_wcsicmp(user,L"WDAGUtilityAccount"))return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    arranged=reinterpret_cast<Arranged>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"IsWindowArranged"));
    WNDCLASSW c{};c.hInstance=GetModuleHandleW(nullptr);c.lpfnWndProc=WindowProc;c.lpszClassName=L"Win10NativePatch.SnapIntegration";c.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);c.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassW(&c)||!arranged)return 2;
    MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY),&info);work=info.rcWork;
    externalFrames=argc>=4;
    WINDOWPLACEMENT originalPlacement[2]{{sizeof(WINDOWPLACEMENT)},{sizeof(WINDOWPLACEMENT)}};
    HWND originalFrames[2]{};
    if(externalFrames){
        for(int i=0;i<2;++i){
            windows[i]=reinterpret_cast<HWND>(wcstoull(argv[i+2],nullptr,10));
            wchar_t name[64]{};GetClassNameW(windows[i],name,64);
            if(wcscmp(name,L"CabinetWClass")||(!stock&&!HeaderVisible(windows[i]))||
               !GetWindowPlacement(windows[i],&originalPlacement[i]))return 2;
            originalFrames[i]=windows[i];
        }
        if(windows[0]==windows[1])return 2;
    }else for(int i=0;i<2;++i)windows[i]=CreateWindowExW(0,c.lpszClassName,i?L"Native Snap Test - Right":L"Native Snap Test - Left",WS_OVERLAPPEDWINDOW|WS_VISIBLE,work.left+80+i*200,work.top+80+i*70,650,400,nullptr,nullptr,c.hInstance,nullptr);
    HWINEVENTHOOK moveEvents{},locationEvents{};
    if(externalFrames){
        moveEvents=SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART,EVENT_SYSTEM_MOVESIZEEND,nullptr,ExternalEvent,0,0,WINEVENT_OUTOFCONTEXT);
        locationEvents=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,ExternalEvent,0,0,WINEVENT_OUTOFCONTEXT);
        if(!moveEvents||!locationEvents){if(moveEvents)UnhookWinEvent(moveEvents);if(locationEvents)UnhookWinEvent(locationEvents);return 2;}
    }
    for(HWND h:windows){ShowWindow(h,SW_SHOWNOACTIVATE);ShowWindow(h,SW_SHOWNORMAL);UpdateWindow(h);}
    Pump(300);
    initialForeground=GetForegroundWindow();
    std::vector<std::pair<std::string,bool>> checks;
    auto check=[&](const char* n,bool p){checks.emplace_back(n,p);};
    DWORD startingShare=snap::Preferences{}.Load(snap::DisplayIdentity(MonitorFromWindow(windows[0],MONITOR_DEFAULTTONEAREST)));
    ULONGLONG started=GetTickCount64();
    bool productStart=Product(windows[0],L"left")&&Product(windows[1],L"right");
    ULONGLONG productStartMs=GetTickCount64()-started;
    check("product_enters_native_snap_from_floating",productStart&&arranged(windows[0])&&arranged(windows[1]));
    check("product_initial_sides_are_complementary",PairAt(snap::Tile(work,false,0,startingShare).right));
    // Each 150 ms motion budget is judged on the median of three runs, all of
    // them recorded in motionRuns: one slow guest frame no longer fails it, a
    // slow product still does. "No restored middle" must hold on every run.
    auto restoreFloating=[&]{for(int i=0;i<2;++i){ShowWindow(windows[i],SW_RESTORE);SetWindowPos(windows[i],nullptr,work.left+80+i*200,work.top+80+i*70,650,400,SWP_NOACTIVATE|SWP_NOZORDER);}Pump(300);};
    auto median=[](std::vector<ULONGLONG> values){std::sort(values.begin(),values.end());return values[values.size()/2];};
    const RECT tileLeft=snap::Tile(work,false,0,startingShare),tileRight=snap::Tile(work,true,0,startingShare);
    std::vector<ULONGLONG> floatingMs,rightMs,leftMs,maximizedMs;unsigned rightMiddle=0,leftMiddle=0,maximizedMiddle=0;
    for(int run=0;run<3;++run){restoreFloating();floatingMs.push_back(MeasureGesture("floating_to_left",VK_LEFT,tileLeft).ms);}
    check("floating_snap_completes_within_150_ms",median(floatingMs)<150);
    for(int run=0;run<3;++run){
        auto right=MeasureGesture("left_to_right",VK_RIGHT,tileRight);rightMs.push_back(right.ms);rightMiddle+=right.middle;
        auto left=MeasureGesture("right_to_left",VK_LEFT,tileLeft);leftMs.push_back(left.ms);leftMiddle+=left.middle;
    }
    check("left_to_right_has_no_restored_middle",rightMiddle==0);
    check("left_to_right_completes_within_150_ms",median(rightMs)<150);
    check("right_to_left_has_no_restored_middle",leftMiddle==0);
    check("right_to_left_completes_within_150_ms",median(leftMs)<150);
    for(int run=0;run<3;++run){
        ShowWindow(windows[0],SW_MAXIMIZE);Pump(300);
        auto maximized=MeasureGesture("maximized_to_left",VK_LEFT,tileLeft);maximizedMs.push_back(maximized.ms);maximizedMiddle+=maximized.middle;
    }
    check("maximized_snap_has_no_restored_middle",maximizedMiddle==0);
    check("maximized_snap_completes_within_150_ms",median(maximizedMs)<150);
    for(int i=0;i<2;++i){ShowWindow(windows[i],SW_RESTORE);SetWindowPos(windows[i],nullptr,work.left+80+i*200,work.top+80+i*70,650,400,SWP_NOACTIVATE|SWP_NOZORDER);}Pump(300);
    bool native=NativeSide(windows[0],VK_LEFT)&&NativeSide(windows[1],VK_RIGHT);
    check("native_route_arranges_both_windows",native);
    long half=work.left+(work.right-work.left)/2,forty=work.left+(work.right-work.left)*4/10,sixty=work.left+(work.right-work.left)*6/10;
    check("native_pair_starts_complementary",PairAt(half));
    for(int i=0;i<2;++i){RECT o{},client{};GetWindowRect(windows[i],&o);GetClientRect(windows[i],&client);
        trace.push_back("native"+std::to_string(i)+" outer="+Rect(o)+" frame="+Rect(Frame(windows[i]))+" client="+Rect(client));}
    bool firstDrag=native&&DragDivider(Frame(windows[0]).right-1,forty);
    RECT nativeLeft=Frame(windows[0]),nativeRight=Frame(windows[1]);
    check("windows_shared_divider_resizes_both",firstDrag&&SharedResize(half,forty));
    std::wstring display=snap::DisplayIdentity(MonitorFromWindow(windows[0],MONITOR_DEFAULTTONEAREST));
    snap::Preferences preferences;
    DWORD firstShare=preferences.Load(display);
    check("native_user_resize_is_remembered",!display.empty()&&Near(snap::Tile(work,false,0,firstShare).right,nativeLeft.right));
    bool positioned=Place(windows[0],RECT{work.left,work.top,half,work.bottom})&&Place(windows[1],RECT{half,work.top,work.right,work.bottom});Pump(500);
    for(int i=0;i<2;++i){RECT o{},client{};GetWindowRect(windows[i],&o);GetClientRect(windows[i],&client);
        trace.push_back("placed"+std::to_string(i)+" outer="+Rect(o)+" frame="+Rect(Frame(windows[i]))+" client="+Rect(client));}
    check("setwindowpos_preserves_arranged_state",positioned&&arranged(windows[0])&&arranged(windows[1]));
    check("adjusted_pair_has_complementary_frames",PairAt(half));
    check("programmatic_adjustment_does_not_replace_preference",preferences.Load(display)==firstShare);
    bool secondDrag=DragDivider(half-1,sixty);
    check("adjusted_pair_keeps_windows_shared_divider",secondDrag&&SharedResize(half,sixty));
    bool product=Product(windows[0],L"left")&&Product(windows[1],L"right");
    check("product_keeps_native_arrangement",product&&arranged(windows[0])&&arranged(windows[1]));
    bool swapped=Product(windows[0],L"right")&&Product(windows[1],L"left");
    std::swap(windows[0],windows[1]);
    check("opposite_side_switches_preserve_native_pair",swapped&&arranged(windows[0])&&arranged(windows[1])&&PairAt(Frame(windows[0]).right));
    long productBoundary=Frame(windows[0]).right;
    // Products reuse the remembered boundary, which may already be 40%; always request real movement.
    const long productTarget=Near(productBoundary,forty)?sixty:forty;
    check("product_pair_keeps_shared_divider",DragDivider(productBoundary-1,productTarget)&&SharedResize(productBoundary,productTarget));
    DWORD remembered=preferences.Load(display);LONG rememberedBoundary=snap::Tile(work,false,0,remembered).right;
    check("new_command_reuses_remembered_boundary",Product(windows[0],L"down")&&Near(Frame(windows[0]).right,rememberedBoundary)&&Near(Frame(windows[0]).bottom,work.bottom));
    check("corner_round_trip_retains_native_arrangement",Product(windows[0],L"left")&&arranged(windows[0])&&Near(Frame(windows[0]).right,rememberedBoundary));
    check("complementary_side_reuses_same_boundary",Product(windows[1],L"right")&&Near(Frame(windows[1]).left,rememberedBoundary));
    check("corner_changes_do_not_replace_preference",preferences.Load(display)==remembered);
    check("receiver_up_selects_top_corner",Gesture(windows[0],VK_UP)&&Near(Frame(windows[0]).bottom,work.top+(work.bottom-work.top)/2));
    check("receiver_second_up_maximizes",Gesture(windows[0],VK_UP)&&IsZoomed(windows[0]));
    check("receiver_repeated_up_stays_maximized",Gesture(windows[0],VK_UP)&&IsZoomed(windows[0]));
    check("receiver_down_restores_pre_maximize_corner",Gesture(windows[0],VK_DOWN)&&!IsZoomed(windows[0])&&Near(Frame(windows[0]).right,rememberedBoundary)&&Near(Frame(windows[0]).bottom,work.top+(work.bottom-work.top)/2));
    check("receiver_down_selects_bottom_without_minimizing",Gesture(windows[0],VK_DOWN)&&Gesture(windows[0],VK_DOWN)&&!IsIconic(windows[0])&&Near(Frame(windows[0]).bottom,work.bottom));
    RECT aBefore=Frame(windows[0]),bBefore=Frame(windows[1]);
    bool focus=Focus(windows[0]);INPUT hold[5]{};for(auto& i:hold)i.type=INPUT_KEYBOARD;
    WORD holdKeys[]={VK_CONTROL,VK_MENU,VK_LWIN,VK_RIGHT,VK_RIGHT};for(int i=0;i<5;++i){hold[i].ki.wVk=holdKeys[i];if(i>=3)hold[i].ki.dwFlags=KEYEVENTF_EXTENDEDKEY;if(i==4)hold[i].ki.dwFlags|=KEYEVENTF_KEYUP;}
    bool held=SendInput(5,hold,sizeof(INPUT))==5;Pump(100);bool changedFocus=Focus(windows[1]);
    INPUT release[3]{};WORD releaseKeys[]={VK_LWIN,VK_MENU,VK_CONTROL};for(int i=0;i<3;++i){release[i].type=INPUT_KEYBOARD;release[i].ki.wVk=releaseKeys[i];release[i].ki.dwFlags=KEYEVENTF_KEYUP;}
    SendInput(3,release,sizeof(INPUT));Pump(1700);RECT aAfter=Frame(windows[0]),bAfter=Frame(windows[1]);
    check("focus_change_cancels_held_modifier_command",focus&&held&&changedFocus&&EqualRect(&aBefore,&aAfter)&&EqualRect(&bBefore,&bAfter));
    bool paired=Product(windows[0],L"left")&&Product(windows[1],L"right");
    bool immediate=paired&&DragDivider(Frame(windows[0]).right-1,sixty,VK_DOWN);
    LONG completedBoundary=releasedBoundary.load();RECT immediateCorner=Frame(windows[0]);
    check("immediate_corner_uses_completed_user_resize",immediate&&Near(immediateCorner.right,completedBoundary)&&Near(immediateCorner.bottom,work.bottom)&&Near(immediateCorner.top,work.top+(work.bottom-work.top)/2));
    check("immediate_user_resize_is_persisted",Near(snap::Tile(work,false,0,preferences.Load(display)).right,completedBoundary));
    if(externalFrames&&!stock)check("real_explorer_headers_survive_snap_and_divider_changes",HeaderVisible(windows[0])&&HeaderVisible(windows[1]));
    RECT afterLeft=Frame(windows[0]),afterRight=Frame(windows[1]);
    bool success=std::all_of(checks.begin(),checks.end(),[](auto& p){return p.second;});
    BOOL arrangingEnabled=FALSE;SystemParametersInfoW(SPI_GETWINARRANGING,0,&arrangingEnabled,0);
    std::ofstream out(argv[1]);out<<"{\"success\":"<<(success?"true":"false")<<",\"initialProductPairMs\":"<<productStartMs<<",\"displayIdentityKnown\":"<<(!display.empty()?"true":"false")<<",\"firstShare\":"<<firstShare<<",\"rememberedShare\":"<<remembered<<",\"winArranging\":"<<arrangingEnabled<<",\"initialForeground\":"<<reinterpret_cast<ULONG_PTR>(initialForeground)<<",\"trace\":[";
    for(size_t i=0;i<trace.size();++i)out<<(i?",":"")<<'"'<<trace[i]<<'"';
    out<<"],\"motionRuns\":[";for(size_t i=0;i<motionRuns.size();++i){if(i)out<<',';const auto& r=motionRuns[i];out<<"{\"name\":\""<<r.name<<"\",\"ms\":"<<r.ms<<",\"restoredMiddleSamples\":"<<r.middle<<'}';}
    out<<"],\"externalExplorerFrames\":"<<(externalFrames?"true":"false")
       <<",\"motionObservation\":\""<<(externalFrames?"WinEvents plus periodic10ms samples":"owned WindowProc events")<<"\""
       <<",\"work\":"<<Rect(work)<<",\"nativeDragFrames\":["<<Rect(nativeLeft)<<","<<Rect(nativeRight)<<"],\"adjustedDragFrames\":["<<Rect(afterLeft)<<","<<Rect(afterRight)<<"],\"checks\":[";
    for(size_t i=0;i<checks.size();++i)out<<(i?",":"")<<"{\"name\":\""<<checks[i].first<<"\",\"passed\":"<<(checks[i].second?"true":"false")<<"}";
    out<<"]}\n";
    if(moveEvents)UnhookWinEvent(moveEvents);if(locationEvents)UnhookWinEvent(locationEvents);
    if(externalFrames){for(int i=0;i<2;++i)if(IsWindow(originalFrames[i]))SetWindowPlacement(originalFrames[i],&originalPlacement[i]);}
    else for(HWND h:windows)if(h)DestroyWindow(h);
    return !out?2:success?0:1;
}
