// Native Snap, part of Win10 Native Patch.
// Half-screen arithmetic and eligibility approach adapted from RectangleWin:
// Copyright 2022 Ahmet Alp Balkan, Apache-2.0. See vendor/rectanglewin-reference.
// RectangleWin's eligibility filter cites Microsoft PowerToys, MIT licensed.
// The state machine, frame compensation, tray and verification below are new.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kClass[] = L"Win10NativePatch.NativeSnap.v1";
constexpr wchar_t kTestClass[] = L"Win10NativePatch.NativeSnap.Test.v1";
constexpr UINT kTrayMessage = WM_APP + 11;
constexpr UINT kTrayId = 1;
constexpr UINT kPause = 101, kHelp = 102, kExit = 103;
constexpr UINT kKeys[] = {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN};
enum class Action { Left, Right, Up, Down };
struct State { DWORD pid{}; RECT last{}; SIZE restore{}; int side{}; bool maximized{}, captureOnRestore{}; };
struct Bounds { RECT outer{}, frame{}, work{}, monitor{}; HMONITOR display{}; };
std::map<HWND, State> states;
HWND mainWindow{};
NOTIFYICONDATA tray{sizeof(tray)};
UINT taskbarCreated{};
bool paused = false, registered = false, arranging = false;
SIZE testMinimum{};
bool testNoFrame=false;
std::wstring lastError;
using IsWindowArrangedFn = BOOL(WINAPI*)(HWND);
IsWindowArrangedFn isWindowArranged{};
template<class T> T Procedure(const char* name) {
    FARPROC address=GetProcAddress(GetModuleHandle(L"user32.dll"),name);
    T result{};static_assert(sizeof(result)==sizeof(address));
    std::memcpy(&result,&address,sizeof(result));return result;
}

