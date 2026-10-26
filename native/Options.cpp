#include "Patch.h"
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <sstream>
#include <algorithm>

namespace {
struct Context {
    patch::Config draft;
    std::array<HWND,4> pages{};
    bool dirty=false;
    bool loading=false;
};
const wchar_t* StateName(patch::EffectiveState state) {
    switch(state) {
    case patch::EffectiveState::Enabled:return L"Enabled";
    case patch::EffectiveState::Disabled:return L"Disabled";
    case patch::EffectiveState::Pending:return L"Pending sign-in";
    default:return L"Unavailable";
    }
}
void Check(HWND h,int id,bool value) {CheckDlgButton(h,id,value?BST_CHECKED:BST_UNCHECKED);}
bool Checked(HWND h,int id) {return IsDlgButtonChecked(h,id)==BST_CHECKED;}
void SetFlag(patch::Config& c,DWORD flag,bool enabled) {if(enabled)c.flags|=flag;else c.flags&=~flag;}
void AddKey(HWND combo,const wchar_t* label,DWORD key) {
    auto row=SendMessage(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
    if(row!=CB_ERR&&row!=CB_ERRSPACE)SendMessage(combo,CB_SETITEMDATA,row,key);
}
void FillKeys(HWND combo,DWORD selected) {
    AddKey(combo,L"Left arrow",VK_LEFT);AddKey(combo,L"Up arrow",VK_UP);
    AddKey(combo,L"Right arrow",VK_RIGHT);AddKey(combo,L"Down arrow",VK_DOWN);
    for(DWORD key=VK_F1;key<=VK_F24;++key) {
        if(key==VK_F12)continue; // Windows reserves F12 for its debugger.
        std::wstring label=L"F"+std::to_wstring(key-VK_F1+1);AddKey(combo,label.c_str(),key);
    }
    for(DWORD key=L'A';key<=L'Z';++key) {wchar_t label[]={static_cast<wchar_t>(key),0};AddKey(combo,label,key);}
    for(LRESULT i=0;i<SendMessage(combo,CB_GETCOUNT,0,0);++i)
        if(static_cast<DWORD>(SendMessage(combo,CB_GETITEMDATA,i,0))==selected) {SendMessage(combo,CB_SETCURSEL,i,0);return;}
}
void ReadPages(Context& c) {
    if(c.pages[0]) {
        SetFlag(c.draft,patch::ExplorerTabs,Checked(c.pages[0],1001));
        SetFlag(c.draft,patch::TouchpadGestures,Checked(c.pages[0],1002));
        SetFlag(c.draft,patch::KeyboardShortcuts,Checked(c.pages[0],1003));
    }
    if(c.pages[1])SetFlag(c.draft,patch::RestoreTabs,Checked(c.pages[1],1101));
    if(c.pages[2]) {
        c.draft.keyboardModifiers=0;
        constexpr DWORD modifiers[]={MOD_CONTROL,MOD_ALT,MOD_WIN,MOD_SHIFT};
        for(int i=0;i<4;++i) {
            if(Checked(c.pages[2],1201+i))c.draft.keyboardModifiers|=modifiers[i];
            HWND combo=GetDlgItem(c.pages[2],1210+i);
            LRESULT row=SendMessage(combo,CB_GETCURSEL,0,0);
            if(row!=CB_ERR)c.draft.keyboardKeys[i]=static_cast<DWORD>(SendMessage(combo,CB_GETITEMDATA,row,0));
        }
    }
}
void UpdateDiagnostics(HWND page) {
    auto d=patch::ReadDiagnostics();
    std::wostringstream out;
    out<<L"Windows 10 22H2 x64: "<<(d.supportedWindows?L"Supported":L"Not supported")
       <<L"\r\nExplorer Tabs installed: "<<(d.tabsInstalled?L"Yes":L"No")
       <<L"\r\nExplorer folder windows: "<<d.explorerWindows
       <<L"\r\nWindows with active tabs: "<<d.tabWindows
       <<L"\r\nSnap helper running: "<<(d.snapRunning?L"Yes":L"No")
       <<L"\r\nTouchpad gestures: "<<StateName(d.gestures.state)
       <<L"\r\n\r\n"<<d.gestures.detail<<L"\r\n"<<d.detail;
    SetDlgItemText(page,1301,out.str().c_str());
}
void UpdateStatus(Context& c) {
    if(c.pages[0]) {
        auto d=patch::ReadDiagnostics();
        auto applied=patch::LoadConfig();
        std::wstring tabs;
        if(!d.tabsInstalled)tabs=L"Not installed. Explorer acceptance must pass before deployment.";
        else if((c.draft.flags^applied.flags)&patch::ExplorerTabs)tabs=L"Change not applied. Select Apply to change Explorer tabs.";
        else if(!(applied.flags&patch::ExplorerTabs))tabs=d.tabWindows?L"Pending: open windows have not confirmed disabling tabs.":L"Disabled. Ordinary Explorer remains available.";
        else if(d.tabWindows)tabs=L"Enabled in "+std::to_wstring(d.tabWindows)+L" Explorer window(s).";
        else tabs=L"Pending: open or reopen an Explorer window to verify tabs.";
        SetDlgItemText(c.pages[0],1011,tabs.c_str());
        EnableWindow(GetDlgItem(c.pages[0],1001),d.tabsInstalled&&d.supportedWindows);
        std::wstring gestures=StateName(d.gestures.state);
        if((c.draft.flags^applied.flags)&patch::TouchpadGestures)gestures=L"Change not applied. Current state: "+gestures;
        if(!d.gestures.detail.empty())gestures+=L". "+d.gestures.detail;
        SetDlgItemText(c.pages[0],1012,gestures.c_str());
        bool helperExists=GetFileAttributes(patch::SiblingPath(L"NativeSnap.exe").c_str())!=INVALID_FILE_ATTRIBUTES;
        EnableWindow(GetDlgItem(c.pages[0],1002),d.gestures.precisionTouchpad&&helperExists);
        EnableWindow(GetDlgItem(c.pages[0],1003),helperExists);
        DWORD activeInputs=static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(FindWindowW(patch::SnapWindowClass,nullptr),L"Win10NativePatch.ActiveInputs.v2")));
        const wchar_t* keys=((c.draft.flags^applied.flags)&patch::KeyboardShortcuts)?L"Change not applied. Select Apply to change keyboard shortcuts.":!(applied.flags&patch::KeyboardShortcuts)?L"Disabled. Touchpad gestures are controlled separately.":
            (activeInputs&patch::KeyboardShortcuts)?L"Enabled. Change bindings on the Snapping page.":L"Pending: keyboard input is paused or has not been confirmed.";
        SetDlgItemText(c.pages[0],1013,keys);
    }
    if(c.pages[2])for(int id=1201;id<=1213;++id) {
        HWND control=GetDlgItem(c.pages[2],id);
        if(control)EnableWindow(control,(c.draft.flags&patch::KeyboardShortcuts)!=0);
    }
    if(c.pages[3])UpdateDiagnostics(c.pages[3]);
}
LRESULT CALLBACK SheetProc(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR ref) {
    if(message==patch::SettingsChangedMessage())UpdateStatus(*reinterpret_cast<Context*>(ref));
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,SheetProc,1);
    return DefSubclassProc(window,message,w,l);
}
void Dirty(HWND page,Context& c) {
    if(c.loading)return;
    ReadPages(c);c.dirty=true;PropSheet_Changed(GetParent(page),page);UpdateStatus(c);
}
bool Apply(HWND page,Context& c) {
    if(!c.dirty)return true;
    ReadPages(c);std::wstring error;patch::GestureStatus status;
    bool gesturesChanged=((patch::LoadConfig().flags^c.draft.flags)&patch::TouchpadGestures)!=0;
    HCURSOR previous=SetCursor(LoadCursor(nullptr,IDC_WAIT));
    bool ok=patch::ApplyConfig(c.draft,status,error);
    SetCursor(previous);
    if(!ok) {MessageBox(page,error.c_str(),L"Options could not be applied",MB_OK|MB_ICONERROR);return false;}
    c.draft=patch::LoadConfig();c.dirty=false;UpdateStatus(c);
    if(gesturesChanged&&status.state==patch::EffectiveState::Pending)
        MessageBox(page,L"Your settings are saved. Sign out and sign back in to finish applying the touchpad change.",L"Sign-in required",MB_OK|MB_ICONINFORMATION);
    return true;
}
INT_PTR CALLBACK PageProc(HWND page,UINT message,WPARAM w,LPARAM l) {
    auto c=reinterpret_cast<Context*>(GetWindowLongPtr(page,GWLP_USERDATA));
    if(message==WM_INITDIALOG) {
        auto descriptor=reinterpret_cast<PROPSHEETPAGE*>(l);c=reinterpret_cast<Context*>(descriptor->lParam);
        SetWindowLongPtr(page,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(c));
        int index=static_cast<int>(reinterpret_cast<ULONG_PTR>(descriptor->pszTemplate))-201;
        c->pages[index]=page;c->loading=true;
        if(index==0)SetWindowSubclass(GetParent(page),SheetProc,1,reinterpret_cast<DWORD_PTR>(c));
        if(index==0) {
            Check(page,1001,(c->draft.flags&patch::ExplorerTabs)!=0);Check(page,1002,(c->draft.flags&patch::TouchpadGestures)!=0);
            Check(page,1003,(c->draft.flags&patch::KeyboardShortcuts)!=0);
        } else if(index==1)Check(page,1101,(c->draft.flags&patch::RestoreTabs)!=0);
        else if(index==2) {
            constexpr DWORD modifiers[]={MOD_CONTROL,MOD_ALT,MOD_WIN,MOD_SHIFT};
            for(int i=0;i<4;++i) {Check(page,1201+i,(c->draft.keyboardModifiers&modifiers[i])!=0);FillKeys(GetDlgItem(page,1210+i),c->draft.keyboardKeys[i]);}
        }
        c->loading=false;UpdateStatus(*c);return TRUE;
    }
    if(!c)return FALSE;
    if(message==WM_COMMAND) {
        if(LOWORD(w)==1302) {UpdateStatus(*c);return TRUE;}
        if(LOWORD(w)==1303) {
            // Stage the change; Cancel must never mutate Windows settings.
            SetFlag(c->draft,patch::TouchpadGestures,false);
            if(c->pages[0])Check(c->pages[0],1002,false);
            c->dirty=true;PropSheet_Changed(GetParent(page),page);UpdateStatus(*c);return TRUE;
        }
        if(HIWORD(w)==BN_CLICKED||HIWORD(w)==CBN_SELCHANGE)Dirty(page,*c);
    } else if(message==WM_NOTIFY) {
        auto notification=reinterpret_cast<NMHDR*>(l);
        if(notification->code==PSN_APPLY) {
            SetWindowLongPtr(page,DWLP_MSGRESULT,Apply(page,*c)?PSNRET_NOERROR:PSNRET_INVALID_NOCHANGEPAGE);return TRUE;
        }
        if(notification->code==PSN_SETACTIVE)UpdateStatus(*c);
    } else if(message==patch::SettingsChangedMessage()) {UpdateStatus(*c);return TRUE;}
    return FALSE;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLine(),&argc);
    std::vector<std::wstring> arguments;
    if(argv){for(int i=1;i<argc;++i)arguments.emplace_back(argv[i]);LocalFree(argv);}
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int result=0;
    if(arguments.size()==2&&arguments[0]==L"--self-test")result=patch::RunConfigSelfTests(arguments[1]);
    else if(arguments.size()==2&&arguments[0]==L"--probe")result=patch::RunGestureProbe(arguments[1]);
    else if(!arguments.empty())result=2;
    else {
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_WIN95_CLASSES};InitCommonControlsEx(&controls);
        Context context;context.draft=patch::LoadConfig();
        std::array<PROPSHEETPAGE,4> pages{};
        constexpr const wchar_t* titles[]={L"Features",L"Explorer",L"Snapping",L"Diagnostics"};
        for(int i=0;i<4;++i) {
            pages[i].dwSize=sizeof(PROPSHEETPAGE);pages[i].dwFlags=PSP_USETITLE;pages[i].hInstance=instance;
            pages[i].pszTemplate=MAKEINTRESOURCE(201+i);pages[i].pszTitle=titles[i];
            pages[i].pfnDlgProc=PageProc;pages[i].lParam=reinterpret_cast<LPARAM>(&context);
        }
        PROPSHEETHEADER sheet{};sheet.dwSize=sizeof(sheet);sheet.dwFlags=PSH_PROPSHEETPAGE|PSH_USEHICON|PSH_NOCONTEXTHELP;
        sheet.hInstance=instance;sheet.hIcon=LoadIcon(instance,MAKEINTRESOURCE(101));sheet.pszCaption=L"Windows 10 Native Patch — Options";
        sheet.nPages=static_cast<UINT>(pages.size());sheet.ppsp=pages.data();
        INT_PTR outcome=PropertySheet(&sheet);result=outcome<0?1:0;
    }
    if(SUCCEEDED(com))CoUninitialize();return result;
}
