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
constexpr UINT_PTR AttachTimer=1,ShellTimer=2;
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
        SetTimer(host,AttachTimer,200,nullptr);
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
// Keep-alive. A second copy of this program (the guard) has no window and no
// timer: it sleeps on the starter's process handle and starts the starter again
// if it ends without having released the guard. The starter watches the guard
// the same way, so ending either one alone brings it back within a second.
// `released` is set on every intended stop (Options, setup, sign-out).
HANDLE guard{},released{};
unsigned strikes{},guardFailures{},shellWaits{};
ULONGLONG guardStarted{};
std::wstring OwnPath() {
    std::wstring path(32768,L'\0');const DWORD length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    path.resize(length<path.size()?length:0);return path;
}
// Starts another copy of this program. `inherit`, when given, is the only
// handle the copy receives; every other handle here stays private.
bool Launch(const std::wstring& arguments,HANDLE inherit,HANDLE* process) {
    const std::wstring app=OwnPath();if(app.empty())return false;
    std::wstring command=L"\""+app+L"\""+(arguments.empty()?L"":L" "+arguments);
    STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);start.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;start.StartupInfo.wShowWindow=SW_HIDE;
    std::vector<BYTE> attributes;SIZE_T size{};
    if(inherit) {
        InitializeProcThreadAttributeList(nullptr,1,0,&size);attributes.resize(size);
        start.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(start.lpAttributeList,1,0,&size))return false;
        if(!UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&inherit,sizeof(inherit),nullptr,nullptr)){DeleteProcThreadAttributeList(start.lpAttributeList);return false;}
    }
    PROCESS_INFORMATION info{};
    const bool started=CreateProcessW(app.c_str(),command.data(),nullptr,nullptr,inherit!=nullptr,CREATE_NO_WINDOW|(inherit?EXTENDED_STARTUPINFO_PRESENT:0),
                                      nullptr,patch::ModuleDirectory().c_str(),&start.StartupInfo,&info)!=FALSE;
    if(inherit)DeleteProcThreadAttributeList(start.lpAttributeList);
    if(!started)return false;
    CloseHandle(info.hThread);if(process)*process=info.hProcess;else CloseHandle(info.hProcess);return true;
}
// Windows is signing out or shutting down: nothing is started again.
bool Ending(){return GetSystemMetrics(SM_SHUTTINGDOWN)!=0;}
void StartGuard() {
    if(!released||guard||Ending())return;
    guardStarted=GetTickCount64();
    Launch(L"--guard "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(reinterpret_cast<ULONG_PTR>(released))+L" "+std::to_wstring(strikes),released,&guard);
}
void GuardEnded() {
    CloseHandle(guard);guard=nullptr;
    // A guard that keeps ending at once is not started a fourth time.
    guardFailures=GetTickCount64()-guardStarted<10000?guardFailures+1:0;
    if(guardFailures<3)StartGuard();
}
bool Number(const std::wstring& text,ULONG_PTR& value) {
    wchar_t* end{};value=static_cast<ULONG_PTR>(wcstoull(text.c_str(),&end,10));return !text.empty()&&end&&!*end;
}
// `--guard <starter pid> <released event> <strikes>`
int Guard(const std::vector<std::wstring>& args) {
    ULONG_PTR pid{},event{},count{};
    if(!Number(args[1],pid)||!Number(args[2],event)||!Number(args[3],count)||!pid||!event)return 2;
    HANDLE starter=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,static_cast<DWORD>(pid));
    if(!starter)return 0;
    std::wstring image(32768,L'\0');DWORD size=static_cast<DWORD>(image.size());
    if(!QueryFullProcessImageNameW(starter,0,image.data(),&size)||_wcsicmp(image.c_str(),OwnPath().c_str())){CloseHandle(starter);return 2;}
    const HANDLE handles[]{starter,reinterpret_cast<HANDLE>(event)};
    const bool ended=WaitForMultipleObjects(2,handles,FALSE,INFINITE)==WAIT_OBJECT_0&&WaitForSingleObject(handles[1],0)!=WAIT_OBJECT_0;
    FILETIME created{},exited{},kernel{},user{};
    const bool timed=ended&&GetProcessTimes(starter,&created,&exited,&kernel,&user);
    CloseHandle(starter);
    if(!ended||!(patch::LoadConfig().flags&patch::ExplorerTabs))return 0;
    // A starter that ends within half a minute of starting counts as a strike;
    // three in a row end the attempts until the next sign-in, Options or setup.
    const auto ticks=[](const FILETIME& time){return (static_cast<ULONGLONG>(time.dwHighDateTime)<<32)|time.dwLowDateTime;};
    const unsigned next=timed&&ticks(exited)-ticks(created)<300000000ull?static_cast<unsigned>(count)+1:0;
    if(next>=3)return 1;
    Sleep(next*1000);
    if(Ending())return 0;
    return Launch(L"--recovered "+std::to_wstring(next),nullptr,nullptr)?0:1;
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
        if(w==ShellTimer) {
            // Started before the desktop shell: look again once it is there.
            if(GetShellWindow()){KillTimer(window,ShellTimer);if(shellHook)RegisterShellHookWindow(window);Refresh();}
            else if(++shellWaits>=300)KillTimer(window,ShellTimer);
            return 0;
        }
        if(w==AttachTimer) {
            for(auto i=pending.begin();i!=pending.end();) {
                if(!ExplorerFrame(i->first)||GetPropW(i->first,L"Win10NativePatch.ExplorerController.v2")||
                   !(patch::LoadConfig().flags&patch::ExplorerTabs)||i->second++>=10)i=pending.erase(i);
                else {Attach(i->first);++i;}
            }
            if(pending.empty())KillTimer(window,AttachTimer);
        }
        return 0;
    case WM_CLOSE: {
        // Only a stop made through StopHost ends the starter. A plain close
        // posted by another program is ignored; the lab's single-window mode
        // closes itself that way.
        const auto request=static_cast<ULONG_PTR>(l);
        if(request?reinterpret_cast<ULONG_PTR>(GetPropW(window,patch::mvp::StopRequest))!=request:(w||!selected))return 0;
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
    case WM_ENDSESSION:
        // Signing out is an intended stop: the guard must not start a new copy.
        if(w&&released)SetEvent(released);
        return 0;
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
    bool recovered=false;
    int count{};auto argv=CommandLineToArgvW(GetCommandLineW(),&count);
    std::vector<std::wstring> args;if(argv){for(int i=1;i<count;i++)args.emplace_back(argv[i]);LocalFree(argv);}
    if(args.size()==2&&args[0]==L"--probe")return Probe(args[1]);
    if(args.size()==4&&args[0]==L"--guard")return Guard(args);
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
    } else if(args.size()==2&&args[0]==L"--recovered") {
        ULONG_PTR previous{};if(!Number(args[1],previous)||previous>2)return 2;
        strikes=static_cast<unsigned>(previous);recovered=true;
    } else if(!args.empty())return 2;
