#include "Patch.h"
#include "MvpHostProtocol.h"
#include "HeaderIntegration.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace {
constexpr UINT RefreshWindow=WM_APP+40;
HWND host{},selected{};
HMODULE extension{};
HOOKPROC hookProcedure{};
std::map<DWORD,HHOOK> hooks;
std::map<HWND,unsigned> pending;
std::set<HWND> observed;

bool ExplorerFrame(HWND window) {
    wchar_t name[64]{};
    if(!IsWindow(window)||GetAncestor(window,GA_ROOT)!=window||
       !GetClassNameW(window,name,64)||wcscmp(name,L"CabinetWClass"))return false;
    DWORD pid{},session{},ours{};GetWindowThreadProcessId(window,&pid);
    if(!ProcessIdToSessionId(pid,&session)||!ProcessIdToSessionId(GetCurrentProcessId(),&ours)||session!=ours)return false;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)return false;
    wchar_t path[32768]{},windows[MAX_PATH]{};DWORD size=32768;
    bool queried=QueryFullProcessImageNameW(process,0,path,&size)!=FALSE;CloseHandle(process);
    UINT length=GetWindowsDirectoryW(windows,MAX_PATH);
    return queried&&length&&length<MAX_PATH&&!_wcsicmp(path,(std::wstring(windows)+L"\\explorer.exe").c_str());
}
void Attach(HWND window) {
    if(!ExplorerFrame(window)||(selected&&selected!=window)||
       !(patch::LoadConfig().flags&patch::ExplorerTabs))return;
    observed.insert(window);
    if(GetPropW(window,L"Win10NativePatch.ExplorerController.v2"))return;
    DWORD thread=GetWindowThreadProcessId(window,nullptr);
    if(!hooks.count(thread)) {
        HHOOK hook=SetWindowsHookExW(WH_GETMESSAGE,hookProcedure,extension,thread);
        if(!hook)return;
        hooks.emplace(thread,hook);
    }
    PostMessageW(window,patch::mvp::AttachMessage(),reinterpret_cast<WPARAM>(host),0);
}
void Queue(HWND window) {
    if(!ExplorerFrame(window)||(selected&&selected!=window))return;
    Attach(window);
    if((patch::LoadConfig().flags&patch::ExplorerTabs)&&
       !GetPropW(window,L"Win10NativePatch.ExplorerController.v2")) {
        pending.try_emplace(window,0);
        SetTimer(host,1,200,nullptr);
    }
}
void Refresh() {
    for(auto i=observed.begin();i!=observed.end();) {
        if(!ExplorerFrame(*i))i=observed.erase(i);else ++i;
    }
    for(auto i=hooks.begin();i!=hooks.end();) {
        bool used=std::any_of(observed.begin(),observed.end(),[&](HWND window){return GetWindowThreadProcessId(window,nullptr)==i->first;});
        if(!used){UnhookWindowsHookEx(i->second);i=hooks.erase(i);}else ++i;
    }
    EnumWindows([](HWND window,LPARAM)->BOOL{Queue(window);return TRUE;},0);
}
void Created(HWND window) {
    if(ExplorerFrame(window))PostMessageW(host,RefreshWindow,reinterpret_cast<WPARAM>(window),0);
}
void Destroyed(HWND window) {
    if(observed.erase(window)) {pending.erase(window);PostMessageW(host,RefreshWindow,0,0);}
}
// The host normally wakes only for top-level windows that get a taskbar
// button (shell hook). The WinEvent range below is the fallback when the
// shell hook cannot be registered: it wakes for every object in the session.
UINT shellHook{};
// Broadcast when the desktop shell (re)starts: Explorer was restarted, or it
// came up after this starter did. Folder windows and hooks are checked again.
UINT taskbarCreated{};
void CALLBACK WindowEvent(HWINEVENTHOOK,DWORD event,HWND window,LONG object,LONG child,DWORD,DWORD) {
    if(object!=OBJID_WINDOW||child!=CHILDID_SELF)return;
    if(event==EVENT_OBJECT_DESTROY)Destroyed(window);else Created(window);
}
bool DetachAll(bool preserveSession,ULONG_PTR request) {
    // Owner callbacks can close a frame while the external call is running.
    const std::vector<HWND> windows(observed.begin(),observed.end());
    bool complete=true;
    for(HWND window:windows) {
        if(request&&reinterpret_cast<ULONG_PTR>(GetPropW(host,patch::mvp::StopRequest))!=request){complete=false;break;}
        if(!IsWindow(window))continue;
        DWORD_PTR result{};
        if(!SendMessageTimeoutW(window,patch::mvp::DetachMessage(),patch::mvp::DetachParameters(preserveSession,request),request?reinterpret_cast<LPARAM>(host):0,SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&result)||
           GetPropW(window,L"Win10NativePatch.ExplorerController.v2"))complete=false;
        RemovePropW(window,patch::mvp::AttachResult);
    }
    if(request&&reinterpret_cast<ULONG_PTR>(GetPropW(host,patch::mvp::StopRequest))!=request)complete=false;
    if(!complete){
        if(request&&reinterpret_cast<ULONG_PTR>(GetPropW(host,patch::mvp::StopRequest))==request)RemovePropW(host,patch::mvp::StopRequest);
        Refresh();return false;
    }
    for(auto [thread,hook]:hooks){(void)thread;UnhookWindowsHookEx(hook);}
    hooks.clear();pending.clear();
    return true;
}
LRESULT CALLBACK HostProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==patch::SettingsChangedMessage()) {Refresh();return 1;}
    if(taskbarCreated&&message==taskbarCreated) {
        if(shellHook)RegisterShellHookWindow(window);
        Refresh();return 0;
    }
    if(shellHook&&message==shellHook) {
        const auto target=reinterpret_cast<HWND>(l);
        switch(w&0x7FFF) {
        case HSHELL_WINDOWCREATED:Created(target);break;
        case HSHELL_WINDOWDESTROYED:Destroyed(target);break;
        default:break;
        }
        return 0;
    }
    switch(message) {
    case RefreshWindow:
        if(w)Queue(reinterpret_cast<HWND>(w));
        else if(selected&&!IsWindow(selected))PostMessageW(window,WM_CLOSE,0,0);
        else Refresh();
        return 0;
    case WM_TIMER:
        if(w==1) {
            for(auto i=pending.begin();i!=pending.end();) {
                if(!ExplorerFrame(i->first)||GetPropW(i->first,L"Win10NativePatch.ExplorerController.v2")||
                   !(patch::LoadConfig().flags&patch::ExplorerTabs)||i->second++>=10)i=pending.erase(i);
                else {Attach(i->first);++i;}
            }
            if(pending.empty())KillTimer(window,1);
        }
        return 0;
    case WM_CLOSE: {
        const auto request=static_cast<ULONG_PTR>(l);
        if((request||w==1)&&(!request||reinterpret_cast<ULONG_PTR>(GetPropW(window,patch::mvp::StopRequest))!=request))return 0;
        if(DetachAll(w==1,request)){
            if(request){
                if(!SetPropW(window,patch::mvp::StopCommitted,reinterpret_cast<HANDLE>(request))){
                    RemovePropW(window,patch::mvp::StopRequest);Refresh();return 0;
                }
                if(reinterpret_cast<ULONG_PTR>(GetPropW(window,patch::mvp::StopRequest))!=request){
                    RemovePropW(window,patch::mvp::StopCommitted);Refresh();return 0;
                }
            }
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_DESTROY:
        if(shellHook)DeregisterShellHookWindow(window);
        PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
bool WriteReport(const std::wstring& path,const std::string& text) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written{};bool ok=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size();
    CloseHandle(file);return ok;
}
void Rect(std::ostringstream& out,const RECT& r) {out<<'['<<r.left<<','<<r.top<<','<<r.right<<','<<r.bottom<<']';}
int Probe(const std::wstring& path) {
    DWORD shellPid{};GetWindowThreadProcessId(GetShellWindow(),&shellPid);
    std::ostringstream out;out<<"{\"hostRunning\":"<<(FindWindowW(patch::mvp::HostClass,nullptr)?"true":"false")<<",\"shellPid\":"<<shellPid<<",\"windows\":[";
    struct Context{std::ostringstream* out;bool first=true;}context{&out};
    EnumWindows([](HWND window,LPARAM data)->BOOL{
        if(!ExplorerFrame(window))return TRUE;
        auto& c=*reinterpret_cast<Context*>(data);auto& o=*c.out;
        if(!std::exchange(c.first,false))o<<',';
        DWORD pid{};DWORD thread=GetWindowThreadProcessId(window,&pid);
        RECT rect{},client{},buttons{};GetWindowRect(window,&rect);GetClientRect(window,&client);
        DwmGetWindowAttribute(window,DWMWA_CAPTION_BUTTON_BOUNDS,&buttons,sizeof(buttons));
        o<<"{\"hwnd\":"<<reinterpret_cast<ULONG_PTR>(window)<<",\"pid\":"<<pid<<",\"thread\":"<<thread
         <<",\"tabsActive\":"<<(GetPropW(window,patch::TabsActiveProperty)?"true":"false")
         <<",\"attachResult\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(window,patch::mvp::AttachResult))
         <<",\"legacyQtHidden\":"<<(GetPropW(window,L"Win10NativePatch.LegacyQtHidden.v1")?"true":"false")
         <<",\"rejected\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(window,L"Win10NativePatch.TabsRejected.v2"))
         <<",\"headerRowHeight\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(window,header::RowProperty))<<",\"outer\":";
        Rect(o,rect);o<<",\"client\":";Rect(o,client);o<<",\"captionButtons\":";Rect(o,buttons);o<<",\"children\":[";
        Context children{&o};
        EnumChildWindows(window,[](HWND child,LPARAM value)->BOOL{
            auto& info=*reinterpret_cast<Context*>(value);wchar_t name[128]{};GetClassNameW(child,name,128);
            std::string ascii;for(wchar_t n:name){if(!n)break;if(n==L'\\'||n==L'"')ascii+='\\';ascii+=n<128?static_cast<char>(n):'?';}
            if(!std::exchange(info.first,false))*info.out<<',';
            RECT bounds{};GetWindowRect(child,&bounds);
            *info.out<<"{\"class\":\""<<ascii<<"\",\"hwnd\":"<<reinterpret_cast<ULONG_PTR>(child)<<",\"parent\":"<<reinterpret_cast<ULONG_PTR>(GetParent(child))<<",\"rect\":";Rect(*info.out,bounds);*info.out<<'}';return TRUE;
        },reinterpret_cast<LPARAM>(&children));
        o<<"]}";return TRUE;
    },reinterpret_cast<LPARAM>(&context));
    out<<"]}\n";return WriteReport(path,out.str())?0:1;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    int count{};auto argv=CommandLineToArgvW(GetCommandLineW(),&count);
    std::vector<std::wstring> args;if(argv){for(int i=1;i<count;i++)args.emplace_back(argv[i]);LocalFree(argv);}
    if(args.size()==2&&args[0]==L"--probe")return Probe(args[1]);
    if(args.size()==1&&args[0]==L"--stop") {
        HWND target=FindWindowW(patch::mvp::HostClass,nullptr);if(!target)return 0;
        DWORD pid{};GetWindowThreadProcessId(target,&pid);HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
        bool stopped=process&&patch::mvp::StopHost(target,process,false);
        if(process)CloseHandle(process);return stopped?0:1;
    }
    if(args.size()==2&&args[0]==L"--window") {
        wchar_t* end{};auto value=wcstoull(args[1].c_str(),&end,10);
        selected=reinterpret_cast<HWND>(static_cast<ULONG_PTR>(value));
        if(!value||!end||*end||!ExplorerFrame(selected))return 2;
    } else if(!args.empty())return 2;
#ifndef NATIVE_LAB_ANY_BUILD
    if(!patch::SupportedWindows())return 3;
#endif
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\Win10NativePatch.ExplorerMvpHost.v1");
    if(!mutex)return 1;if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(mutex);return 0;}
    extension=LoadLibraryExW((patch::ModuleDirectory()+L"\\ExplorerTabs.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(extension)hookProcedure=reinterpret_cast<HOOKPROC>(GetProcAddress(extension,"MvpExplorerHook"));
    if(!hookProcedure){if(extension)FreeLibrary(extension);CloseHandle(mutex);return 4;}
    WNDCLASSW cls{};cls.hInstance=instance;cls.lpfnWndProc=HostProc;cls.lpszClassName=patch::mvp::HostClass;
    if(!RegisterClassW(&cls)){FreeLibrary(extension);CloseHandle(mutex);return 1;}
    host=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Explorer integration",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,nullptr);
    if(!host){FreeLibrary(extension);CloseHandle(mutex);return 1;}
    SetPropW(host,patch::mvp::SessionHandoffCapability,reinterpret_cast<HANDLE>(1));
    HWINEVENTHOOK events{};
    taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    shellHook=RegisterWindowMessageW(L"SHELLHOOK");
    const bool shellHooked=shellHook&&RegisterShellHookWindow(host);
    if(!shellHooked){shellHook=0;events=SetWinEventHook(EVENT_OBJECT_CREATE,EVENT_OBJECT_HIDE,nullptr,WindowEvent,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);}
    Refresh();MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    if(events)UnhookWinEvent(events);
    FreeLibrary(extension);CloseHandle(mutex);return 0;
}