long Width(const RECT& r) { return r.right - r.left; }
long Height(const RECT& r) { return r.bottom - r.top; }
bool Near(const RECT& a, const RECT& b, int e = 3) {
    return abs(a.left-b.left)<=e && abs(a.top-b.top)<=e &&
           abs(a.right-b.right)<=e && abs(a.bottom-b.bottom)<=e;
}
RECT Half(const RECT& w, bool right) {
    // Share one split coordinate: odd-sized work areas never leave a gap.
    long middle = w.left + Width(w)/2;
    return right ? RECT{middle,w.top,w.right,w.bottom} : RECT{w.left,w.top,middle,w.bottom};
}
RECT Center(const RECT& work, SIZE size) {
    long width = std::clamp(size.cx, 1L, Width(work));
    long height = std::clamp(size.cy, 1L, Height(work));
    long left = work.left + (Width(work)-width)/2;
    long top = work.top + (Height(work)-height)/2;
    return RECT{left,top,left+width,top+height};
}
bool ReadBounds(HWND h, Bounds& b) {
    b.display=MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!b.display || !GetMonitorInfo(b.display,&info) || !GetWindowRect(h,&b.outer)) return false;
    b.work=info.rcWork; b.monitor=info.rcMonitor; b.frame=b.outer;
    RECT frame{};
    if (SUCCEEDED(DwmGetWindowAttribute(h,DWMWA_EXTENDED_FRAME_BOUNDS,&frame,sizeof(frame))) &&
        Width(frame)>0 && Height(frame)>0) b.frame=frame;
    return Width(b.work)>0 && Height(b.work)>0;
}
bool Responsive(HWND h) {
    DWORD_PTR result{};
    return SendMessageTimeout(h,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,150,&result)!=0;
}
bool Eligible(HWND h, bool test = false) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h) ||
        GetAncestor(h,GA_ROOT)!=h || h==GetDesktopWindow() || h==GetShellWindow()) return false;
    LONG_PTR style=GetWindowLongPtr(h,GWL_STYLE), ex=GetWindowLongPtr(h,GWL_EXSTYLE);
    if (!(style&WS_THICKFRAME) || (style&(WS_CHILD|WS_DISABLED)) ||
        (ex&WS_EX_TOOLWINDOW) || (!test && (ex&WS_EX_NOACTIVATE))) return false;
    HWND owner=GetWindow(h,GW_OWNER);
    if (owner && IsWindowVisible(owner)) return false;
    wchar_t name[256]{};
    GetClassName(h,name,256);
    const wchar_t* excluded[]={L"#32770",L"SysListView32",L"WorkerW",L"Progman",
        L"Shell_TrayWnd",L"Shell_SecondaryTrayWnd",L"MultitaskingViewFrame"};
    for (const auto c:excluded) if (!_wcsicmp(c,name)) return false;
    BOOL cloaked{};
    if (SUCCEEDED(DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cloaked,sizeof(cloaked))) && cloaked) return false;
    Bounds b;
    if (!ReadBounds(h,b)) return false;
    // Borderless/fullscreen surfaces belong to their application, particularly games.
    if (!(style&WS_CAPTION) && Near(b.frame,b.monitor,2)) return false;
    return Responsive(h);
}
bool Minimum(HWND h, SIZE& size) {
    MINMAXINFO info{};
    DWORD_PTR result{};
    if (!SendMessageTimeout(h,WM_GETMINMAXINFO,0,reinterpret_cast<LPARAM>(&info),
                            SMTO_ABORTIFHUNG|SMTO_BLOCK,150,&result)) return false;
    size={std::max(1L,info.ptMinTrackSize.x),std::max(1L,info.ptMinTrackSize.y)};
    return true;
}
void Pump() {
    MSG message;
    while (PeekMessage(&message,nullptr,0,0,PM_REMOVE)) {
        if (message.message==WM_QUIT) {PostQuitMessage(static_cast<int>(message.wParam));break;}
        TranslateMessage(&message); DispatchMessage(&message);
    }
}
bool WaitState(HWND h, bool maximized) {
    ULONGLONG deadline=GetTickCount64()+500;
    do {
        Pump();
        if (!IsWindow(h)) return false;
        if (!!IsZoomed(h)==maximized && (maximized || !isWindowArranged || !isWindowArranged(h))) return true;
        Sleep(10);
    } while (GetTickCount64()<deadline);
    return false;
}
bool Restore(HWND h) {
    if (IsZoomed(h) || (isWindowArranged && isWindowArranged(h))) {
        ShowWindowAsync(h,SW_RESTORE);
        if (!WaitState(h,false)) return false;
    }
    return true;
}
SIZE RestoreSize(HWND h, const Bounds& b, bool tiled) {
    long width=Width(b.frame), height=Height(b.frame);
    if (IsZoomed(h) || tiled) {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(h,&placement)) {
            width=Width(placement.rcNormalPosition)-(Width(b.outer)-Width(b.frame));
            height=Height(placement.rcNormalPosition)-(Height(b.outer)-Height(b.frame));
        }
    }
    // An external snap may have overwritten its normal rect; use a useful floating size.
    if (width<100 || width>Width(b.work)*85/100 || height<80 || height>Height(b.work)*85/100 ||
        (tiled && abs(width-Width(b.work)/2)<5 && abs(height-Height(b.work))<5)) {
        width=Width(b.work)*7/10; height=Height(b.work)*7/10;
    }
    return SIZE{width,height};
}
bool MoveFrame(HWND h, RECT wanted, Bounds& actual) {
    if (!Restore(h) || !ReadBounds(h,actual)) return false;
    SIZE minimum;
    if (!Minimum(h,minimum)) return false;
    long extraX=Width(actual.outer)-Width(actual.frame), extraY=Height(actual.outer)-Height(actual.frame);
    if (Width(wanted)+extraX<minimum.cx || Height(wanted)+extraY<minimum.cy) {
        long width=std::max(Width(wanted),minimum.cx-extraX);
        long height=std::max(Height(wanted),minimum.cy-extraY);
        if (width>Width(actual.work) || height>Height(actual.work)) {
            lastError=L"This window's minimum size is larger than the available desktop."; return false;
        }
        bool right=wanted.right==actual.work.right;
        bool left=wanted.left==actual.work.left;
        bool top=wanted.top==actual.work.top;
        wanted=Center(actual.work,SIZE{width,height});
        if (right) {wanted.right=actual.work.right; wanted.left=wanted.right-width;}
        else if (left) {wanted.left=actual.work.left; wanted.right=wanted.left+width;}
        if (top) {wanted.top=actual.work.top; wanted.bottom=wanted.top+height;}
    }
    // Match the visible DWM frame, including custom title bars and invisible resize margins.
    for (int attempt=0;attempt<3;attempt++) {
        if (!ReadBounds(h,actual)) return false;
        RECT out{wanted.left-(actual.frame.left-actual.outer.left),
            wanted.top-(actual.frame.top-actual.outer.top),
            wanted.right+(actual.outer.right-actual.frame.right),
            wanted.bottom+(actual.outer.bottom-actual.frame.bottom)};
        if (!SetWindowPos(h,nullptr,out.left,out.top,Width(out),Height(out),
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS|SWP_NOOWNERZORDER)) return false;
        ULONGLONG deadline=GetTickCount64()+220;
        do {
            Pump();
            if (!ReadBounds(h,actual)) return false;
            if (Near(actual.frame,wanted,2)) return true;
            Sleep(10);
        } while (GetTickCount64()<deadline);
    }
    lastError=L"The application did not accept the requested window position.";
    return false;
}
bool Apply(HWND h, Action action, bool test=false) {
    lastError.clear();
    if (!Eligible(h,test)) {lastError=L"The selected window cannot be arranged safely."; return false;}
    Bounds b;
    if (!ReadBounds(h,b)) return false;
    DWORD pid{}; GetWindowThreadProcessId(h,&pid);
    int side=Near(b.frame,Half(b.work,false))?-1:Near(b.frame,Half(b.work,true))?1:0;
    auto found=states.find(h);
    if (found!=states.end() && (found->second.pid!=pid ||
        found->second.maximized!=!!IsZoomed(h) || !Near(found->second.last,b.frame))) {
        states.erase(found); found=states.end();
    }
    if (found==states.end()) {
        State s; s.pid=pid; s.last=b.frame; s.restore=RestoreSize(h,b,side!=0);
        s.captureOnRestore=IsZoomed(h)||(isWindowArranged&&isWindowArranged(h));
        s.side=side; s.maximized=IsZoomed(h); states[h]=s;
    } else if (!side) side=found->second.side;
    State& s=states[h];
    if (action==Action::Up) {
        if (!(GetWindowLongPtr(h,GWL_STYLE)&WS_MAXIMIZEBOX)) return false;
        if (!IsZoomed(h)) ShowWindowAsync(h,SW_MAXIMIZE);
        if (!WaitState(h,true) || !ReadBounds(h,b)) return false;
        s.maximized=true; s.side=0; s.last=b.frame; return true;
    }
    // A window first seen maximized/native-snapped has different non-client
    // margins. Measure its real restored frame rather than subtracting those
    // maximized margins from WINDOWPLACEMENT's normal outer rectangle.
    if (s.captureOnRestore) {
        if (!Restore(h)||!ReadBounds(h,b)) return false;
        s.restore=RestoreSize(h,b,false);s.captureOnRestore=false;
    }
    bool center=action==Action::Down || (action==Action::Right&&side<0) || (action==Action::Left&&side>0);
    RECT target=center?Center(b.work,s.restore):Half(b.work,action==Action::Right);
    if (!MoveFrame(h,target,b)) return false;
    s.side=center?0:(action==Action::Left?-1:1); s.maximized=false; s.last=b.frame;
    // Keep original dimensions, including across repeated Down and maximize/restore.
    for (auto i=states.begin();i!=states.end();) {
        if (!IsWindow(i->first)) i=states.erase(i); else ++i;
    }
    return true;
}
std::string RectJson(const RECT& r) {
    return "["+std::to_string(r.left)+","+std::to_string(r.top)+","+
           std::to_string(r.right)+","+std::to_string(r.bottom)+"]";
}
bool WriteUtf8(const std::wstring& file, const std::string& text) {
    HANDLE output=CreateFile(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (output==INVALID_HANDLE_VALUE) return false;
    DWORD written{}; bool ok=WriteFile(output,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size();
    CloseHandle(output); return ok;
}
void Output(const std::string& text) {
    HANDLE out=GetStdHandle(STD_OUTPUT_HANDLE); DWORD n;
    if (out && out!=INVALID_HANDLE_VALUE) WriteFile(out,text.data(),static_cast<DWORD>(text.size()),&n,nullptr);
}
bool RegisterKeys(HWND h, bool silent=false) {
    for (int i=0;i<4;i++) {
        if (!RegisterHotKey(h,1+i,MOD_CONTROL|MOD_ALT|MOD_WIN|MOD_NOREPEAT,kKeys[i])) {
            for (int j=0;j<i;j++) UnregisterHotKey(h,1+j);
            lastError=L"Ctrl+Alt+Win+Arrow is already in use. Close the other window shortcut utility and try again.";
            if (!silent) MessageBox(h,lastError.c_str(),L"Native Snap could not start",MB_ICONERROR|MB_OK);
            return false;
        }
    }
    registered=true; return true;
}
void UnregisterKeys(HWND h) {
    if (registered) for (int i=0;i<4;i++) UnregisterHotKey(h,1+i);
    registered=false;
}
void Tray(bool add) {
    tray.hWnd=mainWindow; tray.uID=kTrayId;
    tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;
    tray.uCallbackMessage=kTrayMessage;
    tray.hIcon=static_cast<HICON>(LoadImage(GetModuleHandle(nullptr),MAKEINTRESOURCE(paused?102:101),IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
    lstrcpyn(tray.szTip,paused?L"Native Snap — paused":L"Native Snap — active",128);
    bool success=Shell_NotifyIcon(add?NIM_ADD:NIM_MODIFY,&tray)!=FALSE;
    if (add && success) {tray.uVersion=NOTIFYICON_VERSION_4; Shell_NotifyIcon(NIM_SETVERSION,&tray);}
    if (success) KillTimer(mainWindow,1);else SetTimer(mainWindow,1,5000,nullptr);
}
void Help(HWND h) {
    MessageBox(h,L"Native Snap\n\nCtrl + Alt + Windows + Arrow\n\nLeft / Right: snap to that half. Repeating stays there.\nThe opposite direction restores a centered floating window.\nUp: maximize. Down: restore and center, never minimize.\n\nYour configured three-finger swipes use the same actions.\n\nPause releases the shortcuts. Run the patch uninstaller to restore your prior touchpad settings.",
        L"Native Snap",MB_OK|MB_ICONINFORMATION);
}
void Menu(HWND h) {
    HMENU menu=CreatePopupMenu();
    AppendMenu(menu,MF_STRING,kPause,paused?L"Resume snapping":L"Pause snapping");
    AppendMenu(menu,MF_STRING,kHelp,L"How to use Native Snap");
    AppendMenu(menu,MF_SEPARATOR,0,nullptr); AppendMenu(menu,MF_STRING,kExit,L"Exit");
    POINT p; GetCursorPos(&p); SetForegroundWindow(h);
    UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,h,nullptr);
    DestroyMenu(menu); PostMessage(h,WM_NULL,0,0);
    if (command) SendMessage(h,WM_COMMAND,command,0);
}
LRESULT CALLBACK WindowProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    if (message==taskbarCreated && taskbarCreated) {Tray(true);return 0;}
    switch(message) {
    case WM_HOTKEY:
        if (!paused && !arranging && w>=1 && w<=4) {
            arranging=true;
            if (!Apply(GetForegroundWindow(),static_cast<Action>(w-1))) MessageBeep(MB_ICONWARNING);
            arranging=false;
        }
        return 0;
    case kTrayMessage:
        if (LOWORD(l)==WM_CONTEXTMENU || LOWORD(l)==WM_RBUTTONUP || LOWORD(l)==NIN_SELECT || LOWORD(l)==NIN_KEYSELECT) Menu(h);
        else if (LOWORD(l)==WM_LBUTTONDBLCLK) Help(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w)==kPause) {
            if (!paused) {UnregisterKeys(h);paused=true;}
            else if (RegisterKeys(h)) paused=false;
            Tray(false);
        } else if (LOWORD(w)==kHelp) Help(h);
        else if (LOWORD(w)==kExit) DestroyWindow(h);
        return 0;
    case WM_CLOSE: DestroyWindow(h);return 0;
    case WM_DESTROY: UnregisterKeys(h);Shell_NotifyIcon(NIM_DELETE,&tray);PostQuitMessage(0);return 0;
    case WM_DISPLAYCHANGE: states.clear();return 0;
    case WM_TIMER: if(w==1)Tray(true);return 0;
    }
    return DefWindowProc(h,message,w,l);
}
LRESULT CALLBACK TestProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m==WM_NCCALCSIZE && testNoFrame) return 0;
    if (m==WM_GETMINMAXINFO && GetWindowLongPtr(h,GWLP_USERDATA)) {
        auto info=reinterpret_cast<MINMAXINFO*>(l);
        info->ptMinTrackSize={testMinimum.cx,testMinimum.cy};return 0;
    }
    return DefWindowProc(h,m,w,l);
}
bool ParseAction(const std::wstring& text, Action& action) {
    if (text==L"left") action=Action::Left;
    else if(text==L"right") action=Action::Right;
    else if(text==L"up") action=Action::Up;
    else if(text==L"down") action=Action::Down;
    else return false;
    return true;
}
int SelfTest(HINSTANCE instance, const std::wstring& path) {
    HWND originalForeground=GetForegroundWindow();
    std::vector<std::pair<std::string,bool>> checks;
    auto check=[&](const char* name,bool success){checks.emplace_back(name,success);};
    RECT synthetic{-1921,-100,0,981};
    RECT left=Half(synthetic,false), right=Half(synthetic,true);
    check("negative_odd_half_partition",left.left==-1921 && left.right==right.left && right.right==0 && Width(left)+Width(right)==1921);
    RECT centered=Center(synthetic,SIZE{801,501});
    check("negative_odd_center",Width(centered)==801 && Height(centered)==501 && centered.left==-1361 && centered.top==190);
    WNDCLASS wc{};wc.hInstance=instance;wc.lpfnWndProc=TestProc;wc.lpszClassName=kTestClass;
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    bool registeredClass=RegisterClass(&wc)!=0;
    HWND h=registeredClass?CreateWindowEx(WS_EX_NOACTIVATE,kTestClass,L"Native Snap isolated verification",
        WS_OVERLAPPEDWINDOW,100,100,800,550,nullptr,nullptr,instance,nullptr):nullptr;
    check("isolated_test_window_created",h!=nullptr);
    int monitorCount=0;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT,LPARAM data)->BOOL{++*reinterpret_cast<int*>(data);return TRUE;},reinterpret_cast<LPARAM>(&monitorCount));
    Bounds before{},after{};
    if(h) {
        ShowWindow(h,SW_SHOWNOACTIVATE);UpdateWindow(h);Pump();ReadBounds(h,before);
        SIZE original{Width(before.frame),Height(before.frame)};
        check("left_matches_visible_workarea",Apply(h,Action::Left,true)&&ReadBounds(h,after)&&Near(after.frame,Half(after.work,false),2));
        RECT first=after.frame;
        check("repeated_left_is_stable",Apply(h,Action::Left,true)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("opposite_restores_center",Apply(h,Action::Right,true)&&ReadBounds(h,after)&&Near(after.frame,Center(after.work,original),2));
        check("next_right_snaps_right",Apply(h,Action::Right,true)&&ReadBounds(h,after)&&Near(after.frame,Half(after.work,true),2));
        check("down_restores_without_minimize",Apply(h,Action::Down,true)&&!IsIconic(h)&&ReadBounds(h,after)&&Near(after.frame,Center(after.work,original),2));
        first=after.frame;
        check("repeated_down_is_stable",Apply(h,Action::Down,true)&&!IsIconic(h)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("up_maximizes",Apply(h,Action::Up,true)&&IsZoomed(h));
        check("repeated_up_stays_maximized",Apply(h,Action::Up,true)&&IsZoomed(h));
        check("down_from_maximized_restores_size",Apply(h,Action::Down,true)&&!IsIconic(h)&&!IsZoomed(h)&&ReadBounds(h,after)&&Near(after.frame,Center(after.work,original),2));
        // Invalidate state through an external move of this isolated window.
        SetWindowPos(h,nullptr,before.work.left+60,before.work.top+70,700,480,SWP_NOACTIVATE|SWP_NOZORDER);Pump();ReadBounds(h,after);
        SIZE manual{Width(after.frame),Height(after.frame)};
        check("manual_move_invalidates_old_restore",Apply(h,Action::Left,true)&&Apply(h,Action::Right,true)&&ReadBounds(h,after)&&Near(after.frame,Center(after.work,manual),2));
        ShowWindowAsync(h,SW_MAXIMIZE);WaitState(h,true);states.erase(h);
        check("previously_maximized_window_restores_real_frame",Apply(h,Action::Down,true)&&ReadBounds(h,after)&&Near(after.frame,Center(after.work,manual),2));
        // Simulate an external window manager placing it in a half, without stored state.
        states.erase(h);Bounds external;MoveFrame(h,Half(after.work,false),external);states.erase(h);
        check("external_half_opposite_centers",Apply(h,Action::Right,true)&&ReadBounds(h,after)&&!Near(after.frame,Half(after.work,true))&&abs((after.frame.left+after.frame.right)-(after.work.left+after.work.right))<=1);
        SetWindowLongPtr(h,GWLP_USERDATA,1);
        testMinimum={Width(before.work)*6/10,Height(before.work)*8/10};
        check("application_minimum_is_respected",Apply(h,Action::Down,true)&&ReadBounds(h,after)&&Width(after.outer)>=testMinimum.cx&&Height(after.outer)>=testMinimum.cy);
        first=after.frame;
        check("minimum_size_repeated_down_is_stable",Apply(h,Action::Down,true)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        SetWindowLongPtr(h,GWLP_USERDATA,0);
        LONG_PTR style=GetWindowLongPtr(h,GWL_STYLE);
        SetWindowLongPtr(h,GWL_STYLE,style&~WS_THICKFRAME);
        check("nonresizable_window_is_rejected",!Eligible(h,true));
        SetWindowLongPtr(h,GWL_STYLE,WS_POPUP|WS_VISIBLE);
        SetWindowPos(h,nullptr,after.monitor.left,after.monitor.top,Width(after.monitor),Height(after.monitor),SWP_NOACTIVATE|SWP_NOZORDER|SWP_FRAMECHANGED);
        check("borderless_fullscreen_is_rejected",!Eligible(h,true));
        SetWindowLongPtr(h,GWL_STYLE,(style&~WS_CAPTION)|WS_THICKFRAME);
        testNoFrame=true;
        SetWindowPos(h,nullptr,before.work.left+50,before.work.top+50,700,500,SWP_NOACTIVATE|SWP_NOZORDER|SWP_FRAMECHANGED);states.erase(h);
        check("frameless_resizable_window_is_frame_aware",Apply(h,Action::Left,true)&&ReadBounds(h,after)&&Near(after.frame,Half(after.work,false),2));
        DestroyWindow(h);states.clear();
        testNoFrame=false;
    }
    check("foreground_window_untouched",GetForegroundWindow()==originalForeground||!IsWindow(originalForeground));
    if(registeredClass)UnregisterClass(kTestClass,instance);
    bool success=std::all_of(checks.begin(),checks.end(),[](const auto& c){return c.second;});
    std::ostringstream json;
    json<<"{\n  \"success\": "<<(success?"true":"false")<<",\n  \"monitorCount\": "<<monitorCount
        <<",\n  \"physicalGesturesTested\": false,\n  \"workArea\": "<<RectJson(before.work)<<",\n  \"checks\": [\n";
    for(size_t i=0;i<checks.size();i++)json<<"    {\"name\": \""<<checks[i].first<<"\", \"passed\": "<<(checks[i].second?"true":"false")<<"}"<<(i+1<checks.size()?",":"")<<"\n";
    json<<"  ]\n}\n";
    return WriteUtf8(path,json.str())?(success?0:1):2;
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    auto setDpi=Procedure<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>("SetProcessDpiAwarenessContext");
    if(setDpi)setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);else SetProcessDPIAware();
    isWindowArranged=Procedure<IsWindowArrangedFn>("IsWindowArranged");
    int argc{};LPWSTR* argv=CommandLineToArgvW(GetCommandLine(),&argc);
    std::vector<std::wstring> args;
    if(argv){for(int i=1;i<argc;i++)args.emplace_back(argv[i]);LocalFree(argv);}
    if(!args.empty()) {
        if(args[0]==L"--stop") {
            HWND h=FindWindow(kClass,nullptr);if(!h)return 0;
            DWORD pid{};GetWindowThreadProcessId(h,&pid);
            HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
            if(!PostMessage(h,WM_CLOSE,0,0)){if(process)CloseHandle(process);return 1;}
            if(!process)return 1;
            bool stopped=WaitForSingleObject(process,3000)==WAIT_OBJECT_0;CloseHandle(process);return stopped?0:1;
        }
        if(args[0]==L"--check-hotkeys") {bool ok=RegisterKeys(nullptr,true);UnregisterKeys(nullptr);Output(ok?"{\"available\":true}\n":"{\"available\":false}\n");return ok?0:1;}
        if(args[0]==L"--self-test"&&args.size()==2)return SelfTest(instance,args[1]);
        if(args[0]==L"--probe"&&args.size()==2) {
            wchar_t* end{};auto value=wcstoull(args[1].c_str(),&end,0);Bounds b;HWND h=reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
            if(!end||*end||!value||!ReadBounds(h,b))return 2;
            Output("{\"hwnd\":"+std::to_string(value)+",\"eligible\":"+(Eligible(h)?"true":"false")+",\"maximized\":"+(IsZoomed(h)?"true":"false")+",\"outer\":"+RectJson(b.outer)+",\"visibleFrame\":"+RectJson(b.frame)+",\"workArea\":"+RectJson(b.work)+"}\n");return 0;
        }
        if(args[0]==L"--apply"&&args.size()==3) {
            Action action;wchar_t* end{};auto value=wcstoull(args[2].c_str(),&end,0);
            if(!ParseAction(args[1],action)||!end||*end||!value)return 2;
            bool ok=Apply(reinterpret_cast<HWND>(static_cast<uintptr_t>(value)),action);
            Output(ok?"{\"success\":true}\n":"{\"success\":false}\n");return ok?0:1;
        }
        return 2;
    }
    HANDLE mutex=CreateMutex(nullptr,FALSE,L"Local\\Win10NativePatch.NativeSnap.v1");
    if(!mutex)return 2;
    if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(mutex);return 0;}
    WNDCLASS wc{};wc.hInstance=instance;wc.lpfnWndProc=WindowProc;wc.lpszClassName=kClass;
    wc.hIcon=LoadIcon(instance,MAKEINTRESOURCE(101));
    if(!RegisterClass(&wc)){CloseHandle(mutex);return 2;}
    mainWindow=CreateWindowEx(WS_EX_TOOLWINDOW,kClass,L"Native Snap",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,nullptr);
    if(!mainWindow){CloseHandle(mutex);return 2;}
    if(!RegisterKeys(mainWindow)){DestroyWindow(mainWindow);CloseHandle(mutex);return 1;}
    taskbarCreated=RegisterWindowMessage(L"TaskbarCreated");Tray(true);
    MSG message;int result;
    while((result=GetMessage(&message,nullptr,0,0))>0){TranslateMessage(&message);DispatchMessage(&message);}
    CloseHandle(mutex);return result<0?2:0;
}
