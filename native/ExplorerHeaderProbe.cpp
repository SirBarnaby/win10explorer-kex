// Read-only acceptance probe. Never loads the extension or changes Explorer.
#include <windows.h>
#include "HeaderCommands.h"
#include <oleacc.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
std::string Json(const std::wstring& value) {
    int size=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string bytes(size,'\0'),result="\"";
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),bytes.data(),size,nullptr,nullptr);
    constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:bytes) {
        if(c=='"'||c=='\\'){result+='\\';result+=static_cast<char>(c);}
        else if(c<32){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}
        else result+=static_cast<char>(c);
    }
    return result+'"';
}
using header::Inspection;
void Rect(std::ostream& out,const RECT& r){out<<'['<<r.left<<','<<r.top<<','<<r.right<<','<<r.bottom<<']';}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count{};auto argv=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!argv||(count!=3&&count!=4)){if(argv)LocalFree(argv);return 2;}
    bool semantic=count==4&&!wcscmp(argv[3],L"--semantic");
    wchar_t* end{};unsigned long long value=wcstoull(argv[1],&end,10);
    HWND frame=reinterpret_cast<HWND>(static_cast<ULONG_PTR>(value));
    std::wstring path=argv[2];bool parsed=value&&end&&!*end;LocalFree(argv);
    wchar_t name[128]{};
    if(!parsed||!IsWindow(frame)||GetAncestor(frame,GA_ROOT)!=frame||
       !GetClassNameW(frame,name,128)||wcscmp(name,L"CabinetWClass"))return 2;
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com))return 3;
    struct Windows { HWND strip{},navigation{};std::vector<HWND> ribbons; } windows;
    EnumChildWindows(frame,[](HWND h,LPARAM data)->BOOL {
        auto& w=*reinterpret_cast<Windows*>(data);wchar_t cls[128]{};GetClassNameW(h,cls,128);
        if(wcsncmp(cls,L"Win10NativePatch.ExplorerTabs.v2",32)==0&&IsWindowVisible(h))w.strip=h;
        if(!wcscmp(cls,L"NetUIHWND")&&IsWindowVisible(h))w.ribbons.push_back(h);
        if(!wcscmp(cls,L"Address Band Root")&&IsWindowVisible(h))w.navigation=GetParent(GetParent(h));
        return TRUE;
    },reinterpret_cast<LPARAM>(&windows));
    Inspection inspection;
    inspection.collectVisualContent=semantic;
    for(HWND ribbon:windows.ribbons) {
        ComPtr<IAccessible> accessible;
        if(SUCCEEDED(AccessibleObjectFromWindow(ribbon,static_cast<DWORD>(OBJID_CLIENT),IID_IAccessible,
                reinterpret_cast<void**>(accessible.GetAddressOf()))))inspection.Walk(accessible.Get());
        else ++inspection.errors;
    }
    RECT strip{};if(windows.strip)GetWindowRect(windows.strip,&strip);
    RECT navigation{},overlap{};if(windows.navigation)GetWindowRect(windows.navigation,&navigation);
    bool navigationCollision=windows.strip&&windows.navigation&&IntersectRect(&overlap,&strip,&navigation);
    unsigned collisions{};
    std::ostringstream out;
    wchar_t title[2048]{};GetWindowTextW(frame,title,2048);
    out<<"{\"schemaVersion\":1,\"window\":"<<value<<",\"windowTitle\":"<<Json(title)<<",\"semanticContentRequested\":"<<(semantic?"true":"false")<<",\"readOnly\":true,\"tabsActive\":"
       <<(windows.strip?"true":"false")<<",\"tabRect\":";Rect(out,strip);
    out<<",\"ribbonCount\":"<<windows.ribbons.size()<<",\"nativeCommands\":[";
    bool first=true;
    for(const auto& command:inspection.commands) {
        RECT intersection{};
        bool visible=command.hasState&&!(command.state&(STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_OFFSCREEN));
        bool intersects=windows.strip&&command.hasRect&&IntersectRect(&intersection,&strip,&command.rect);
        bool collision=intersects&&visible;
        if(collision)++collisions;
        if(!first)out<<',';first=false;
        out<<"{\"name\":"<<Json(command.name)<<",\"role\":"<<command.role<<",\"state\":"<<command.state
           <<",\"visible\":"<<(visible?"true":"false")<<",\"boundsKnown\":"<<(command.hasRect?"true":"false")
           <<",\"rect\":";Rect(out,command.rect);
        out<<",\"intersectsTabRect\":"<<(intersects?"true":"false")<<",\"collision\":"<<(collision?"true":"false")<<'}';
    }
    bool complete=!windows.ribbons.empty()&&!inspection.commands.empty()&&!inspection.errors&&!inspection.truncated;
    bool accepted=windows.strip&&windows.navigation&&complete&&!collisions&&!navigationCollision;
    out<<"],\"nodes\":"<<inspection.nodes<<",\"errors\":"<<inspection.errors<<",\"truncated\":"
       <<(inspection.truncated?"true":"false")<<",\"complete\":"<<(complete?"true":"false")
       <<",\"collisionCount\":"<<collisions<<",\"noCommandOverlap\":"<<(accepted?"true":"false")
       <<",\"navigationRect\":";Rect(out,navigation);
    out<<",\"navigationOverlap\":"<<(navigationCollision?"true":"false")<<",\"appearanceVerified\":false,\"interactionVerified\":false}\n";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    bool written=false;
    if(file!=INVALID_HANDLE_VALUE) {
        auto json=out.str();DWORD bytes{};
        written=WriteFile(file,json.data(),static_cast<DWORD>(json.size()),&bytes,nullptr)&&bytes==json.size();
        CloseHandle(file);
    }
    // Release all accessible proxies before shutting down the COM apartment.
    inspection.identities.clear();CoUninitialize();
    return !written?3:accepted?0:1;
}
