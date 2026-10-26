#include "Patch.h"
#include "LegacyMigration.h"
#include <msi.h>
#include <msiquery.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>

namespace {
constexpr wchar_t ProductCode[]=L"{9F62E90D-90FB-48CF-A445-B01E1DCFF200}";
constexpr wchar_t LegacyRun[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
struct State {std::wstring msi;bool accepted{},busy{};};
bool Property(const std::wstring& msi,const wchar_t* key,std::wstring& value) {
    MSIHANDLE db{},view{},record{};if(MsiOpenDatabaseW(msi.c_str(),MSIDBOPEN_READONLY,&db)!=ERROR_SUCCESS)return false;
    std::wstring query=L"SELECT `Value` FROM `Property` WHERE `Property`='"+std::wstring(key)+L"'";
    bool ok=MsiDatabaseOpenViewW(db,query.c_str(),&view)==ERROR_SUCCESS&&MsiViewExecute(view,0)==ERROR_SUCCESS&&MsiViewFetch(view,&record)==ERROR_SUCCESS;
    if(ok){wchar_t text[32768];DWORD size=32768;ok=MsiRecordGetStringW(record,1,text,&size)==ERROR_SUCCESS;if(ok)value.assign(text,size);}
    if(record)MsiCloseHandle(record);if(view)MsiCloseHandle(view);MsiCloseHandle(db);return ok;
}
bool Installed(){return MsiQueryProductStateW(ProductCode)==INSTALLSTATE_DEFAULT;}
bool NativeSnapInstalled(){return MsiQueryFeatureStateW(ProductCode,L"Snap")==INSTALLSTATE_LOCAL;}
std::wstring LocalPath(const wchar_t* leaf){PWSTR local{};if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_CREATE,nullptr,&local)))return {};std::wstring dir=std::wstring(local)+L"\\Win10NativePatch";CoTaskMemFree(local);CreateDirectoryW(dir.c_str(),nullptr);return dir+L"\\"+leaf;}
bool RunMsi(HWND owner,const std::wstring& arguments,std::wstring& error) {
    wchar_t system[32768];UINT n=GetSystemDirectoryW(system,32768);if(!n||n>=32768){error=L"The Windows Installer path is unavailable.";return false;}
    std::wstring app=std::wstring(system)+L"\\msiexec.exe";
    std::wstring params=arguments+L" /qb! /norestart REBOOT=ReallySuppress MSIRESTARTMANAGERCONTROL=Disable /l*v \""+LocalPath(L"native-v2-setup.log")+L"\"";
    SHELLEXECUTEINFOW info{sizeof(info)};info.fMask=SEE_MASK_NOCLOSEPROCESS;info.hwnd=owner;info.lpVerb=L"runas";info.lpFile=app.c_str();info.lpParameters=params.c_str();info.nShow=SW_HIDE;
    if(!ShellExecuteExW(&info)){error=GetLastError()==ERROR_CANCELLED?L"Administrator approval was cancelled. Installation was not started.":L"Windows Installer could not start.";return false;}
    // Keep repainting this dialog; DisableWindow prevents a second operation.
    bool done=false;MSG message{};
    while(!done){DWORD wait=MsgWaitForMultipleObjects(1,&info.hProcess,FALSE,INFINITE,QS_ALLINPUT);if(wait==WAIT_OBJECT_0)done=true;else if(wait==WAIT_OBJECT_0+1){while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessage(&message);}}else{error=L"Waiting for Windows Installer failed.";break;}}
    DWORD exitCode=ERROR_INSTALL_FAILURE;if(done)GetExitCodeProcess(info.hProcess,&exitCode);CloseHandle(info.hProcess);
    if(exitCode==ERROR_SUCCESS||exitCode==ERROR_SUCCESS_REBOOT_REQUIRED)return true;
    error=exitCode==ERROR_INSTALL_USEREXIT?L"Installation was cancelled. Windows Installer rolled back its changes.":L"Windows Installer returned "+std::to_wstring(exitCode)+L". See the setup log in LocalAppData\\Win10NativePatch.";return false;
}
bool EmptyRows(MSIHANDLE database,const wchar_t* query) {
    MSIHANDLE view{},row{};bool empty=false;
    if(MsiDatabaseOpenViewW(database,query,&view)==ERROR_SUCCESS&&MsiViewExecute(view,0)==ERROR_SUCCESS)empty=MsiViewFetch(view,&row)==ERROR_NO_MORE_ITEMS;
    if(row)MsiCloseHandle(row);if(view)MsiCloseHandle(view);return empty;
}
bool RetireOwnedQt(HWND owner,std::wstring& error) {
    constexpr wchar_t qtProduct[]=L"{8E3584AF-23BA-4E2E-B490-8B58D7FB180D}";
    if(MsiQueryProductStateW(qtProduct)!=INSTALLSTATE_DEFAULT)return true;
    // OpenProduct applies the installed product's cached transforms. Refuse an
    // uninstall whose effective database could kill Explorer or shared keys.
    MSIHANDLE product{};if(MsiOpenProductW(qtProduct,&product)!=ERROR_SUCCESS){error=L"QTTabBar's installed recovery database is unavailable. It was preserved.";return false;}
    MSIHANDLE database=MsiGetActiveDatabase(product);
    bool safe=database&&EmptyRows(database,L"SELECT `Action` FROM `InstallExecuteSequence` WHERE `Action`='KillExplorer' OR `Action`='WaitAfterKill' OR `Action`='StartExplorer'")&&EmptyRows(database,L"SELECT `Registry` FROM `Registry` WHERE `Root`=2 AND `Key`='Software\\Microsoft\\Internet Explorer\\Toolbar' AND (`Name`='*' OR `Name`='-')");
    if(database)MsiCloseHandle(database);MsiCloseHandle(product);
    if(!safe){error=L"QTTabBar retirement was blocked: its effective MSI still contains Explorer restart actions or shared-key removal. The working snap helper and old extension are preserved.";return false;}
    if(!RunMsi(owner,L"/x "+std::wstring(qtProduct),error))return false;
    if(MsiQueryProductStateW(qtProduct)==INSTALLSTATE_DEFAULT){error=L"Windows has not confirmed QTTabBar removal. Reopen or sign in before retrying the cutover.";return false;}
    return true;
}
bool RemoveLegacySnapIfSafe(std::wstring& error) {
    auto status=patch::ReadGestureStatus();if(status.state!=patch::EffectiveState::Enabled&&status.state!=patch::EffectiveState::Disabled)return true;
    DWORD journalBytes{};
    bool nativeCycle=RegGetValueW(HKEY_CURRENT_USER,patch::SettingsKey,L"GestureCycle",RRF_RT_REG_BINARY,nullptr,nullptr,&journalBytes)==ERROR_SUCCESS||RegGetValueW(HKEY_CURRENT_USER,patch::SettingsKey,L"LastGestureCycle",RRF_RT_REG_BINARY,nullptr,nullptr,&journalBytes)==ERROR_SUCCESS;
    if(!nativeCycle)return true;
    PWSTR local{};if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)))return true;
    std::wstring app=std::wstring(local)+L"\\Programs\\Win10NativePatch\\NativeSnap.exe";CoTaskMemFree(local);
    HKEY run{};if(RegOpenKeyExW(HKEY_CURRENT_USER,LegacyRun,0,KEY_QUERY_VALUE|KEY_SET_VALUE,&run)!=ERROR_SUCCESS)return true;
    wchar_t command[32768];DWORD size=sizeof(command),type{};
    // v1's ownership name and full executable must both match.
    LONG result=RegQueryValueExW(run,L"Win10NativeSnap",nullptr,&type,reinterpret_cast<BYTE*>(command),&size);
    bool owned=result==ERROR_SUCCESS&&type==REG_SZ&&size>=sizeof(wchar_t)&&size<=sizeof(command)&&command[size/sizeof(wchar_t)-1]==0&&(!_wcsicmp(command,app.c_str())||!_wcsicmp(command,(L"\""+app+L"\"").c_str()));
    if(owned){RegDeleteValueW(run,L"Win10NativeSnap");HWND old=FindWindowW(L"Win10NativePatch.NativeSnap.v1",nullptr);if(old)PostMessageW(old,WM_CLOSE,0,0);}
    RegCloseKey(run);
    if(owned){
        constexpr wchar_t oldArp[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Win10NativePatch";
        wchar_t location[32768];DWORD bytes=sizeof(location);std::wstring expected=app.substr(0,app.find_last_of(L'\\'));
        if(RegGetValueW(HKEY_CURRENT_USER,oldArp,L"InstallLocation",RRF_RT_REG_SZ,nullptr,location,&bytes)==ERROR_SUCCESS&&!_wcsicmp(location,expected.c_str()))RegDeleteTreeW(HKEY_CURRENT_USER,oldArp);
    }
    error.clear();return true;
}
void Refresh(HWND dialog,State& state) {
    bool supported=patch::SupportedWindows(),installed=Installed();
    bool package=GetFileAttributesW(state.msi.c_str())!=INVALID_FILE_ATTRIBUTES;
    EnableWindow(GetDlgItem(dialog,1401),supported&&state.accepted&&!state.busy);
    EnableWindow(GetDlgItem(dialog,1402),supported&&!state.busy);
    EnableWindow(GetDlgItem(dialog,1403),supported&&package&&!state.busy);
    EnableWindow(GetDlgItem(dialog,1404),supported&&installed&&!state.busy);
    EnableWindow(GetDlgItem(dialog,1405),installed&&!state.busy);
    std::wstring text=!supported?L"This package supports Windows 10 22H2 x64 (build 19045) only.":!package?L"Build-only preview. No installable MSI is present. Current Explorer and snapping are unchanged.":!state.accepted?L"Explorer deployment is gated: clean-Windows title-bar, appearance, reliability and performance acceptance has not passed. The existing Explorer extension is preserved.":L"Explorer acceptance passed for this package. Components can be installed and removed independently.";
    SetDlgItemTextW(dialog,1410,text.c_str());
}
bool Change(HWND dialog,State& state,int action,std::wstring& error) {
    bool tabs=IsDlgButtonChecked(dialog,1401)==BST_CHECKED&&state.accepted;
    bool snap=IsDlgButtonChecked(dialog,1402)==BST_CHECKED;
    patch::Config old=patch::LoadConfig(),next=old;bool hadNativeSnap=NativeSnapInstalled();
    // Do all read-only compatibility/migration checks before requesting elevation.
    migration::Legacy legacy;
    if(!migration::ReadLegacy(LocalPath(L"installation.clixml"),legacy,error))return false;
    if(action==1404)return RunMsi(dialog,L"/fa "+std::wstring(ProductCode),error);
    if(action==1405) {
        next.flags=0;patch::GestureStatus status;
        if(!patch::ApplyConfig(next,status,error))return false;
        if(status.state==patch::EffectiveState::Pending){error=L"Gesture restoration is pending. Sign out and sign back in, then run Remove all again. The helper remains available until restoration is confirmed.";return false;}
        if(!RunMsi(dialog,L"/x "+std::wstring(ProductCode),error)) {std::wstring ignored;patch::ApplyConfig(old,status,ignored);return false;}
        RemoveLegacySnapIfSafe(error);return true;
    }
    if(!tabs&&(old.flags&patch::ExplorerTabs))next.flags&=~patch::ExplorerTabs;
    if(!snap)next.flags&=~DWORD(patch::TouchpadGestures|patch::KeyboardShortcuts);
    patch::GestureStatus status=patch::ReadGestureStatus();
    // Removed components are disabled before their binaries/registration go away.
    if(next.flags!=old.flags&&!patch::ApplyConfig(next,status,error))return false;
    if(!snap&&hadNativeSnap&&status.state==patch::EffectiveState::Pending){error=L"Gesture restoration is pending sign-in. Sign out and sign in before removing Snap.";return false;}
    std::wstring add=L"Core",remove;
    if(tabs)add+=L",Explorer";else remove=L"Explorer";
    if(snap)add+=L",Snap";else remove+=remove.empty()?L"Snap":L",Snap";
    std::wstring args=L"/i \""+state.msi+L"\" ADDLOCAL="+add;if(!remove.empty())args+=L" REMOVE="+remove;
    if(!RunMsi(dialog,args,error)){if(next.flags!=old.flags){std::wstring ignored;patch::ApplyConfig(old,status,ignored);}return false;}
    // Defaults apply only to a new user record. Existing choices survive repair.
    DWORD bytes{};bool configured=RegGetValueW(HKEY_CURRENT_USER,patch::SettingsKey,L"Config",RRF_RT_REG_BINARY,nullptr,nullptr,&bytes)==ERROR_SUCCESS;
    if(!configured){next=patch::Config{};next.flags=(tabs?patch::ExplorerTabs:0)|(snap?patch::TouchpadGestures:0);if(snap&&!patch::ReadGestureStatus().precisionTouchpad)next.flags&=~patch::TouchpadGestures;}
    if(legacy.found&&!migration::PrepareLegacyImport(error))return false;
    if(!patch::ApplyConfig(next,status,error))return false;
    RemoveLegacySnapIfSafe(error);
    // The release builder requires clean-VM and cutover acceptance. Use only the
    // owned product and its already-transformed MSI; never call v1 Uninstall.
    if(tabs&&legacy.qtOwned&&!RetireOwnedQt(dialog,error)) {
        next.flags&=~patch::ExplorerTabs;std::wstring ignored;patch::ApplyConfig(next,status,ignored);return false;
    }
    return true;
}
INT_PTR CALLBACK Dialog(HWND dialog,UINT message,WPARAM w,LPARAM l) {
    State* state=reinterpret_cast<State*>(GetWindowLongPtrW(dialog,GWLP_USERDATA));
    if(message==WM_INITDIALOG){state=reinterpret_cast<State*>(l);SetWindowLongPtrW(dialog,GWLP_USERDATA,l);CheckDlgButton(dialog,1401,state->accepted&&patch::TabsInstalled()?BST_CHECKED:BST_UNCHECKED);CheckDlgButton(dialog,1402,BST_CHECKED);SendMessageW(dialog,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101))));Refresh(dialog,*state);return TRUE;}
    if(!state)return FALSE;
    if(message==WM_CLOSE||(message==WM_COMMAND&&LOWORD(w)==IDCANCEL)){if(!state->busy)EndDialog(dialog,0);return TRUE;}
    if(message==WM_COMMAND&&(LOWORD(w)==1403||LOWORD(w)==1404||LOWORD(w)==1405)) {
        state->busy=true;Refresh(dialog,*state);std::wstring error;bool ok=Change(dialog,*state,LOWORD(w),error);state->busy=false;Refresh(dialog,*state);
        MessageBoxW(dialog,error.empty()?L"The operation completed. Open Options from the Start menu to configure the installed components.":error.c_str(),ok?L"Setup":L"Setup needs attention",MB_OK|(ok?MB_ICONINFORMATION:MB_ICONERROR));return TRUE;
    }return FALSE;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);int result=0;
    if(argv&&argc==3&&!wcscmp(argv[1],L"--self-test"))result=migration::SelfTest(argv[2]);
    else if(argv&&argc==2&&!wcscmp(argv[1],L"--emergency-disable")){patch::Config c=patch::LoadConfig();c.flags=0;patch::GestureStatus status;std::wstring error;result=patch::ApplyConfig(c,status,error)?0:1;MessageBoxW(nullptr,result?error.c_str():status.state==patch::EffectiveState::Pending?L"Components are disabled. Sign out and sign in to finish restoring gestures.":L"Components are disabled. Explorer will return to ordinary chrome as its windows process the settings event.",L"Emergency disable",MB_OK|(result?MB_ICONERROR:MB_ICONINFORMATION));}
    else if(argv&&(argc==1||(argc==2&&!wcscmp(argv[1],L"--uninstall")))){INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_STANDARD_CLASSES};InitCommonControlsEx(&cc);State state;state.msi=patch::ModuleDirectory()+L"\\Windows10NativePatch.msi";std::wstring value;
        if(GetFileAttributesW(state.msi.c_str())==INVALID_FILE_ATTRIBUTES&&Installed()){wchar_t cached[32768];DWORD size=32768;if(MsiGetProductInfoW(ProductCode,INSTALLPROPERTY_LOCALPACKAGE,cached,&size)==ERROR_SUCCESS)state.msi=cached;}
        state.accepted=Property(state.msi,L"ProductCode",value)&&value==ProductCode&&Property(state.msi,L"EXPLORERACCEPTED",value)&&value==L"1";result=DialogBoxParamW(instance,MAKEINTRESOURCEW(301),nullptr,Dialog,reinterpret_cast<LPARAM>(&state))<0?1:0;}
    else result=2;
    if(argv)LocalFree(argv);if(SUCCEEDED(hr))CoUninitialize();return result;
}
