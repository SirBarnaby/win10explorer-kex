#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <process.h>
#include <map>
#include <mutex>
#include <utility>
#include <dcomp.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <d2d1.h>
#include <exdisp.h>
#include <shlobj.h>
#include "MvpHostProtocol.h"
#include "HeaderCommands.h"

namespace {
bool Guest(){wchar_t user[256]{};DWORD n=256;return GetUserNameW(user,&n)&&!_wcsicmp(user,L"WDAGUtilityAccount");}
UINT ApplyMessage(){static UINT m=RegisterWindowMessageW(L"Win10NativePatch.HeaderLab.Apply");return m;}
constexpr wchar_t StateProperty[]=L"Win10NativePatch.HeaderLab.State";
constexpr wchar_t CallsProperty[]=L"Win10NativePatch.HeaderLab.Calls";
constexpr wchar_t FaultProperty[]=L"Win10NativePatch.HeaderLab.Fallback";
constexpr wchar_t FaultCallsProperty[]=L"Win10NativePatch.HeaderLab.FallbackCalls";
}
#ifdef HEADER_LAB_DLL
namespace {
HMODULE ribbon{};HWND frame{};DWORD owner{};int extra{};ULONG calls{};
bool Exchange(PVOID* slot,PVOID replacement,PVOID expected){
    DWORD protect{};if(!VirtualProtect(slot,sizeof(PVOID),PAGE_READWRITE,&protect))return false;
    PVOID before=InterlockedCompareExchangePointer(slot,replacement,expected);
    DWORD ignored{};VirtualProtect(slot,sizeof(PVOID),protect,&ignored);return before==expected;
}
using BorderFn=HRESULT(WINAPI*)(void*,void**,const RECT*);
BorderFn originalBorder{};PVOID* borderSlot{};
bool product{};
ULONG faultCalls{};
PVOID* heldAccessibility{};PVOID savedAccessibility{};
std::mutex eventMutex;
std::map<HWINEVENTHOOK,DWORD> eventOwners;
HWND eventAnchor{};ULONG eventsCreated{},eventsRemoved{},wrongEventThread{},eventFailures{};
HMODULE eventLibrary{};PVOID* eventSetSlot{};PVOID* eventRemoveSlot{};PVOID savedEventSet{},savedEventRemove{};
void EventStatus(){SetPropW(eventAnchor,L"HeaderLab.EventsCreated",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(eventsCreated)));SetPropW(eventAnchor,L"HeaderLab.EventsRemoved",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(eventsRemoved)));SetPropW(eventAnchor,L"HeaderLab.WrongEventThread",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(wrongEventThread)));SetPropW(eventAnchor,L"HeaderLab.EventFailures",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(eventFailures)));}
HWINEVENTHOOK WINAPI ObserveSetEvent(DWORD first,DWORD last,HMODULE module,WINEVENTPROC callback,DWORD pid,DWORD thread,DWORD flags){
    auto hook=SetWinEventHook(first,last,module,callback,pid,thread,flags);
    if(hook&&first==EVENT_OBJECT_LOCATIONCHANGE&&last==EVENT_OBJECT_LOCATIONCHANGE){std::lock_guard lock(eventMutex);eventOwners[hook]=GetCurrentThreadId();++eventsCreated;EventStatus();}
    return hook;
}
BOOL WINAPI ObserveRemoveEvent(HWINEVENTHOOK hook){
    DWORD hookOwner{};{std::lock_guard lock(eventMutex);auto found=eventOwners.find(hook);if(found!=eventOwners.end())hookOwner=found->second;}
    BOOL result=UnhookWinEvent(hook);
    if(hookOwner){std::lock_guard lock(eventMutex);if(hookOwner!=GetCurrentThreadId())++wrongEventThread;if(result){++eventsRemoved;eventOwners.erase(hook);}else ++eventFailures;EventStatus();}
    return result;
}
using RtlVersionFn=LONG(WINAPI*)(OSVERSIONINFOW*);
LONG WINAPI LabVersion(OSVERSIONINFOW* version){
    auto real=reinterpret_cast<RtlVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
    LONG status=real(version);if(status==0&&version->dwBuildNumber==19041)version->dwBuildNumber=19045;return status;
}
FARPROC WINAPI LabResolver(HMODULE library,LPCSTR name){
    if(reinterpret_cast<ULONG_PTR>(name)>65535&&!strcmp(name,"RtlGetVersion"))return reinterpret_cast<FARPROC>(LabVersion);
    return GetProcAddress(library,name);
}
PVOID* ImportSlot(HMODULE library,const char* wanted){
    BYTE* base=reinterpret_cast<BYTE*>(library);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    auto imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(auto entry=imports;entry->Name;++entry){
        auto names=reinterpret_cast<IMAGE_THUNK_DATA*>(base+entry->OriginalFirstThunk),functions=reinterpret_cast<IMAGE_THUNK_DATA*>(base+entry->FirstThunk);
        if(!entry->OriginalFirstThunk)continue;
        for(;names->u1.AddressOfData;++names,++functions){
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)){
                HMODULE dependency=GetModuleHandleA(reinterpret_cast<const char*>(base+entry->Name));
                FARPROC named=dependency?GetProcAddress(dependency,wanted):nullptr;
                if(named&&named==GetProcAddress(dependency,MAKEINTRESOURCEA(IMAGE_ORDINAL(names->u1.Ordinal))))
                    return reinterpret_cast<PVOID*>(&functions->u1.Function);
                continue;
            }
            auto import=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            if(!strcmp(reinterpret_cast<const char*>(import->Name),wanted))return reinterpret_cast<PVOID*>(&functions->u1.Function);
        }
    }return nullptr;
}
PVOID* creationSlot{};PVOID originalCreation{};HWND creationOwner{};HMODULE creationLibrary{};
PVOID* saveSlot{};PVOID originalSave{};HWND saveOwner{};
LSTATUS WINAPI CloseDuringSave(HKEY key,LPCWSTR name,DWORD reserved,DWORD type,const BYTE* bytes,DWORD size){
    const auto result=reinterpret_cast<decltype(&RegSetValueExW)>(originalSave)(key,name,reserved,type,bytes,size);
    if(name&&!wcsncmp(name,L"Window.",7)&&saveOwner){
        const HWND closing=std::exchange(saveOwner,nullptr);
        Exchange(saveSlot,originalSave,reinterpret_cast<PVOID>(CloseDuringSave));saveSlot=nullptr;
        DestroyWindow(closing);
        std::ofstream report(L"C:\\NativeLabResults\\nested-save-callback.json");report<<"{\"called\":true,\"frameDestroyed\":"<<(!IsWindow(closing)?"true":"false")<<"}";
    }
    return result;
}
HRESULT WINAPI ObserveCreation(REFCLSID clsid,IUnknown* outer,DWORD context,REFIID iid,void** out){
    HRESULT hr=reinterpret_cast<decltype(&CoCreateInstance)>(originalCreation)(clsid,outer,context,iid,out);
    if(SUCCEEDED(hr)&&clsid==CLSID_ShellBrowserWindow&&out&&*out){IWebBrowser2* browser{};if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&browser)))){SHANDLE_PTR handle{};browser->get_HWND(&handle);SetPropW(creationOwner,L"HeaderLab.ProductCreatedFrame",reinterpret_cast<HANDLE>(handle));browser->Release();}}
    return hr;
}
bool InstrumentCreation(HWND window,bool enabled){
    if(!enabled){if(creationSlot)Exchange(creationSlot,originalCreation,reinterpret_cast<PVOID>(ObserveCreation));creationSlot=nullptr;if(creationLibrary)FreeLibrary(creationLibrary);creationLibrary=nullptr;return true;}
    creationOwner=window;wchar_t path[32768]{};HMODULE ours{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&InstrumentCreation),&ours);GetModuleFileNameW(ours,path,32768);std::wstring file=path;file.resize(file.find_last_of(L'\\'));file+=L"\\ExplorerTabs.dll";
    creationLibrary=LoadLibraryW(file.c_str());creationSlot=creationLibrary?ImportSlot(creationLibrary,"CoCreateInstance"):nullptr;if(!creationSlot)return false;originalCreation=*creationSlot;return Exchange(creationSlot,reinterpret_cast<PVOID>(ObserveCreation),originalCreation);
}
PVOID* graphicsSlot{};PVOID graphicsOriginal{},graphicsReplacement{};
PVOID* debugSlot{};PVOID debugOriginal{};
void WINAPI ObserveDebug(LPCWSTR text){
    std::wofstream log(L"C:\\NativeLabResults\\renderer-debug.txt",std::ios::app);
    log<<GetTickCount64()<<L" "<<(text?text:L"")<<L"\n";
    reinterpret_cast<void(WINAPI*)(LPCWSTR)>(debugOriginal)(text);
}
// The production renderer draws through DirectComposition surfaces created by
// its own desktop device. The draw fault rejects BeginDraw on those surfaces
// only, as a lost device would, so Explorer's own composition is untouched.
PVOID* surfaceSlot{};PVOID surfaceOriginal{},surfaceReplacement{};
PVOID* drawSlot{};PVOID drawOriginal{},drawReplacement{};IUnknown* observedDevice{};
std::vector<IUnknown*> observedSurfaces;
bool rejectDraw{};ULONG graphicsCalls{};
void RestoreGraphics(){
    if(debugSlot)Exchange(debugSlot,debugOriginal,reinterpret_cast<PVOID>(ObserveDebug));
    debugSlot=nullptr;
    if(graphicsSlot)Exchange(graphicsSlot,graphicsOriginal,graphicsReplacement);
    graphicsSlot=nullptr;
    if(drawSlot)Exchange(drawSlot,drawOriginal,drawReplacement);
    drawSlot=nullptr;
    if(surfaceSlot)Exchange(surfaceSlot,surfaceOriginal,surfaceReplacement);
    surfaceSlot=nullptr;observedDevice=nullptr;observedSurfaces.clear();rejectDraw=false;
}
HRESULT WINAPI NoComposition(IUnknown*,REFIID,void** value){
    if(value)*value=nullptr;
    SetPropW(frame,L"HeaderLab.GraphicsCalls",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(++graphicsCalls)));
    return E_FAIL;
}
using BeginDrawFn=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,const RECT*,REFIID,void**,POINT*);
HRESULT STDMETHODCALLTYPE ObserveBeginDraw(IUnknown* surface,const RECT* update,REFIID iid,void** value,POINT* offset){
    if(rejectDraw&&std::find(observedSurfaces.begin(),observedSurfaces.end(),surface)!=observedSurfaces.end()){
        if(value)*value=nullptr;
        SetPropW(frame,L"HeaderLab.GraphicsCalls",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(++graphicsCalls)));
        return D2DERR_RECREATE_TARGET;
    }
    return reinterpret_cast<BeginDrawFn>(drawOriginal)(surface,update,iid,value,offset);
}
using CreateSurfaceFn=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,UINT,UINT,DXGI_FORMAT,DXGI_ALPHA_MODE,IUnknown**);
HRESULT STDMETHODCALLTYPE ObserveSurface(IUnknown* device,UINT width,UINT height,DXGI_FORMAT format,DXGI_ALPHA_MODE alpha,IUnknown** surface){
    HRESULT hr=reinterpret_cast<CreateSurfaceFn>(surfaceOriginal)(device,width,height,format,alpha,surface);
    if(SUCCEEDED(hr)&&device==observedDevice&&surface&&*surface){
        observedSurfaces.push_back(*surface);
        // IDCompositionSurface: IUnknown (3 slots), then BeginDraw.
        auto slot=*reinterpret_cast<PVOID**>(*surface)+3;
        if(!drawSlot){drawOriginal=*slot;drawReplacement=reinterpret_cast<PVOID>(ObserveBeginDraw);if(Exchange(slot,drawReplacement,drawOriginal))drawSlot=slot;}
    }
    return hr;
}
HRESULT WINAPI ObserveComposition(IUnknown* rendering,REFIID id,void** value){
    using Fn=HRESULT(WINAPI*)(IUnknown*,REFIID,void**);
    HRESULT hr=reinterpret_cast<Fn>(graphicsOriginal)(rendering,id,value);
    if(SUCCEEDED(hr)&&value&&*value){
        observedDevice=static_cast<IUnknown*>(*value);
        // IDCompositionDevice2: IUnknown (3), Commit, WaitForCommitCompletion,
        // GetFrameStatistics, CreateVisual, CreateSurfaceFactory, CreateSurface.
        auto slot=*reinterpret_cast<PVOID**>(observedDevice)+8;
        if(!surfaceSlot){surfaceOriginal=*slot;surfaceReplacement=reinterpret_cast<PVOID>(ObserveSurface);if(Exchange(slot,surfaceReplacement,surfaceOriginal))surfaceSlot=slot;}
    }
    return hr;
}
bool InstrumentGraphics(HMODULE library,bool creation){
    graphicsCalls=0;rejectDraw=false;
    debugSlot=ImportSlot(library,"OutputDebugStringW");
    if(debugSlot){debugOriginal=*debugSlot;if(!Exchange(debugSlot,reinterpret_cast<PVOID>(ObserveDebug),debugOriginal))debugSlot=nullptr;}
    graphicsSlot=ImportSlot(library,"DCompositionCreateDevice2");
    if(!graphicsSlot)return false;
    graphicsOriginal=*graphicsSlot;
    graphicsReplacement=creation?reinterpret_cast<PVOID>(NoComposition):reinterpret_cast<PVOID>(ObserveComposition);
    return Exchange(graphicsSlot,graphicsReplacement,graphicsOriginal);
}
bool InstrumentEvents(HWND anchor,bool enable){
    if(!enable){
        if(eventSetSlot)Exchange(eventSetSlot,savedEventSet,reinterpret_cast<PVOID>(ObserveSetEvent));
        if(eventRemoveSlot)Exchange(eventRemoveSlot,savedEventRemove,reinterpret_cast<PVOID>(ObserveRemoveEvent));
        if(eventLibrary)FreeLibrary(eventLibrary);eventLibrary=nullptr;return true;
    }
    eventAnchor=anchor;eventsCreated=eventsRemoved=wrongEventThread=eventFailures=0;eventOwners.clear();
    wchar_t file[32768]{};HMODULE ours{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&InstrumentEvents),&ours);GetModuleFileNameW(ours,file,32768);
    std::wstring path=file;path=path.substr(0,path.find_last_of(L"\\"))+L"\\ExplorerTabs.dll";eventLibrary=LoadLibraryW(path.c_str());if(!eventLibrary)return false;
    eventSetSlot=ImportSlot(eventLibrary,"SetWinEventHook");eventRemoveSlot=ImportSlot(eventLibrary,"UnhookWinEvent");if(!eventSetSlot||!eventRemoveSlot)return false;
    savedEventSet=*eventSetSlot;savedEventRemove=*eventRemoveSlot;
    if(!Exchange(eventSetSlot,reinterpret_cast<PVOID>(ObserveSetEvent),savedEventSet))return false;
    if(!Exchange(eventRemoveSlot,reinterpret_cast<PVOID>(ObserveRemoveEvent),savedEventRemove)){Exchange(eventSetSlot,savedEventSet,reinterpret_cast<PVOID>(ObserveSetEvent));return false;}
    return true;
}
HRESULT WINAPI NoAccessibility(HWND window,DWORD,REFIID,void** result){
    wchar_t name[64]{};GetClassNameW(window,name,64);
    if(!wcscmp(name,L"NetUIHWND")){++faultCalls;SetPropW(frame,FaultCallsProperty,reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(faultCalls)));}
    if(result)*result=nullptr;return E_FAIL;
}
void RestoreAccessibility(){if(heldAccessibility)Exchange(heldAccessibility,savedAccessibility,reinterpret_cast<PVOID>(NoAccessibility));heldAccessibility=nullptr;}
void FallbackState(HWND window){
    bool strip=false;EnumChildWindows(window,[](HWND h,LPARAM value)->BOOL{wchar_t name[128]{};GetClassNameW(h,name,128);if(!wcsncmp(name,L"Win10NativePatch.ExplorerTabs.v2",32)&&IsWindowVisible(h))*reinterpret_cast<bool*>(value)=true;return TRUE;},reinterpret_cast<LPARAM>(&strip));
    auto base=reinterpret_cast<BYTE*>(GetModuleHandleW(L"UIRibbon.dll"));bool restored=base&&*reinterpret_cast<PVOID*>(base+0x21a5f0+0x18)==base+0x11900;
    ULONG_PTR flags=(strip?1:0)|(GetPropW(window,L"Win10NativePatch.TabsActive.v2")?2:0)|(restored?4:0);
    SetPropW(window,FaultProperty,reinterpret_cast<HANDLE>(flags));
}
bool AttachProduct(MSG* message,bool fault=false,bool persistent=false,int graphics=0){
    if(fault)faultCalls=0;
    wchar_t file[32768]{};HMODULE ours{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&AttachProduct),&ours);
    GetModuleFileNameW(ours,file,32768);std::wstring path=file;path=path.substr(0,path.find_last_of(L"\\"))+L"\\ExplorerTabs.dll";
    HMODULE library=LoadLibraryW(path.c_str());if(!library)return false;
    if(graphics&&!InstrumentGraphics(library,graphics==1)){FreeLibrary(library);return false;}
    PVOID* resolver=ImportSlot(library,"GetProcAddress");PVOID originalResolver=resolver?*resolver:nullptr;
    if(!resolver||!Exchange(resolver,reinterpret_cast<PVOID>(LabResolver),originalResolver)){FreeLibrary(library);return false;}
    auto hook=reinterpret_cast<HOOKPROC>(GetProcAddress(library,"MvpExplorerHook"));
    PVOID* accessibility=fault?ImportSlot(library,"AccessibleObjectFromWindow"):nullptr;
    PVOID originalAccessibility=accessibility?*accessibility:nullptr;
    if(fault&&(!accessibility||!Exchange(accessibility,reinterpret_cast<PVOID>(NoAccessibility),originalAccessibility))){Exchange(resolver,originalResolver,reinterpret_cast<PVOID>(LabResolver));FreeLibrary(library);return false;}
    MSG attach=*message;attach.message=patch::mvp::AttachMessage();
    if(hook)hook(HC_ACTION,PM_REMOVE,reinterpret_cast<LPARAM>(&attach));
    if(fault){
        if(persistent){heldAccessibility=accessibility;savedAccessibility=originalAccessibility;}
        else Exchange(accessibility,originalAccessibility,reinterpret_cast<PVOID>(NoAccessibility));
        bool strip=false;EnumChildWindows(message->hwnd,[](HWND h,LPARAM value)->BOOL{wchar_t name[128]{};GetClassNameW(h,name,128);if(!wcsncmp(name,L"Win10NativePatch.ExplorerTabs.v2",32)&&IsWindowVisible(h))*reinterpret_cast<bool*>(value)=true;return TRUE;},reinterpret_cast<LPARAM>(&strip));
        auto base=reinterpret_cast<BYTE*>(GetModuleHandleW(L"UIRibbon.dll"));bool restored=*reinterpret_cast<PVOID*>(base+0x21a5f0+0x18)==base+0x11900;
        ULONG_PTR flags=(strip?1:0)|(GetPropW(message->hwnd,L"Win10NativePatch.TabsActive.v2")?2:0)|(restored?4:0);
        SetPropW(message->hwnd,FaultProperty,reinterpret_cast<HANDLE>(flags));
    }
    Exchange(resolver,originalResolver,reinterpret_cast<PVOID>(LabResolver));FreeLibrary(library);
    return GetPropW(message->hwnd,L"Win10NativePatch.TabsActive.v2")!=nullptr;
}
HRESULT WINAPI BorderHook(void* self,void** client,const RECT* border){
    HWND ownerFrame=client&&*client?*reinterpret_cast<HWND*>(reinterpret_cast<BYTE*>(*client)+0x58):nullptr;
    if(extra&&border&&ownerFrame==frame&&GetCurrentThreadId()==owner){
        RECT allocation=*border;allocation.top+=MulDiv(extra,static_cast<int>(GetDpiForWindow(frame)),96);
        ++calls;SetPropW(frame,CallsProperty,reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(calls)));
        return originalBorder(self,client,&allocation);
    }
    return originalBorder(self,client,border);
}
bool HookBorder(){
    ribbon=GetModuleHandleW(L"UIRibbon.dll");if(!ribbon)return false;
    auto base=reinterpret_cast<BYTE*>(ribbon);auto table=reinterpret_cast<PVOID*>(base+0x21a5f0);
    originalBorder=reinterpret_cast<BorderFn>(base+0x11900);
    for(int i=0;i<20;++i)if(table[i]==reinterpret_cast<PVOID>(originalBorder)){borderSlot=table+i;return Exchange(borderSlot,reinterpret_cast<PVOID>(BorderHook),reinterpret_cast<PVOID>(originalBorder));}
    return false;
}
void UnhookBorder(){if(borderSlot)Exchange(borderSlot,reinterpret_cast<PVOID>(originalBorder),reinterpret_cast<PVOID>(BorderHook));borderSlot=nullptr;}
}
extern "C" __declspec(dllexport) LRESULT CALLBACK HeaderLabHook(int code,WPARAM w,LPARAM l){
    if(code>=0&&w==PM_REMOVE&&Guest()){
        auto m=reinterpret_cast<MSG*>(l);
        if(m->message==ApplyMessage()){
            if(m->wParam==85){
                wchar_t path[32768]{};HMODULE ours{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&HeaderLabHook),&ours);GetModuleFileNameW(ours,path,32768);std::wstring file=path;file.resize(file.find_last_of(L'\\'));file+=L"\\ExplorerTabs.dll";
                const HMODULE library=GetModuleHandleW(file.c_str());saveOwner=m->hwnd;saveSlot=library?ImportSlot(library,"RegSetValueExW"):nullptr;
                if(saveSlot){originalSave=*saveSlot;Exchange(saveSlot,reinterpret_cast<PVOID>(CloseDuringSave),originalSave);}
                return CallNextHookEx(nullptr,code,w,l);
            }
            if(m->wParam==83||m->wParam==84){InstrumentCreation(m->hwnd,m->wParam==83);return CallNextHookEx(nullptr,code,w,l);}
            if(m->wParam==81||m->wParam==82){if(m->wParam==81)SetCapture(m->hwnd);else ReleaseCapture();return CallNextHookEx(nullptr,code,w,l);}
            if(m->wParam==80){
                IWebBrowser2* created{};SHANDLE_PTR handle{};
                HRESULT hr=CoCreateInstance(CLSID_ShellBrowserWindow,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&created));
                if(created){hr=created->get_HWND(&handle);created->put_Visible(VARIANT_TRUE);created->Release();}
                SetPropW(m->hwnd,L"HeaderLab.CreatedFrame",reinterpret_cast<HANDLE>(handle));
                SetPropW(m->hwnd,L"HeaderLab.CreationResult",reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(hr)));
                return CallNextHookEx(nullptr,code,w,l);
            }
            if(m->wParam==73){RestoreGraphics();return CallNextHookEx(nullptr,code,w,l);}
            if(m->wParam==75){
                rejectDraw=true;
                EnumChildWindows(m->hwnd,[](HWND child,LPARAM)->BOOL{
                    wchar_t name[128]{};GetClassNameW(child,name,128);
                    if(!wcsncmp(name,L"Win10NativePatch.ExplorerTabs.v2",32))InvalidateRect(child,nullptr,FALSE);
                    return TRUE;
                },0);
                // The strip only draws what changed, so a repaint request alone
                // may draw nothing. A colour change, as Windows sends it, makes
                // it draw everything again and so meet the injected fault.
                SendMessageW(m->hwnd,WM_SYSCOLORCHANGE,0,0);
                return CallNextHookEx(nullptr,code,w,l);
            }
            if(m->wParam==70){FallbackState(m->hwnd);return CallNextHookEx(nullptr,code,w,l);}
            if(m->wParam==68||m->wParam==69){bool ok=InstrumentEvents(m->hwnd,m->wParam==68);SetPropW(m->hwnd,L"HeaderLab.Instrumented",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(ok?1:2)));return CallNextHookEx(nullptr,code,w,l);}
            if(m->wParam){
                frame=m->hwnd;owner=GetCurrentThreadId();calls=0;extra=static_cast<int>(m->wParam);
                product=m->wParam==64||m->wParam==66||m->wParam==67||m->wParam==72||m->wParam==74;
                bool ok=product?AttachProduct(m,m->wParam==66||m->wParam==67,m->wParam==66,m->wParam==72?1:m->wParam==74?2:0):HookBorder();SetPropW(frame,StateProperty,reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(ok?1:3)));
            }else{
                extra=0;RestoreGraphics();RestoreAccessibility();InstrumentCreation(m->hwnd,false);if(product)SendMessageW(m->hwnd,patch::mvp::DetachMessage(),0,0);else UnhookBorder();
                SetPropW(m->hwnd,StateProperty,reinterpret_cast<HANDLE>(2));
            }
        }
    }
    return CallNextHookEx(nullptr,code,w,l);
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
#else
namespace {
void Wait(unsigned ms){Sleep(ms);}
void Capture(HWND h,const std::wstring& path){
    RECT r{};GetWindowRect(h,&r);int width=r.right-r.left,height=r.bottom-r.top;
    HDC source=GetWindowDC(h),dc=CreateCompatibleDC(source);HBITMAP image=CreateCompatibleBitmap(source,width,height);HGDIOBJ old=SelectObject(dc,image);
    HDC desktop=GetDC(nullptr);BOOL rendered=BitBlt(dc,0,0,width,height,desktop,r.left,r.top,SRCCOPY|CAPTUREBLT);ReleaseDC(nullptr,desktop);
    if(rendered){Gdiplus::Bitmap bitmap(image,nullptr);CLSID png{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0,0,0xf8,0x1e,0xf3,0x2e}};bitmap.Save(path.c_str(),&png,nullptr);}
    SelectObject(dc,old);DeleteObject(image);DeleteDC(dc);ReleaseDC(h,source);
}
void Snapshot(HWND frame,const std::wstring& path){
    std::ofstream out(path);out<<"{\"metricCalls\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(frame,CallsProperty))<<",\"children\":[";
    struct Context{std::ofstream* out;bool first=true;}c{&out};
    EnumChildWindows(frame,[](HWND h,LPARAM value)->BOOL{
        auto& c=*reinterpret_cast<Context*>(value);wchar_t name[128]{};GetClassNameW(h,name,128);
        RECT r{};GetWindowRect(h,&r);if(!c.first)*c.out<<',';c.first=false;
        std::string type;for(wchar_t ch:name){if(!ch)break;type+=ch<128?static_cast<char>(ch):'?';}
        *c.out<<"{\"class\":\""<<type<<"\",\"rect\":["<<r.left<<','<<r.top<<','<<r.right<<','<<r.bottom<<"]}";return TRUE;
    },reinterpret_cast<LPARAM>(&c));out<<"]}\n";
}
void Commands(HWND h,const std::wstring& path){
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);std::wstring program=module;program=program.substr(0,program.find_last_of(L"\\"))+L"\\ExplorerHeaderProbe.exe";
    std::wstring window=std::to_wstring(reinterpret_cast<ULONG_PTR>(h));
    _wspawnl(_P_WAIT,program.c_str(),program.c_str(),window.c_str(),path.c_str(),nullptr);
}
header::Inspection Inspect(HWND frame){
    header::Inspection result;
    EnumChildWindows(frame,[](HWND h,LPARAM value)->BOOL{
        wchar_t name[64]{};GetClassNameW(h,name,64);if(wcscmp(name,L"NetUIHWND"))return TRUE;
        Microsoft::WRL::ComPtr<IAccessible> accessible;
        if(SUCCEEDED(AccessibleObjectFromWindow(h,static_cast<DWORD>(OBJID_CLIENT),IID_IAccessible,reinterpret_cast<void**>(accessible.GetAddressOf()))))reinterpret_cast<header::Inspection*>(value)->Walk(accessible.Get());
        return TRUE;
    },reinterpret_cast<LPARAM>(&result));return result;
}
bool PaneVisible(HWND frame){bool visible=false;EnumChildWindows(frame,[](HWND h,LPARAM value)->BOOL{wchar_t name[64]{};GetClassNameW(h,name,64);if(!wcscmp(name,L"NamespaceTreeControl")&&IsWindowVisible(h))*reinterpret_cast<bool*>(value)=true;return TRUE;},reinterpret_cast<LPARAM>(&visible));return visible;}
bool ClickCommand(HWND frame,const wchar_t* name){
    auto inspection=Inspect(frame);
    for(const auto& command:inspection.commands)if(command.name==name&&command.hasRect&&!(command.state&(STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_OFFSCREEN))){
        LONG_PTR style=GetWindowLongPtrW(frame,GWL_EXSTYLE);SetWindowPos(frame,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SetCursorPos((command.rect.left+command.rect.right)/2,(command.rect.top+command.rect.bottom)/2);
        INPUT click[2]{};for(auto& i:click)i.type=INPUT_MOUSE;click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
        bool sent=SendInput(2,click,sizeof(INPUT))==2;Wait(500);
        if(!(style&WS_EX_TOPMOST))SetWindowPos(frame,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        return sent;
    }return false;
}
void ActivateFrame(HWND frame){
    SetForegroundWindow(frame);RECT r{};GetWindowRect(frame,&r);SetCursorPos(r.left+450,r.top+250);
    INPUT click[2]{};for(auto& i:click)i.type=INPUT_MOUSE;click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    SendInput(2,click,sizeof(INPUT));Wait(200);
}
std::vector<BYTE> SavedSession(){
    DWORD bytes=0;constexpr auto key=L"Software\\Win10NativePatch\\v2\\ExplorerTabs";
    if(RegGetValueW(HKEY_CURRENT_USER,key,L"LastSession",RRF_RT_REG_BINARY,nullptr,nullptr,&bytes)!=ERROR_SUCCESS)return {};
    std::vector<BYTE> result(bytes);if(RegGetValueW(HKEY_CURRENT_USER,key,L"LastSession",RRF_RT_REG_BINARY,nullptr,result.data(),&bytes)!=ERROR_SUCCESS)return {};
    return result;
}
void Reflow(HWND h,int width,int height){
    DWORD_PTR result{};
    SendMessageTimeoutW(h,WM_SETTINGCHANGE,SPI_SETNONCLIENTMETRICS,0,SMTO_ABORTIFHUNG,1000,&result);
    SendMessageTimeoutW(h,WM_THEMECHANGED,0,0,SMTO_ABORTIFHUNG,1000,&result);
    SetWindowPos(h,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_FRAMECHANGED);
    RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);Wait(1200);
}
}
int wmain(int argc,wchar_t** argv){
    if((argc!=3&&argc!=4&&argc!=5)||!Guest())return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND h=reinterpret_cast<HWND>(wcstoull(argv[1],nullptr,10));wchar_t name[64]{};GetClassNameW(h,name,64);if(wcscmp(name,L"CabinetWClass"))return 2;
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);std::wstring dll=path;dll=dll.substr(0,dll.find_last_of(L"\\"))+L"\\HeaderLabHook.dll";
    HMODULE module=LoadLibraryW(dll.c_str());if(!module)return 3;
    auto proc=reinterpret_cast<HOOKPROC>(GetProcAddress(module,"HeaderLabHook"));DWORD thread=GetWindowThreadProcessId(h,nullptr);
    HHOOK hook=proc?SetWindowsHookExW(WH_GETMESSAGE,proc,module,thread):nullptr;if(!hook){FreeLibrary(module);return 4;}
    ULONG_PTR gdip{};Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&gdip,&input,nullptr);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    std::wstring dir=argv[2];RECT outer{};GetWindowRect(h,&outer);int width=outer.right-outer.left,height=outer.bottom-outer.top;
    if(argc==4&&!wcscmp(argv[3],L"--attach-only")) {
        PostMessageW(h,ApplyMessage(),64,0);bool ready=false;
        for(unsigned i=0;i<30&&!ready;++i){Wait(100);EnumChildWindows(h,[](HWND child,LPARAM value)->BOOL{wchar_t name[128]{};GetClassNameW(child,name,128);if(!wcsncmp(name,L"Win10NativePatch.ExplorerTabs.v2",32)&&IsWindowVisible(child))*reinterpret_cast<bool*>(value)=true;return TRUE;},reinterpret_cast<LPARAM>(&ready));}
        std::ofstream report(dir);report<<"{\"success\":"<<(ready?"true":"false")<<",\"frame\":"<<reinterpret_cast<ULONG_PTR>(h)<<"}";
        UnhookWindowsHookEx(hook);FreeLibrary(module);Gdiplus::GdiplusShutdown(gdip);CoUninitialize();return ready?0:1;
    }
    if(argc==4&&!wcscmp(argv[3],L"--renderer-modes")) {
        PostMessageW(h,ApplyMessage(),64,0);Wait(800);
        ActivateFrame(h);Capture(h,dir+L"\\mode-native.png");
        HIGHCONTRASTW saved{sizeof(saved)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(saved),&saved,0);
        std::wstring scheme=saved.lpszDefaultScheme?saved.lpszDefaultScheme:L"";
        HIGHCONTRASTW white{sizeof(white)};white.dwFlags=saved.dwFlags|HCF_HIGHCONTRASTON;
        white.lpszDefaultScheme=const_cast<wchar_t*>(L"High Contrast White");
        bool whiteSet=SystemParametersInfoW(SPI_SETHIGHCONTRAST,sizeof(white),&white,SPIF_SENDCHANGE)!=FALSE;
        Wait(1000);ActivateFrame(h);Capture(h,dir+L"\\mode-high-contrast-white.png");
        HIGHCONTRASTW actual{sizeof(actual)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(actual),&actual,0);
        COLORREF caption=GetSysColor(COLOR_ACTIVECAPTION),captionText=GetSysColor(COLOR_CAPTIONTEXT),buttonText=GetSysColor(COLOR_BTNTEXT);
        saved.lpszDefaultScheme=scheme.empty()?nullptr:scheme.data();
        bool restored=SystemParametersInfoW(SPI_SETHIGHCONTRAST,sizeof(saved),&saved,SPIF_SENDCHANGE)!=FALSE;
        Wait(1000);ActivateFrame(h);Capture(h,dir+L"\\mode-restored.png");
        HKEY theme{};DWORD transparency=1,size=4;bool hadTransparency=false;
        if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",0,KEY_QUERY_VALUE|KEY_SET_VALUE,&theme)==ERROR_SUCCESS) {
            hadTransparency=RegQueryValueExW(theme,L"EnableTransparency",nullptr,nullptr,reinterpret_cast<BYTE*>(&transparency),&size)==ERROR_SUCCESS;
            DWORD off=0;RegSetValueExW(theme,L"EnableTransparency",0,REG_DWORD,reinterpret_cast<BYTE*>(&off),4);
            DWORD_PTR result{};SendMessageTimeoutW(HWND_BROADCAST,WM_SETTINGCHANGE,0,reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),SMTO_ABORTIFHUNG,200,&result);
            Wait(700);ActivateFrame(h);Capture(h,dir+L"\\mode-transparency-disabled.png");
            if(hadTransparency)RegSetValueExW(theme,L"EnableTransparency",0,REG_DWORD,reinterpret_cast<BYTE*>(&transparency),4);
            else RegDeleteValueW(theme,L"EnableTransparency");
            SendMessageTimeoutW(HWND_BROADCAST,WM_SETTINGCHANGE,0,reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),SMTO_ABORTIFHUNG,200,&result);RegCloseKey(theme);
        }
        Wait(600);PostMessageW(h,ApplyMessage(),0,0);Wait(400);
        std::ofstream report(dir+L"\\renderer-modes.json");report<<"{\"whiteSet\":"<<(whiteSet?"true":"false")
            <<",\"highContrastObserved\":"<<((actual.dwFlags&HCF_HIGHCONTRASTON)?"true":"false")
            <<",\"activeCaptionColor\":"<<caption<<",\"captionTextColor\":"<<captionText<<",\"buttonTextColor\":"<<buttonText
            <<",\"systemRestored\":"<<(restored?"true":"false")<<",\"providerExplicitlyDisabled\":false,\"transparencySettingExercised\":"<<(theme?"true":"false")<<"}";
        UnhookWindowsHookEx(hook);FreeLibrary(module);Gdiplus::GdiplusShutdown(gdip);CoUninitialize();return whiteSet&&restored?0:1;
    }
    if(argc==4&&!wcscmp(argv[3],L"--caption-ink")) {
        // AE7: tabs in the title bar use the window's own title ink in light,
        // dark, accent (light and dark colours) and DWMBlurGlass modes.
        PostMessageW(h,ApplyMessage(),64,0);Wait(800);
        SetWindowPos(h,HWND_TOPMOST,100,70,1050,650,SWP_SHOWWINDOW);Wait(300);
        HWND strip{};EnumChildWindows(h,[](HWND child,LPARAM value)->BOOL{wchar_t name[128]{};GetClassNameW(child,name,128);if(!wcsncmp(name,L"Win10NativePatch.ExplorerTabs.v2",32)){*reinterpret_cast<HWND*>(value)=child;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&strip));
        const wchar_t* personalize=L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";const wchar_t* dwm=L"Software\\Microsoft\\Windows\\DWM";
        struct Saved{const wchar_t* key;const wchar_t* name;DWORD value;bool present;};
        std::vector<Saved> saved{{personalize,L"AppsUseLightTheme",0,false},{dwm,L"ColorPrevalence",0,false},{dwm,L"AccentColor",0,false}};
        for(auto& s:saved){DWORD size=4;s.present=RegGetValueW(HKEY_CURRENT_USER,s.key,s.name,RRF_RT_REG_DWORD,nullptr,&s.value,&size)==ERROR_SUCCESS;}
        auto set=[&](const wchar_t* key,const wchar_t* name,DWORD value){RegSetKeyValueW(HKEY_CURRENT_USER,key,name,REG_DWORD,&value,4);};
        auto broadcast=[&]{DWORD_PTR result{};SendMessageTimeoutW(HWND_BROADCAST,WM_SETTINGCHANGE,0,reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),SMTO_ABORTIFHUNG,500,&result);Wait(1200);};
        auto darkFrame=[&]{for(DWORD attribute:{20u,19u}){BOOL dark=FALSE;if(SUCCEEDED(DwmGetWindowAttribute(h,attribute,&dark,sizeof(dark))))return dark!=FALSE;}DWORD light=1,size=4;RegGetValueW(HKEY_CURRENT_USER,personalize,L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);return light==0;};
        COLORREF themeActive=RGB(0,0,0),themeInactive=RGB(0,0,0);
        if(HTHEME theme=OpenThemeData(h,L"WINDOW")){GetThemeColor(theme,WP_CAPTION,CS_ACTIVE,TMT_TEXTCOLOR,&themeActive);GetThemeColor(theme,WP_CAPTION,CS_INACTIVE,TMT_TEXTCOLOR,&themeInactive);CloseThemeData(theme);}
        std::ofstream report(dir+L"\\caption-ink.json");report<<"{\"strip\":"<<reinterpret_cast<ULONG_PTR>(strip)<<",\"modes\":[";bool good=strip!=nullptr,first=true;
        auto inkPixels=[&](COLORREF expected){RECT r{};GetWindowRect(strip,&r);const int w=r.right-r.left,hh=r.bottom-r.top;if(w<=0||hh<=0)return 0;
            HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-hh;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits{};
            HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&bits,nullptr,0);int count=0;
            if(bitmap){HGDIOBJ old=SelectObject(dc,bitmap);BitBlt(dc,0,0,w,hh,screen,r.left,r.top,SRCCOPY|CAPTUREBLT);GdiFlush();const auto* p=static_cast<const BYTE*>(bits);
                for(int i=0;i<w*hh;++i){const int d=abs(p[i*4+2]-GetRValue(expected))+abs(p[i*4+1]-GetGValue(expected))+abs(p[i*4]-GetBValue(expected));if(d<=60)++count;}SelectObject(dc,old);DeleteObject(bitmap);}
            DeleteDC(dc);ReleaseDC(nullptr,screen);return count;};
        // exact=false: the theme fallback on an inactive window is faded like
        // DWM's own caption text, so only the reported ink is compared.
        auto measure=[&](const wchar_t* mode,bool active,COLORREF expected,bool exact){
            if(active)ActivateFrame(h);else{SetCursorPos(40,420);INPUT click[2]{};for(auto& i:click)i.type=INPUT_MOUSE;click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,click,sizeof(INPUT));Wait(500);}
            InvalidateRect(strip,nullptr,FALSE);Wait(400);
            const COLORREF ink=static_cast<COLORREF>(reinterpret_cast<ULONG_PTR>(GetPropW(strip,L"Win10NativePatch.CaptionInk")))&0xFFFFFF;
            const int pixels=inkPixels(expected);const bool foreground=GetForegroundWindow()==h;
            const bool pass=ink==expected&&(!exact||pixels>=15)&&foreground==active;good=good&&pass;
            Capture(h,dir+L"\\ink-"+mode+(active?L"-active.png":L"-inactive.png"));
            char name[64]{};WideCharToMultiByte(CP_UTF8,0,mode,-1,name,64,nullptr,nullptr);
            report<<(first?"":",")<<"{\"mode\":\""<<name<<"\",\"active\":"<<(active?"true":"false")<<",\"expected\":"<<expected<<",\"ink\":"<<ink<<",\"pixels\":"<<pixels<<",\"foreground\":"<<(foreground?"true":"false")<<",\"darkFrame\":"<<(darkFrame()?"true":"false")<<",\"passed\":"<<(pass?"true":"false")<<'}';first=false;report.flush();};
        auto both=[&](const wchar_t* mode,COLORREF active,COLORREF inactive,bool inactiveExact){measure(mode,true,active,true);measure(mode,false,inactive,inactiveExact);};
        set(personalize,L"AppsUseLightTheme",1);set(dwm,L"ColorPrevalence",0);broadcast();
        both(L"light",themeActive,themeInactive,false);
        set(personalize,L"AppsUseLightTheme",0);broadcast();
        if(darkFrame())both(L"dark",RGB(255,255,255),RGB(170,170,170),true);else both(L"dark-not-applied",themeActive,themeInactive,false);
        // Accent title bars use DWM's live colorization colour; text is black on
        // light colours and white on dark ones (both rules are unit tested).
        set(personalize,L"AppsUseLightTheme",1);set(dwm,L"ColorPrevalence",1);broadcast();
        DWORD argb{};BOOL opaque{};DwmGetColorizationColor(&argb,&opaque);
        const COLORREF accent=RGB((argb>>16)&0xFF,(argb>>8)&0xFF,argb&0xFF);
        const bool lightAccent=5*GetGValue(accent)+2*GetRValue(accent)+GetBValue(accent)>8*128;
        report<<(first?"":",")<<"{\"accentColour\":"<<accent<<'}';first=false;
        both(L"accent",lightAccent?RGB(0,0,0):RGB(255,255,255),themeInactive,false);
        set(dwm,L"ColorPrevalence",0);broadcast();
        // DWMBlurGlass stand-in: a process with its name beside the user's config.
        wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);const std::wstring stub=std::wstring(temp)+L"BlurGlassStub";
        CreateDirectoryW(stub.c_str(),nullptr);CreateDirectoryW((stub+L"\\data").c_str(),nullptr);
        const bool copied=CopyFileW(L"C:\\Windows\\System32\\PING.EXE",(stub+L"\\DWMBlurGlass.exe").c_str(),FALSE)!=FALSE;
        {std::ofstream ini(stub+L"\\data\\config.ini");ini<<"[config]\r\napplyglobal=false\r\nactiveTextColor=4278190080\r\ninactiveTextColor=4294967295\r\nactiveTextColorDark=4280492835\r\ninactiveTextColorDark=4294967295\r\n";}
        STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
        std::wstring command=L"\""+stub+L"\\DWMBlurGlass.exe\" -n 60 127.0.0.1";
        const bool started=copied&&CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,stub.c_str(),&startup,&process);
        Wait(6000); // the process list is re-read every five seconds
        both(L"blurglass",RGB(0,0,0),RGB(255,255,255),true);
        if(started){TerminateProcess(process.hProcess,0);WaitForSingleObject(process.hProcess,2000);CloseHandle(process.hThread);CloseHandle(process.hProcess);}
        for(auto& s:saved){if(s.present)set(s.key,s.name,s.value);else RegDeleteKeyValueW(HKEY_CURRENT_USER,s.key,s.name);}
        broadcast();PostMessageW(h,ApplyMessage(),0,0);Wait(400);
        report<<"],\"blurGlassStubStarted\":"<<(started?"true":"false")<<",\"themeActive\":"<<themeActive<<",\"themeInactive\":"<<themeInactive<<",\"success\":"<<(good&&started?"true":"false")<<'}';
        UnhookWindowsHookEx(hook);FreeLibrary(module);Gdiplus::GdiplusShutdown(gdip);CoUninitialize();return good&&started?0:1;
    }
    if(argc==4&&!wcscmp(argv[3],L"--renderer-faults")) {
        const auto sessionBefore=SavedSession();PostMessageW(h,ApplyMessage(),72,0);Wait(2700);PostMessageW(h,ApplyMessage(),70,0);Wait(150);
        const bool sessionPreserved=SavedSession()==sessionBefore;const ULONG_PTR creationFlags=reinterpret_cast<ULONG_PTR>(GetPropW(h,FaultProperty));
        const ULONG_PTR creationCalls=reinterpret_cast<ULONG_PTR>(GetPropW(h,L"HeaderLab.GraphicsCalls"));
        Capture(h,dir+L"\\renderer-creation-restored.png");
        PostMessageW(h,ApplyMessage(),0,0);Wait(250);
        PostMessageW(h,ApplyMessage(),72,0);Wait(100);PostMessageW(h,ApplyMessage(),73,0);Wait(900);
        const bool creationRecovered=GetPropW(h,L"Win10NativePatch.TabsActive.v2")!=nullptr;
        PostMessageW(h,ApplyMessage(),0,0);Wait(250);
        PostMessageW(h,ApplyMessage(),74,0);
        // Attachment can be deferred while native caption allocation settles.
        // Inject device loss only after an actually visible production child.
        const auto readyStart=GetTickCount64();
        bool beforeDraw=false;
        do {
            bool visible=false;
            EnumChildWindows(h,[](HWND child,LPARAM context)->BOOL {
                wchar_t name[128]{};GetClassNameW(child,name,128);
                if(!wcsncmp(name,L"Win10NativePatch.ExplorerTabs.v2",32)&&IsWindowVisible(child))
                    *reinterpret_cast<bool*>(context)=true;
                return TRUE;
            },reinterpret_cast<LPARAM>(&visible));
            beforeDraw=visible&&GetPropW(h,L"Win10NativePatch.TabsActive.v2")!=nullptr;
            if(!beforeDraw)Wait(50);
        } while(!beforeDraw&&GetTickCount64()-readyStart<10000);
        const auto readyMilliseconds=GetTickCount64()-readyStart;
        {std::ofstream diagnostic(dir+L"\\renderer-attach-diagnostic.json");
         diagnostic<<"{\"state\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(h,StateProperty))
          <<",\"attachResult\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(h,patch::mvp::AttachResult))
          <<",\"rejected\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(h,L"Win10NativePatch.TabsRejected.v2"))
          <<",\"controller\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(h,L"Win10NativePatch.ExplorerController.v2"))<<"}";}
        if(beforeDraw)PostMessageW(h,ApplyMessage(),75,0);
        Wait(2700);PostMessageW(h,ApplyMessage(),70,0);Wait(150);
        const ULONG_PTR drawFlags=reinterpret_cast<ULONG_PTR>(GetPropW(h,FaultProperty));
        const ULONG_PTR drawCalls=reinterpret_cast<ULONG_PTR>(GetPropW(h,L"HeaderLab.GraphicsCalls"));
        Capture(h,dir+L"\\renderer-device-restored.png");
        PostMessageW(h,ApplyMessage(),0,0);Wait(250);PostMessageW(h,ApplyMessage(),64,0);Wait(800);
        const bool enabledAgain=GetPropW(h,L"Win10NativePatch.TabsActive.v2")!=nullptr;
        PostMessageW(h,ApplyMessage(),0,0);Wait(300);
        const bool cleanup=!GetPropW(h,L"Win10NativePatch.TabsActive.v2")&&!GetPropW(h,L"Win10NativePatch.HeaderRowPixels.v1");
        const bool success=sessionPreserved&&creationFlags==4&&creationCalls&&creationRecovered&&beforeDraw&&drawFlags==4&&drawCalls&&enabledAgain&&cleanup;
        std::ofstream report(dir+L"\\renderer-faults.json");
        report<<"{\"success\":"<<(success?"true":"false")<<",\"sessionPreservedDuringCreationFailure\":"<<(sessionPreserved?"true":"false")<<",\"creationCalls\":"<<creationCalls
              <<",\"creationNativeRestored\":"<<(creationFlags==4?"true":"false")
              <<",\"creationRecoveredWithinBound\":"<<(creationRecovered?"true":"false")
              <<",\"readyBeforeDrawFault\":"<<(beforeDraw?"true":"false")<<",\"readyMilliseconds\":"<<readyMilliseconds<<",\"drawRecreateTargetCalls\":"<<drawCalls
              <<",\"drawNativeRestored\":"<<(drawFlags==4?"true":"false")
              <<",\"explicitEnableRecovered\":"<<(enabledAgain?"true":"false")<<",\"cleanup\":"<<(cleanup?"true":"false")<<"}";
        UnhookWindowsHookEx(hook);FreeLibrary(module);Gdiplus::GdiplusShutdown(gdip);CoUninitialize();return success?0:1;
    }
    if(argc==5&&!wcscmp(argv[3],L"--sta")){
        HWND other=reinterpret_cast<HWND>(wcstoull(argv[4],nullptr,10));DWORD pidA{},pidB{};DWORD threadA=GetWindowThreadProcessId(h,&pidA),threadB=GetWindowThreadProcessId(other,&pidB);
        bool targets=IsWindow(other)&&pidA==pidB&&threadA!=threadB;HHOOK second=targets?SetWindowsHookExW(WH_GETMESSAGE,proc,module,threadB):nullptr;
        if(second){
            PostMessageW(other,ApplyMessage(),68,0);Wait(200);PostMessageW(h,ApplyMessage(),64,0);Wait(500);PostMessageW(other,ApplyMessage(),64,0);Wait(500);
            bool attached=GetPropW(h,StateProperty)==reinterpret_cast<HANDLE>(1)&&GetPropW(other,StateProperty)==reinterpret_cast<HANDLE>(1);
            PostMessageW(h,ApplyMessage(),0,0);Wait(250);PostMessageW(other,ApplyMessage(),0,0);Wait(250);
            ULONG_PTR created=reinterpret_cast<ULONG_PTR>(GetPropW(other,L"HeaderLab.EventsCreated")),removed=reinterpret_cast<ULONG_PTR>(GetPropW(other,L"HeaderLab.EventsRemoved")),wrong=reinterpret_cast<ULONG_PTR>(GetPropW(other,L"HeaderLab.WrongEventThread")),failed=reinterpret_cast<ULONG_PTR>(GetPropW(other,L"HeaderLab.EventFailures"));
            bool success=attached&&created&&created==removed&&!wrong&&!failed&&!GetPropW(h,L"Win10NativePatch.TabsActive.v2")&&!GetPropW(other,L"Win10NativePatch.TabsActive.v2");
            std::ofstream report(dir+L"\\header-sta.json");report<<"{\"success\":"<<(success?"true":"false")<<",\"distinctStaThreads\":true,\"bothAttached\":"<<(attached?"true":"false")<<",\"created\":"<<created<<",\"removed\":"<<removed<<",\"wrongThread\":"<<wrong<<",\"unhookFailures\":"<<failed<<"}\n";
            PostMessageW(other,ApplyMessage(),69,0);Wait(200);UnhookWindowsHookEx(second);UnhookWindowsHookEx(hook);FreeLibrary(module);Gdiplus::GdiplusShutdown(gdip);CoUninitialize();return success?0:1;
        }
        UnhookWindowsHookEx(hook);FreeLibrary(module);Gdiplus::GdiplusShutdown(gdip);CoUninitialize();return 2;
    }
    if(argc==4&&!wcscmp(argv[3],L"--transient")){
        PostMessageW(h,ApplyMessage(),67,0);Wait(200);
        bool rejected=reinterpret_cast<ULONG_PTR>(GetPropW(h,FaultProperty))==4;
        ULONG_PTR injected=reinterpret_cast<ULONG_PTR>(GetPropW(h,FaultCallsProperty));
        Wait(1600);bool recovered=GetPropW(h,L"Win10NativePatch.TabsActive.v2")!=nullptr;
        Commands(h,dir+L"\\commands-recovered.json");
        std::ofstream report(dir+L"\\header-transient.json");report<<"{\"success\":"<<(rejected&&injected&&recovered?"true":"false")<<",\"initialRejectionObserved\":"<<(rejected?"true":"false")<<",\"injectedHeaderCalls\":"<<injected<<",\"recoveredWithoutReattach\":"<<(recovered?"true":"false")<<"}\n";
        PostMessageW(h,ApplyMessage(),0,0);Wait(300);UnhookWindowsHookEx(hook);FreeLibrary(module);Gdiplus::GdiplusShutdown(gdip);CoUninitialize();return rejected&&injected&&recovered?0:1;
    }
    if(argc==4&&!wcscmp(argv[3],L"--fault")){
        PostMessageW(h,ApplyMessage(),66,0);Wait(2600);PostMessageW(h,ApplyMessage(),70,0);Wait(100);
        ULONG_PTR flags=reinterpret_cast<ULONG_PTR>(GetPropW(h,FaultProperty));
        ULONG_PTR calls=reinterpret_cast<ULONG_PTR>(GetPropW(h,FaultCallsProperty));
        std::ofstream report(dir+L"\\header-fallback.json");report<<"{\"success\":"<<(flags==4&&calls?"true":"false")<<",\"injectedHeaderCalls\":"<<calls<<",\"stripAbsent\":"<<(!(flags&1)?"true":"false")<<",\"activeMarkerAbsent\":"<<(!(flags&2)?"true":"false")<<",\"nativeCallbackRestored\":"<<((flags&4)?"true":"false")<<"}\n";
        PostMessageW(h,ApplyMessage(),0,0);Wait(300);UnhookWindowsHookEx(hook);FreeLibrary(module);RemovePropW(h,FaultProperty);
        RemovePropW(h,FaultCallsProperty);Gdiplus::GdiplusShutdown(gdip);CoUninitialize();PostMessageW(h,WM_CLOSE,0,0);return flags==4&&calls?0:1;
    }
    Snapshot(h,dir+L"\\header-before.json");Capture(h,dir+L"\\header-before.png");Commands(h,dir+L"\\commands-before.json");
    PostMessageW(h,ApplyMessage(),argc==4?64:32,0);Wait(500);bool attached=GetPropW(h,StateProperty)==reinterpret_cast<HANDLE>(1);
    Reflow(h,width,height);Snapshot(h,dir+L"\\header-wide.json");Capture(h,dir+L"\\header-wide.png");Commands(h,dir+L"\\commands-wide.json");
    if(argc==4){
        bool before=PaneVisible(h),clicked=ClickCommand(h,L"Navigation pane"),after=PaneVisible(h);
        bool second=ClickCommand(h,L"Navigation pane"),restoredPane=PaneVisible(h);
        std::ofstream interaction(dir+L"\\header-interaction.json");interaction<<"{\"navigationCommandFound\":"<<(clicked?"true":"false")<<",\"nativePaneToggled\":"<<(clicked&&before!=after?"true":"false")<<",\"nativePaneRestored\":"<<(second&&restoredPane==before?"true":"false")<<"}\n";
    }
    Reflow(h,400,height);Snapshot(h,dir+L"\\header-expanded.json");Capture(h,dir+L"\\header-expanded.png");Commands(h,dir+L"\\commands-expanded.json");
    if(argc==4){Reflow(h,width,height);Snapshot(h,dir+L"\\header-wide-again.json");Capture(h,dir+L"\\header-wide-again.png");Commands(h,dir+L"\\commands-wide-again.json");}
    PostMessageW(h,ApplyMessage(),0,0);Wait(500);bool restored=GetPropW(h,StateProperty)==reinterpret_cast<HANDLE>(2);
    Reflow(h,width,height);Snapshot(h,dir+L"\\header-restored.json");Capture(h,dir+L"\\header-restored.png");Commands(h,dir+L"\\commands-restored.json");
    UnhookWindowsHookEx(hook);FreeLibrary(module);RemovePropW(h,StateProperty);RemovePropW(h,CallsProperty);Gdiplus::GdiplusShutdown(gdip);
    CoUninitialize();return attached&&restored?0:1;
}
#endif
