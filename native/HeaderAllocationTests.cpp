// Guest-only live seam: compile the production allocator unchanged into this
// test hook and exercise its native owner, title lease and row lifecycle.
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <fstream>
#include <map>
#include <string>
#include "HeaderIntegration.h"

namespace {
bool Guest(){wchar_t name[256]{};DWORD size=256;return GetUserNameW(name,&size)&&!_wcsicmp(name,L"WDAGUtilityAccount");}
UINT Message(){return RegisterWindowMessageW(L"Win10NativePatch.HeaderAllocation.Apply");}
void Rect(std::ostream& out,const RECT& rect){out<<'['<<rect.left<<','<<rect.top<<','<<rect.right<<','<<rect.bottom<<']';}
DWORD Flags(HWND frame){return static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(frame,MAKEINTATOM(0xa914))));}
}
#ifdef HEADER_ALLOCATION_DLL
namespace {
struct Frame {HWND surface{};header::LayoutState layout;DWORD flagsBefore{};bool updating{};};
thread_local std::map<HWND,Frame> owned;
void Refresh(HWND frame){
    auto found=owned.find(frame);if(found==owned.end()||found->second.updating)return;
    auto& state=found->second;state.updating=true;
    header::Allocation allocation;
    if(header::Allocate(frame,state.surface,MulDiv(152,GetDpiForWindow(frame),96),allocation)){
        const std::uint64_t ids[]{11,22,33,44,55,66,77,88,99,111,122,133};
        auto next=header::BuildLayout(allocation,ids,133,state.layout.firstVisible);
        auto r=allocation.bounds;
        SetWindowPos(state.surface,HWND_TOP,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        state.layout=std::move(next);
        SetPropW(frame,L"HeaderAllocation.Ready",reinterpret_cast<HANDLE>(1));
    }else{
        ShowWindow(state.surface,SW_HIDE);SetPropW(frame,L"HeaderAllocation.Ready",reinterpret_cast<HANDLE>(2));
    }
    state.updating=false;
}
void Snapshot(HWND frame,unsigned number){
    auto found=owned.find(frame);if(found==owned.end())return;
    Refresh(frame);
    const auto& state=found->second;const auto& allocation=state.layout.allocation;
    const auto& native=allocation.native;
    const std::wstring path=L"C:\\NativeLabResults\\allocation-case-"+std::to_wstring(number)+L".json";
    std::ofstream out(path);
    RECT outer{},surface{},buttons{},navigation{};GetWindowRect(frame,&outer);GetWindowRect(state.surface,&surface);
    DwmGetWindowAttribute(frame,DWMWA_CAPTION_BUTTON_BOUNDS,&buttons,sizeof(buttons));
    EnumChildWindows(frame,[](HWND child,LPARAM param)->BOOL{
        wchar_t name[80]{};GetClassNameW(child,name,80);
        if(!wcscmp(name,L"Address Band Root")&&IsWindowVisible(child))GetWindowRect(GetParent(GetParent(child)),reinterpret_cast<RECT*>(param));
        return TRUE;
    },reinterpret_cast<LPARAM>(&navigation));
    RECT intersection{};bool navigationCollision=IntersectRect(&intersection,&surface,&navigation)!=FALSE;
    out<<"{\"case\":"<<number<<",\"frame\":"<<reinterpret_cast<ULONG_PTR>(frame)<<",\"current\":"<<(header::IsCurrent(frame,allocation)?"true":"false")
        <<",\"mode\":\""<<(allocation.region==header::Region::Caption?"caption":allocation.region==header::Region::AllocatedRow?"row":"unavailable")
        <<"\",\"owner\":"<<allocation.owner<<",\"revision\":"<<allocation.revision<<",\"dpi\":"<<allocation.dpi<<",\"captionFlags\":"<<Flags(frame)<<",\"flagsBefore\":"<<state.flagsBefore
        <<",\"rowPixels\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(frame,header::RowProperty))<<",\"allocation\":";Rect(out,allocation.bounds);
    out<<",\"nativeComplete\":"<<(native.complete?"true":"false")<<",\"nativePaint\":";Rect(out,native.paint);
    out<<",\"nativeTitle\":";Rect(out,native.title);out<<",\"nativeCustom\":[";Rect(out,native.custom[0]);out<<',';Rect(out,native.custom[1]);
    out<<"],\"captionButtons\":";Rect(out,buttons);out<<",\"outer\":";Rect(out,outer);out<<",\"surface\":";Rect(out,surface);
    out<<",\"navigation\":";Rect(out,navigation);out<<",\"navigationCollision\":"<<(navigationCollision?"true":"false")
        <<",\"activeVisible\":"<<(state.layout.Find(133)&&state.layout.Find(133)->Visible()?"true":"false")<<",\"firstVisible\":"<<state.layout.firstVisible<<"}\n";
}
LRESULT CALLBACK FrameProc(HWND frame,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR){
    if(message==header::ChangedMessage()){Refresh(frame);return 0;}
    if(message==WM_SIZE||message==WM_WINDOWPOSCHANGED||message==WM_DPICHANGED||message==WM_THEMECHANGED||message==WM_SETTINGCHANGE){
        header::Invalidate(frame);const auto result=DefSubclassProc(frame,message,w,l);Refresh(frame);return result;
    }
    return DefSubclassProc(frame,message,w,l);
}
void Release(HWND frame){
    auto found=owned.find(frame);if(found==owned.end())return;
    const DWORD before=found->second.flagsBefore;
    const HWND surface=found->second.surface;
    RemoveWindowSubclass(frame,FrameProc,0x4841);header::Detach(frame);DestroyWindow(surface);owned.erase(frame);
    std::ofstream out(L"C:\\NativeLabResults\\allocation-cleanup.json");
    out<<"{\"flagsBefore\":"<<before<<",\"flagsAfter\":"<<Flags(frame)<<",\"rowAbsent\":"<<(!GetPropW(frame,header::RowProperty)?"true":"false")
        <<",\"surfaceAbsent\":"<<(!IsWindow(surface)?"true":"false")<<"}\n";
    RemovePropW(frame,L"HeaderAllocation.Ready");
}
}
extern "C" __declspec(dllexport) LRESULT CALLBACK HeaderAllocationHook(int code,WPARAM removed,LPARAM data){
    if(code>=0&&removed==PM_REMOVE&&Guest()){
        auto* message=reinterpret_cast<MSG*>(data);
        if(message->message==Message()){
            HWND frame=message->hwnd;wchar_t name[64]{};GetClassNameW(frame,name,64);
            if(wcscmp(name,L"CabinetWClass")||GetWindowThreadProcessId(frame,nullptr)!=GetCurrentThreadId())return CallNextHookEx(nullptr,code,removed,data);
            if(message->wParam==1&&!owned.contains(frame)){
                Frame state;state.flagsBefore=Flags(frame);
                state.surface=CreateWindowExW(0,L"STATIC",L"Native allocation",WS_CHILD|WS_CLIPSIBLINGS,0,0,0,0,frame,nullptr,nullptr,nullptr);
                if(state.surface&&header::Attach(frame)){
                    owned.emplace(frame,std::move(state));SetWindowSubclass(frame,FrameProc,0x4841,0);Refresh(frame);
                }else if(state.surface)DestroyWindow(state.surface);
            }else if(message->wParam==2)Snapshot(frame,static_cast<unsigned>(message->lParam));
            else if(message->wParam==3&&owned.contains(frame)){
                auto& state=owned.at(frame);const auto previous=state.layout.allocation;
                state.updating=true;header::Detach(frame);const bool attached=header::Attach(frame);
                const bool rejected=!header::IsCurrent(frame,previous);state.updating=false;Refresh(frame);
                std::ofstream out(L"C:\\NativeLabResults\\allocation-reattach.json");
                out<<"{\"attached\":"<<(attached?"true":"false")<<",\"oldSnapshotRejected\":"<<(rejected?"true":"false")
                    <<",\"oldOwner\":"<<previous.owner<<",\"newOwner\":"<<state.layout.allocation.owner<<"}\n";
            }
            else if(message->wParam==0)Release(frame);
        }
    }
    return CallNextHookEx(nullptr,code,removed,data);
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
#else
int wmain(int argc,wchar_t** argv){
    if((argc!=3&&argc!=6)||!Guest())return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HWND frame=reinterpret_cast<HWND>(wcstoull(argv[1],nullptr,10));
    if(argc==6&&!wcscmp(argv[3],L"--native-click")){
        const POINT point{_wtoi(argv[4]),_wtoi(argv[5])};
        SetForegroundWindow(frame);SetCursorPos(point.x,point.y);
        INPUT input[2]{};input[0].type=input[1].type=INPUT_MOUSE;
        input[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
        const UINT sent=SendInput(2,input,sizeof(INPUT));
        return sent==2?0:1;
    }
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);std::wstring libraryPath=path;
    libraryPath=libraryPath.substr(0,libraryPath.find_last_of(L"\\"))+L"\\HeaderAllocationHook.dll";
    const HMODULE library=LoadLibraryW(libraryPath.c_str());if(!library)return 3;
    const auto callback=reinterpret_cast<HOOKPROC>(GetProcAddress(library,"HeaderAllocationHook"));
    const HHOOK hook=SetWindowsHookExW(WH_GETMESSAGE,callback,library,GetWindowThreadProcessId(frame,nullptr));
    if(!hook)return 4;
    PostMessageW(frame,Message(),1,0);
    const ULONGLONG deadline=GetTickCount64()+240000;
    const std::wstring stop=std::wstring(argv[2])+L"\\allocation.stop";
    while(GetTickCount64()<deadline&&GetFileAttributesW(stop.c_str())==INVALID_FILE_ATTRIBUTES)Sleep(100);
    PostMessageW(frame,Message(),0,0);Sleep(800);UnhookWindowsHookEx(hook);FreeLibrary(library);return 0;
}
#endif
