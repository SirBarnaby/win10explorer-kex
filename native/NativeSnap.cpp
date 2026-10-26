// Native Snap, part of Win10 Native Patch.
// Half-screen arithmetic and eligibility approach adapted from RectangleWin:
// Copyright 2022 Ahmet Alp Balkan, Apache-2.0. See vendor/rectanglewin-reference.
// RectangleWin's eligibility filter cites Microsoft PowerToys, MIT licensed.
// The state machine, frame compensation and verification below are new.
#include "Patch.h"
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
constexpr const wchar_t* kClass = patch::SnapWindowClass;
constexpr wchar_t kTestClass[] = L"Win10NativePatch.NativeSnap.Test.v1";
enum class Action { Left, Right, Up, Down };
struct State { DWORD pid{}; RECT last{}; int side{}; bool maximized{}; };
struct Bounds { RECT outer{}, frame{}, work{}, monitor{}; HMONITOR display{}; };
std::map<HWND, State> states;
HWND mainWindow{};
bool arranging = false;
bool reloadingKeys=false;
struct Binding {int id;DWORD modifiers,key;Action action;};
std::vector<Binding> activeBindings;
HANDLE settingsEvent{};
patch::Config activeConfig;
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
RECT Quarter(const RECT& w, bool right, bool bottom) {
    RECT r=Half(w,right);long middle=w.top+Height(w)/2;
    if(bottom)r.top=middle;else r.bottom=middle;return r;
}
void DetectTile(const RECT& frame,const RECT& work,int& side,int& row) {
    side=0;row=0;
    for(int column:{-1,1}) {
        if(Near(frame,Half(work,column>0))){side=column;return;}
        for(int vertical:{-1,1})if(Near(frame,Quarter(work,column>0,vertical>0))){side=column;row=vertical;return;}
    }
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
        bool bottom=wanted.bottom==actual.work.bottom;
        wanted=Center(actual.work,SIZE{width,height});
        if (right) {wanted.right=actual.work.right; wanted.left=wanted.right-width;}
        else if (left) {wanted.left=actual.work.left; wanted.right=wanted.left+width;}
        if (top) {wanted.top=actual.work.top; wanted.bottom=wanted.top+height;}
        else if (bottom) {wanted.bottom=actual.work.bottom; wanted.top=wanted.bottom-height;}
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
    int side{},row{};DetectTile(b.frame,b.work,side,row);
    auto found=states.find(h);
    if (found!=states.end() && (found->second.pid!=pid ||
        found->second.maximized!=!!IsZoomed(h) || !Near(found->second.last,b.frame))) {
        states.erase(found); found=states.end();
    }
    if (found!=states.end() && !side) side=found->second.side;
    bool vertical=action==Action::Up||action==Action::Down;
    // Vertical gestures only refine an existing side; they do not choose a side,
    // restore to the centre, maximize, or minimize a floating window.
    if (vertical && !side) return true;
    int targetSide=vertical?side:(action==Action::Left?-1:1);
    int targetRow=vertical?(action==Action::Up?-1:1):0;
    RECT target=targetRow?Quarter(b.work,targetSide>0,targetRow>0):Half(b.work,targetSide>0);
    if (!MoveFrame(h,target,b)) return false;
    State& s=states[h];s.pid=pid;s.side=targetSide;s.maximized=false;s.last=b.frame;
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
std::vector<Binding> RequestedBindings(const patch::Config& config,bool privateActive) {
    std::vector<Binding> result;constexpr Action actions[]={Action::Left,Action::Up,Action::Right,Action::Down};
    if(privateActive)for(int i=0;i<4;i++)result.push_back({101+i,patch::GestureModifiers,patch::GestureKeys[i],actions[i]});
    if(config.flags&patch::KeyboardShortcuts)for(int i=0;i<4;i++)result.push_back({201+i,config.keyboardModifiers,config.keyboardKeys[i],actions[i]});
    return result;
}
bool RegisterSet(HWND h,const std::vector<Binding>& keys) {
    size_t i=0;for(;i<keys.size();i++)if(!RegisterHotKey(h,keys[i].id,keys[i].modifiers|MOD_NOREPEAT,keys[i].key))break;
    if(i==keys.size())return true;
    while(i)UnregisterHotKey(h,keys[--i].id);return false;
}
void PublishInputs(HWND h) {
    DWORD inputs=0;
    for(const auto& b:activeBindings) {
        if(b.id>=201)inputs|=patch::KeyboardShortcuts;
        else if(activeConfig.flags&patch::TouchpadGestures)inputs|=patch::TouchpadGestures;
    }
    SetPropW(h,L"Win10NativePatch.ActiveInputs.v2",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(inputs)));
}
bool RegisterKeys(HWND h,bool silent=false) {
    if(reloadingKeys)return false;
    struct ReloadGuard{ReloadGuard(){reloadingKeys=true;}~ReloadGuard(){reloadingKeys=false;}} guard;
    auto config=patch::LoadConfig();auto status=patch::ReadGestureStatus();auto wanted=RequestedBindings(config,status.privateBindingsActive);
    auto before=activeBindings;for(const auto& b:before)UnregisterHotKey(h,b.id);
    if(!RegisterSet(h,wanted)) {
        bool recovered=RegisterSet(h,before);activeBindings=recovered?before:std::vector<Binding>{};
        PublishInputs(h);
        lastError=recovered?L"A snapping shortcut is already in use. The previous working shortcuts were kept. Choose another combination in Options.":L"A snapping shortcut is already in use, and the previous shortcuts could not be restored. Review the conflicting shortcut utility in Options.";
        if(!silent)MessageBoxW(h,lastError.c_str(),L"Native Snap",MB_ICONERROR|MB_OK);
        return false;
    }
    activeBindings=std::move(wanted);activeConfig=config;PublishInputs(h);return true;
}
void UnregisterKeys(HWND h) {
    RemovePropW(h,L"Win10NativePatch.ActiveInputs.v2");
    for(const auto& b:activeBindings)UnregisterHotKey(h,b.id);activeBindings.clear();
}
void RemoveIdleStartup() {
    // Only remove our exact native command; never touch the v1 migration route
    // or a startup entry whose command was subsequently changed by the user.
    HKEY key{};if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE|KEY_SET_VALUE,&key)!=ERROR_SUCCESS)return;
    wchar_t command[32768];DWORD type{},size=sizeof(command);
    std::wstring expected=L"\""+patch::SiblingPath(L"NativeSnap.exe")+L"\"";
    if(RegQueryValueExW(key,L"Win10NativePatch.NativeSnap.v2",nullptr,&type,reinterpret_cast<BYTE*>(command),&size)==ERROR_SUCCESS&&type==REG_SZ&&size>=sizeof(wchar_t)&&command[size/sizeof(wchar_t)-1]==0&&!_wcsicmp(command,expected.c_str()))RegDeleteValueW(key,L"Win10NativePatch.NativeSnap.v2");
    RegCloseKey(key);
}
LRESULT CALLBACK WindowProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    if(message==patch::SettingsChangedMessage()) {
        if(arranging||reloadingKeys)return 2; // Busy, retry without changing the request.
        bool ok=RegisterKeys(h,true);
        if(ok&&activeBindings.empty()&&!(activeConfig.flags&patch::KeyboardShortcuts)){RemoveIdleStartup();PostMessageW(h,WM_CLOSE,0,0);}
        return ok?1:0;
    }
    switch(message) {
    case WM_HOTKEY:
        if (!arranging) {
            auto binding=std::find_if(activeBindings.begin(),activeBindings.end(),[&](const Binding& b){return b.id==static_cast<int>(w);});
            if(binding==activeBindings.end())return 0;Action action=binding->action;
            if(binding->id<201&&!(activeConfig.flags&patch::TouchpadGestures))return 0;
            arranging=true;
            if (!Apply(GetForegroundWindow(),action)) MessageBeep(MB_ICONWARNING);
            arranging=false;
        }
        return 0;
    case WM_CLOSE: DestroyWindow(h);return 0;
    case WM_DESTROY: UnregisterKeys(h);PostQuitMessage(0);return 0;
    case WM_DISPLAYCHANGE: states.clear();return 0;
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
    auto check=[&](const std::string& name,bool success){checks.emplace_back(name,success);};
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
        // Every destination is seeded without saved state, as if another window
        // manager had placed it there. Exercise all four actions from each one.
        struct Tile {const char* name;int side,row;};
        const Tile tiles[]={{"left_half",-1,0},{"right_half",1,0},
            {"top_left",-1,-1},{"bottom_left",-1,1},
            {"top_right",1,-1},{"bottom_right",1,1}};
        const Action actions[]={Action::Left,Action::Right,Action::Up,Action::Down};
        const char* actionNames[]={"left","right","up","down"};
        for(const auto& tile:tiles)for(size_t i=0;i<4;i++) {
            states.erase(h);Bounds seeded;
            RECT source=tile.row?Quarter(before.work,tile.side>0,tile.row>0):Half(before.work,tile.side>0);
            bool seededOk=MoveFrame(h,source,seeded);states.erase(h);
            bool vertical=actions[i]==Action::Up||actions[i]==Action::Down;
            int targetSide=vertical?tile.side:(actions[i]==Action::Left?-1:1);
            int targetRow=vertical?(actions[i]==Action::Up?-1:1):0;
            RECT expected=targetRow?Quarter(before.work,targetSide>0,targetRow>0):Half(before.work,targetSide>0);
            check(std::string("transition_")+tile.name+"_"+actionNames[i],
                seededOk&&Apply(h,actions[i],true)&&!IsIconic(h)&&!IsZoomed(h)&&
                ReadBounds(h,after)&&Near(after.frame,expected,2));
        }
        states.erase(h);Bounds floating;
        bool floatingOk=MoveFrame(h,before.frame,floating);states.erase(h);
        RECT first=floating.frame;
        check("floating_up_leaves_window_unchanged",floatingOk&&Apply(h,Action::Up,true)&&!IsIconic(h)&&!IsZoomed(h)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("floating_repeated_up_leaves_window_unchanged",Apply(h,Action::Up,true)&&!IsIconic(h)&&!IsZoomed(h)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        // Reset independently so a failed Up test cannot mask Down behavior.
        states.erase(h);floatingOk=MoveFrame(h,before.frame,floating);states.erase(h);first=floating.frame;
        check("floating_down_leaves_window_unchanged",floatingOk&&Apply(h,Action::Down,true)&&!IsIconic(h)&&!IsZoomed(h)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("floating_repeated_down_leaves_window_unchanged",Apply(h,Action::Down,true)&&!IsIconic(h)&&!IsZoomed(h)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("saved_corner_right_selects_right_half",Apply(h,Action::Left,true)&&Apply(h,Action::Down,true)&&Apply(h,Action::Right,true)&&ReadBounds(h,after)&&Near(after.frame,Half(after.work,true),2));
        check("saved_corner_left_selects_left_half",Apply(h,Action::Up,true)&&Apply(h,Action::Left,true)&&ReadBounds(h,after)&&Near(after.frame,Half(after.work,false),2));
        // Invalidate saved tile state through an external move of this window.
        SetWindowPos(h,nullptr,before.work.left+60,before.work.top+70,700,480,SWP_NOACTIVATE|SWP_NOZORDER);Pump();ReadBounds(h,after);first=after.frame;
        check("manual_move_invalidates_saved_side_for_up",Apply(h,Action::Up,true)&&!IsIconic(h)&&!IsZoomed(h)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("manual_move_invalidates_saved_side_for_down",Apply(h,Action::Down,true)&&!IsIconic(h)&&!IsZoomed(h)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("floating_right_selects_right_half",Apply(h,Action::Right,true)&&ReadBounds(h,after)&&Near(after.frame,Half(after.work,true),2));
        SetWindowLongPtr(h,GWLP_USERDATA,1);
        testMinimum={Width(before.work)*6/10,Height(before.work)*8/10};
        check("application_minimum_keeps_bottom_and_right_edges",Apply(h,Action::Down,true)&&ReadBounds(h,after)&&Width(after.outer)>=testMinimum.cx&&Height(after.outer)>=testMinimum.cy&&abs(after.frame.bottom-after.work.bottom)<=2&&abs(after.frame.right-after.work.right)<=2);
        first=after.frame;
        check("minimum_size_repeated_down_is_stable",Apply(h,Action::Down,true)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("minimum_size_horizontal_resets_to_left_half",Apply(h,Action::Left,true)&&ReadBounds(h,after)&&Width(after.outer)>=testMinimum.cx&&abs(after.frame.left-after.work.left)<=2&&abs(after.frame.top-after.work.top)<=2&&abs(after.frame.bottom-after.work.bottom)<=2);
        check("minimum_size_keeps_bottom_and_left_edges",Apply(h,Action::Down,true)&&ReadBounds(h,after)&&Width(after.outer)>=testMinimum.cx&&Height(after.outer)>=testMinimum.cy&&abs(after.frame.bottom-after.work.bottom)<=2&&abs(after.frame.left-after.work.left)<=2);
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
        check("left_down_is_bottom_left",Apply(h,Action::Down,true)&&ReadBounds(h,after)&&Near(after.frame,Quarter(after.work,false,true),2));
        first=after.frame;
        check("left_down_down_stays_visible_bottom_left",Apply(h,Action::Down,true)&&!IsIconic(h)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("left_down_left_selects_left_half",Apply(h,Action::Left,true)&&ReadBounds(h,after)&&Near(after.frame,Half(after.work,false),2));
        check("left_corner_up_is_top_left",Apply(h,Action::Up,true)&&ReadBounds(h,after)&&Near(after.frame,Quarter(after.work,false,false),2));
        first=after.frame;
        check("left_up_up_is_stable",Apply(h,Action::Up,true)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("left_corner_right_selects_right_half",Apply(h,Action::Right,true)&&ReadBounds(h,after)&&Near(after.frame,Half(after.work,true),2));
        check("right_up_is_top_right",Apply(h,Action::Up,true)&&ReadBounds(h,after)&&Near(after.frame,Quarter(after.work,true,false),2));
        first=after.frame;
        check("right_up_up_is_stable",Apply(h,Action::Up,true)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("right_up_right_selects_right_half",Apply(h,Action::Right,true)&&ReadBounds(h,after)&&Near(after.frame,Half(after.work,true),2));
        check("right_corner_down_is_bottom_right",Apply(h,Action::Down,true)&&ReadBounds(h,after)&&Near(after.frame,Quarter(after.work,true,true),2));
        first=after.frame;
        check("right_down_down_stays_visible",Apply(h,Action::Down,true)&&!IsIconic(h)&&ReadBounds(h,after)&&Near(first,after.frame,0));
        check("right_corner_left_selects_left_half",Apply(h,Action::Left,true)&&ReadBounds(h,after)&&Near(after.frame,Half(after.work,false),2));
        states.erase(h);Bounds externalCorner;
        MoveFrame(h,Quarter(after.work,false,true),externalCorner);states.erase(h);
        check("external_bottom_left_repeat_down_is_stable",Apply(h,Action::Down,true)&&!IsIconic(h)&&ReadBounds(h,after)&&Near(after.frame,Quarter(after.work,false,true),2));
        SetWindowLongPtr(h,GWLP_USERDATA,1);testMinimum={Width(before.work)*6/10,Height(before.work)*8/10};
        check("oversized_corner_minimum_keeps_bottom_and_right_edges",Apply(h,Action::Right,true)&&Apply(h,Action::Down,true)&&ReadBounds(h,after)&&Width(after.outer)>=testMinimum.cx&&Height(after.outer)>=testMinimum.cy&&abs(after.frame.bottom-after.work.bottom)<=2&&abs(after.frame.right-after.work.right)<=2);
        SetWindowLongPtr(h,GWLP_USERDATA,0);
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
        if(args[0]==L"--config-self-test"&&args.size()==2)return patch::RunConfigSelfTests(args[1]);
        if(args[0]==L"--probe-gestures"&&args.size()==2)return patch::RunGestureProbe(args[1]);
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
    HANDLE mutex=CreateMutex(nullptr,FALSE,L"Local\\Win10NativePatch.NativeSnap.v2");
    if(!mutex)return 2;
    if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(mutex);return 0;}
    WNDCLASS wc{};wc.hInstance=instance;wc.lpfnWndProc=WindowProc;wc.lpszClassName=kClass;
    wc.hIcon=LoadIcon(instance,MAKEINTRESOURCE(101));
    if(!RegisterClass(&wc)){CloseHandle(mutex);return 2;}
    mainWindow=CreateWindowEx(WS_EX_TOOLWINDOW,kClass,L"Native Snap",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,nullptr);
    if(!mainWindow){CloseHandle(mutex);return 2;}
    if(!RegisterKeys(mainWindow)){DestroyWindow(mainWindow);CloseHandle(mutex);return 1;}
    if(activeBindings.empty()){RemoveIdleStartup();DestroyWindow(mainWindow);CloseHandle(mutex);return 0;}
    settingsEvent=CreateEventW(nullptr,FALSE,FALSE,L"Local\\Win10NativePatch.SettingsChanged.v2");
    MSG message{};bool running=true;
    while(running){DWORD wait=MsgWaitForMultipleObjects(settingsEvent?1:0,settingsEvent?&settingsEvent:nullptr,FALSE,INFINITE,QS_ALLINPUT);
        if(settingsEvent&&wait==WAIT_OBJECT_0){SendMessageW(mainWindow,patch::SettingsChangedMessage(),0,0);continue;}
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT){running=false;break;}TranslateMessage(&message);DispatchMessageW(&message);}}
    if(settingsEvent)CloseHandle(settingsEvent);CloseHandle(mutex);return 0;
}
