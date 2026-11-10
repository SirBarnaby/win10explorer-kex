#include "Patch.h"
#include "MvpHostProtocol.h"
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <fstream>
#include <cstring>

namespace patch {
std::pair<unsigned,unsigned> RunGestureModelSelfTests();
bool RollbackGestureAttempt(std::wstring& error);
namespace {
constexpr wchar_t RunKey[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t EventName[]=L"Local\\Win10NativePatch.SettingsChanged.v2";
struct ConfigLock {HANDLE h{};bool owned=false;ConfigLock(){h=CreateMutexW(nullptr,FALSE,L"Local\\Win10NativePatch.Config.v2");if(h){DWORD r=WaitForSingleObject(h,3000);owned=r==WAIT_OBJECT_0||r==WAIT_ABANDONED;}}~ConfigLock(){if(owned)ReleaseMutex(h);if(h)CloseHandle(h);}};
bool Store(HKEY root,const wchar_t* path,const Config& c) {
    HKEY key{};LONG opened=RegCreateKeyExW(root,path,0,nullptr,0,KEY_SET_VALUE|KEY_QUERY_VALUE,nullptr,&key,nullptr);if(opened!=ERROR_SUCCESS){SetLastError(opened);return false;}
    DWORD oldType{},oldSize{};LONG oldResult=RegQueryValueExW(key,L"Config",nullptr,&oldType,nullptr,&oldSize);
    if(oldResult!=ERROR_SUCCESS&&oldResult!=ERROR_FILE_NOT_FOUND){RegCloseKey(key);return false;}
    if(oldSize>65536){RegCloseKey(key);return false;}std::vector<BYTE> old(oldSize);
    if(oldResult==ERROR_SUCCESS&&RegQueryValueExW(key,L"Config",nullptr,&oldType,old.data(),&oldSize)!=ERROR_SUCCESS){RegCloseKey(key);return false;}
    LONG code=RegSetValueExW(key,L"Config",0,REG_BINARY,reinterpret_cast<const BYTE*>(&c),sizeof(c));
    if(code==ERROR_SUCCESS&&RegFlushKey(key)!=ERROR_SUCCESS){if(oldResult==ERROR_FILE_NOT_FOUND)RegDeleteValueW(key,L"Config");else RegSetValueExW(key,L"Config",0,oldType,old.data(),oldSize);RegFlushKey(key);code=ERROR_WRITE_FAULT;}
    RegCloseKey(key);SetLastError(code);return code==ERROR_SUCCESS;
}
bool Read(HKEY root,const wchar_t* path,Config& c) {
    DWORD type{},size=sizeof(c);return RegGetValueW(root,path,L"Config",RRF_RT_REG_BINARY,&type,&c,&size)==ERROR_SUCCESS&&size==sizeof(c);
}
}
bool ValidateConfig(const Config& c,std::wstring& error) {
    if(c.size!=sizeof(Config)||c.version!=2){error=L"The settings format is not supported.";return false;}
    if(c.flags&~DWORD(ExplorerTabs|TouchpadGestures|KeyboardShortcuts|RestoreTabs)){error=L"The settings contain unknown features.";return false;}
    if(std::any_of(c.reserved.begin(),c.reserved.end(),[](DWORD n){return n!=0;})){error=L"The settings contain unknown fields.";return false;}
    if(!c.keyboardModifiers||(c.keyboardModifiers&~DWORD(MOD_CONTROL|MOD_ALT|MOD_SHIFT|MOD_WIN))){error=L"Choose a valid keyboard modifier combination.";return false;}
    if(c.keyboardModifiers==MOD_WIN){error=L"The built-in Windows + Arrow shortcuts are preserved. Add Ctrl, Alt or Shift.";return false;}
    for(size_t i=0;i<c.keyboardKeys.size();i++) {
        DWORD key=c.keyboardKeys[i];
        if(key<8||key>254||key==VK_SHIFT||key==VK_CONTROL||key==VK_MENU||key==VK_LWIN||key==VK_RWIN||key==VK_F12){error=L"Choose a valid key for each direction.";return false;}
        for(size_t j=0;j<i;j++)if(key==c.keyboardKeys[j]){error=L"Each direction needs a different keyboard shortcut.";return false;}
        if(c.keyboardModifiers==GestureModifiers&&std::find(GestureKeys.begin(),GestureKeys.end(),key)!=GestureKeys.end()){error=L"That shortcut is reserved for touchpad gestures.";return false;}
    }
    error.clear();return true;
}
Config LoadConfig() {
    Config c,found;c.flags=0;std::wstring error;if(Read(HKEY_CURRENT_USER,SettingsKey,found)&&ValidateConfig(found,error))c=found;return c;
}
bool SaveConfig(const Config& c,std::wstring& error) {
    ConfigLock lock;if(!lock.owned){error=L"Another settings update is running.";return false;}
    if(!ValidateConfig(c,error))return false;
    if(!Store(HKEY_CURRENT_USER,SettingsKey,c)){error=L"Windows could not save the settings.";return false;}
    NotifySettingsChanged();return true;
}
UINT SettingsChangedMessage(){static UINT m=RegisterWindowMessageW(L"Win10NativePatch.SettingsChanged.v2");return m;}
void NotifySettingsChanged() {
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,EventName);if(event){SetEvent(event);CloseHandle(event);}
    SendNotifyMessageW(HWND_BROADCAST,SettingsChangedMessage(),0,0);
}
std::wstring ModuleDirectory() {
    HMODULE module{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&ModuleDirectory),&module);
    std::wstring path(32768,L'\0');DWORD n=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if(!n||n>=path.size())return {};path.resize(n);size_t slash=path.find_last_of(L"\\/");return slash==std::wstring::npos?L"":path.substr(0,slash);
}
std::wstring SiblingPath(const wchar_t* name){wchar_t dir[32768];for(HKEY hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}){DWORD size=sizeof(dir);if(RegGetValueW(hive,SettingsKey,L"InstallDirectory",RRF_RT_REG_SZ,nullptr,dir,&size)==ERROR_SUCCESS){std::wstring target=dir;if(!target.empty()&&target.back()!=L'\\'&&target.back()!=L'/')target+=L'\\';target+=name;DWORD attr=GetFileAttributesW(target.c_str());if(attr!=INVALID_FILE_ATTRIBUTES&&!(attr&FILE_ATTRIBUTE_DIRECTORY))return target;}}return ModuleDirectory()+L"\\"+name;}
void OpenOptions(HWND owner){HWND frame=owner?GetAncestor(owner,GA_ROOT):nullptr;wchar_t type[64]{};if(frame)GetClassNameW(frame,type,64);std::wstring args=frame&&!wcscmp(type,L"CabinetWClass")?L"--owner "+std::to_wstring(reinterpret_cast<ULONG_PTR>(frame)):L"";ShellExecuteW(owner,L"open",SiblingPath(L"Win10PatchOptions.exe").c_str(),args.empty()?nullptr:args.c_str(),ModuleDirectory().c_str(),SW_SHOWNORMAL);}
void Log(const wchar_t* component,const std::wstring& message){std::wstring text=L"[Win10NativePatch/"+std::wstring(component)+L"] "+message+L"\n";OutputDebugStringW(text.c_str());}
bool SupportedWindows() {
    using RtlGetVersionFn=LONG(WINAPI*)(OSVERSIONINFOW*);HMODULE nt=GetModuleHandleW(L"ntdll.dll");
    auto address=GetProcAddress(nt,"RtlGetVersion");RtlGetVersionFn fn{};static_assert(sizeof(fn)==sizeof(address));std::memcpy(&fn,&address,sizeof(fn));
    SYSTEM_INFO system{};GetNativeSystemInfo(&system);
    OSVERSIONINFOW os{sizeof(os)};return system.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64&&fn&&fn(&os)==0&&os.dwMajorVersion==10&&os.dwBuildNumber==19045;
}
bool TabsInstalled(){if(GetFileAttributesW(SiblingPath(L"ExplorerMvpHost.exe").c_str())!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(SiblingPath(L"ExplorerTabs.dll").c_str())!=INVALID_FILE_ATTRIBUTES)return true;DWORD features{},length=sizeof(features);if(RegGetValueW(HKEY_LOCAL_MACHINE,SettingsKey,L"InstalledFeatures",RRF_RT_REG_DWORD,nullptr,&features,&length)!=ERROR_SUCCESS||!(features&ExplorerTabs))return false;std::wstring path=L"CLSID\\"+std::wstring(TabsClassId)+L"\\InprocServer32";wchar_t dll[32768];DWORD size=sizeof(dll);return RegGetValueW(HKEY_CLASSES_ROOT,path.c_str(),nullptr,RRF_RT_REG_SZ,nullptr,dll,&size)==ERROR_SUCCESS&&!_wcsicmp(dll,SiblingPath(L"ExplorerTabs.dll").c_str())&&GetFileAttributesW(dll)!=INVALID_FILE_ATTRIBUTES;}
Diagnostics ReadDiagnostics() {
    Diagnostics d;d.supportedWindows=SupportedWindows();d.tabsInstalled=TabsInstalled();d.snapRunning=FindWindowW(SnapWindowClass,nullptr)!=nullptr;d.gestures=ReadGestureStatus();
    DWORD activeInputs=static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(FindWindowW(SnapWindowClass,nullptr),L"Win10NativePatch.ActiveInputs.v2")));
    if(d.gestures.state==EffectiveState::Enabled&&!(activeInputs&TouchpadGestures)){d.gestures.state=EffectiveState::Pending;d.gestures.detail=L"Windows gesture assignments are active, but snapping is paused or its receiver has not confirmed startup.";}
    EnumWindows([](HWND h,LPARAM data)->BOOL{auto& d=*reinterpret_cast<Diagnostics*>(data);wchar_t c[80];GetClassNameW(h,c,80);if(!_wcsicmp(c,L"CabinetWClass")){d.explorerWindows++;if(GetPropW(h,TabsActiveProperty))d.tabWindows++;if(GetPropW(h,L"Win10NativePatch.TabsRejected.v2"))d.detail=L"At least one Explorer window has unsupported caption geometry or could not isolate native caption painting. Tabs stayed off in that window.";}return TRUE;},reinterpret_cast<LPARAM>(&d));return d;
}
bool ReconcileSnapRuntime(const Config& c,std::wstring& error) {
    GestureStatus status=ReadGestureStatus();bool needed=(c.flags&KeyboardShortcuts)||status.privateBindingsActive;
    HKEY run{};LONG result=RegCreateKeyExW(HKEY_CURRENT_USER,RunKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&run,nullptr);
    if(result!=ERROR_SUCCESS){error=L"Windows could not update startup settings.";return false;}
    if(needed){std::wstring command=L"\""+SiblingPath(L"NativeSnap.exe")+L"\"";result=RegSetValueExW(run,L"Win10NativePatch.NativeSnap.v2",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));}
    else {result=RegDeleteValueW(run,L"Win10NativePatch.NativeSnap.v2");if(result==ERROR_FILE_NOT_FOUND)result=ERROR_SUCCESS;}
    RegCloseKey(run);if(result!=ERROR_SUCCESS){error=L"Windows could not update startup settings.";return false;}
    HWND h=FindWindowW(SnapWindowClass,nullptr);
    if(h){DWORD_PTR response{};ULONGLONG deadline=GetTickCount64()+1500;do{if(!SendMessageTimeoutW(h,SettingsChangedMessage(),1,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&response))break;if(response==1)return true;if(response!=2)break;Sleep(30);}while(GetTickCount64()<deadline);error=L"Native Snap could not apply the shortcuts. Check for a conflicting shortcut utility.";return false;}
    if(!needed)return true;
    std::wstring app=SiblingPath(L"NativeSnap.exe"),command=L"\""+app+L"\"";
    STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
    if(!CreateProcessW(app.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,ModuleDirectory().c_str(),&si,&pi)){error=L"Native Snap could not start.";return false;}
    CloseHandle(pi.hThread);bool ready=false;ULONGLONG deadline=GetTickCount64()+5000;
    while(GetTickCount64()<deadline){if(WaitForSingleObject(pi.hProcess,30)==WAIT_OBJECT_0)break;h=FindWindowW(SnapWindowClass,nullptr);if(h){DWORD_PTR response{};if(SendMessageTimeoutW(h,SettingsChangedMessage(),1,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,1500,&response)){ready=response==1;if(response!=2)break;}else break;}}
    CloseHandle(pi.hProcess);if(!ready){error=L"Native Snap could not register its shortcuts.";return false;}return true;
}
bool ReconcileTabsRuntime(const Config& config,std::wstring& error) {
    const std::wstring app=SiblingPath(L"ExplorerMvpHost.exe");
    if(GetFileAttributesW(app.c_str())==INVALID_FILE_ATTRIBUTES)return true;
    const bool needed=(config.flags&ExplorerTabs)!=0;
    HKEY run{};LONG code=RegCreateKeyExW(HKEY_CURRENT_USER,RunKey,0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&run,nullptr);
    if(code!=ERROR_SUCCESS){error=L"Windows could not update Explorer startup settings.";return false;}
    std::wstring command=L"\""+app+L"\"";
    if(needed)code=RegSetValueExW(run,L"Win10NativePatch.ExplorerMvp.v1",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
    else {wchar_t existing[32768]{};DWORD type{},bytes=sizeof(existing);LONG read=RegQueryValueExW(run,L"Win10NativePatch.ExplorerMvp.v1",nullptr,&type,reinterpret_cast<BYTE*>(existing),&bytes);if(read==ERROR_SUCCESS&&type==REG_SZ&&bytes>=sizeof(wchar_t)&&existing[bytes/sizeof(wchar_t)-1]==0&&!_wcsicmp(existing,command.c_str()))code=RegDeleteValueW(run,L"Win10NativePatch.ExplorerMvp.v1");}
    RegCloseKey(run);
    if(code!=ERROR_SUCCESS){error=L"Windows could not save Explorer startup settings.";return false;}
    HWND host=FindWindowW(mvp::HostClass,nullptr);
    if(!needed){
        if(!host)return true;
        DWORD pid{};GetWindowThreadProcessId(host,&pid);HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
        const bool stopped=process&&mvp::StopHost(host,process,false);if(process)CloseHandle(process);
        if(!stopped)error=L"Explorer integration has not finished stopping. Wait a moment and try again.";
        return stopped;
    }
    if(host){
        const ULONGLONG deadline=GetTickCount64()+4000;
        while(IsWindow(host)&&mvp::Stopping(host)&&GetTickCount64()<deadline)Sleep(30);
        if(IsWindow(host)){
            if(mvp::Stopping(host)){error=L"Explorer integration is still stopping. Recovery has not confirmed a ready runtime.";return false;}
            PostMessageW(host,SettingsChangedMessage(),0,0);return true;
        }
    }
    STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    if(!CreateProcessW(app.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,ModuleDirectory().c_str(),&start,&process)){error=L"Explorer integration could not start.";return false;}
    CloseHandle(process.hThread);bool ready=false;ULONGLONG deadline=GetTickCount64()+4000;
    while(GetTickCount64()<deadline){if(WaitForSingleObject(process.hProcess,30)==WAIT_OBJECT_0)break;if(FindWindowW(mvp::HostClass,nullptr)){ready=true;break;}}
    CloseHandle(process.hProcess);if(!ready)error=L"Explorer integration did not confirm startup.";return ready;
}
bool ApplyConfig(const Config& c,GestureStatus& status,std::wstring& error) {
    ConfigLock lock;if(!lock.owned){error=L"Another settings update is running.";return false;}
    if(!SupportedWindows()){error=L"This patch requires Windows 10 22H2 (build 19045).";return false;}
    if(!ValidateConfig(c,error))return false;Config before=LoadConfig();
    DWORD importedBytes{};
    bool legacyDisable=!(c.flags&TouchpadGestures)&&RegGetValueW(HKEY_CURRENT_USER,SettingsKey,L"LegacyGestureBaseline",RRF_RT_REG_BINARY,nullptr,nullptr,&importedBytes)==ERROR_SUCCESS;
    bool changed=(((before.flags^c.flags)&TouchpadGestures)!=0)||legacyDisable;
    status=ReadGestureStatus();
    if(changed&&!ConfigureGestures((c.flags&TouchpadGestures)!=0,status,error)){std::wstring ignored;ReconcileSnapRuntime(before,ignored);return false;}
    if(!SaveConfig(c,error)||!ReconcileSnapRuntime(c,error)||!ReconcileTabsRuntime(c,error)) {
        std::wstring ignored;Store(HKEY_CURRENT_USER,SettingsKey,before);
        if(changed)RollbackGestureAttempt(ignored);
        status=ReadGestureStatus();
        ReconcileSnapRuntime(before,ignored);ReconcileTabsRuntime(before,ignored);NotifySettingsChanged();return false;
    }
    NotifySettingsChanged();return true;
}
int RunConfigSelfTests(const std::wstring& reportPath) {
    unsigned passed=0,total=0;auto test=[&](bool ok){total++;if(ok)passed++;};std::wstring e;Config c;
    test(ValidateConfig(c,e));c.version=99;test(!ValidateConfig(c,e));c=Config{};c.flags|=0x80000000;test(!ValidateConfig(c,e));c=Config{};c.reserved[3]=1;test(!ValidateConfig(c,e));c=Config{};c.keyboardModifiers=MOD_WIN;test(!ValidateConfig(c,e));c=Config{};c.keyboardKeys[1]=c.keyboardKeys[0];test(!ValidateConfig(c,e));c=Config{};c.keyboardKeys[0]=VK_F13;test(!ValidateConfig(c,e));c=Config{};c.keyboardModifiers=MOD_CONTROL|MOD_ALT;test(ValidateConfig(c,e));
    for(DWORD featureFlags=0;featureFlags<16;featureFlags++){Config combination;combination.flags=featureFlags;test(ValidateConfig(combination,e));}
    std::wstring isolated=L"Software\\Win10NativePatch\\IsolatedTest-"+std::to_wstring(GetCurrentProcessId());Config roundtrip;
    bool stored=Store(HKEY_CURRENT_USER,isolated.c_str(),c);DWORD storeError=GetLastError();bool read=Read(HKEY_CURRENT_USER,isolated.c_str(),roundtrip),equal=!std::memcmp(&c,&roundtrip,sizeof(c));
    test(stored&&read&&equal);RegDeleteTreeW(HKEY_CURRENT_USER,isolated.c_str());
    unsigned configPassed=passed;
    auto gestureTests=RunGestureModelSelfTests();passed+=gestureTests.first;total+=gestureTests.second;
    std::string json="{\"success\":"+std::string(passed==total?"true":"false")+",\"passed\":"+std::to_string(passed)+",\"total\":"+std::to_string(total)+",\"configPassed\":"+std::to_string(configPassed)+",\"gestureModelPassed\":"+std::to_string(gestureTests.first)+",\"atomicStore\":"+(stored?"true":"false")+",\"atomicStoreError\":"+std::to_string(storeError)+",\"atomicRead\":"+(read?"true":"false")+",\"atomicEqual\":"+(equal?"true":"false")+",\"liveSettingsModified\":false}\n";
    HANDLE file=CreateFileW(reportPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);if(file==INVALID_HANDLE_VALUE)return 2;DWORD n{};bool written=WriteFile(file,json.data(),static_cast<DWORD>(json.size()),&n,nullptr)&&n==json.size();CloseHandle(file);return written?(passed==total?0:1):2;
}
}