#ifndef NATIVE_LAB_ANY_BUILD
    if(!patch::SupportedWindows())return 3;
#endif
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\Win10NativePatch.ExplorerMvpHost.v1");
    // Started by the guard: give the ended starter a moment to let go of the name.
    for(int attempt=0;recovered&&mutex&&GetLastError()==ERROR_ALREADY_EXISTS&&attempt<20;++attempt) {
        CloseHandle(mutex);Sleep(100);mutex=CreateMutexW(nullptr,FALSE,L"Local\\Win10NativePatch.ExplorerMvpHost.v1");
    }
    if(!mutex)return 1;if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(mutex);return 0;}
    const bool guarded=!selected;
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
    Refresh();
    if(!GetShellWindow())SetTimer(host,ShellTimer,1000,nullptr);
    if(guarded) {
        SECURITY_ATTRIBUTES inheritable{sizeof(inheritable),nullptr,TRUE};
        released=CreateEventW(&inheritable,TRUE,FALSE,nullptr);StartGuard();
    }
    MSG message{};bool running=true;
    while(running) {
        const DWORD watched=guard?1:0;
        if(MsgWaitForMultipleObjects(watched,&guard,FALSE,INFINITE,QS_ALLINPUT)==WAIT_OBJECT_0&&watched){GuardEnded();continue;}
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT){running=false;break;}
            TranslateMessage(&message);DispatchMessageW(&message);
        }
    }
    // Release the guard and let it leave first, so no copy of this program is
    // still running (or holding its file) once the starter has exited.
    if(released)SetEvent(released);
    if(guard){WaitForSingleObject(guard,2000);CloseHandle(guard);}
    if(released)CloseHandle(released);
    if(events)UnhookWinEvent(events);
    FreeLibrary(extension);CloseHandle(mutex);return 0;
}
