// Disposable-guest physical-path characterization. No production entry points.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <oleacc.h>
#include <exdisp.h>
#include <shldisp.h>
#include <wrl/client.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <gdiplus.h>
#include <dwmapi.h>
#include "HeaderCommands.h"
using Microsoft::WRL::ComPtr;
namespace {
bool Guest(){wchar_t u[256]{};DWORD n=256;return GetUserNameW(u,&n)&&!_wcsicmp(u,L"WDAGUtilityAccount");}
std::string Json(const std::wstring& s){int n=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);std::string b(n,'\0'),o="\"";WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),b.data(),n,nullptr,nullptr);for(unsigned char c:b){if(c=='"'||c=='\\')o+='\\';if(c>=32)o+=static_cast<char>(c);else o+='?';}return o+'"';}
std::wstring Class(HWND h){wchar_t b[128]{};GetClassNameW(h,b,128);return b;}
void Rect(std::ostream& o,RECT r){o<<'['<<r.left<<','<<r.top<<','<<r.right<<','<<r.bottom<<']';}
}
#ifdef HEADER_INTERACTION_DLL
namespace {
void Observe(HWND h,UINT m,WPARAM w,LPARAM l,const char* route){
 if(!Guest()||!(m==WM_NCHITTEST||m==WM_LBUTTONDOWN||m==WM_LBUTTONUP||m==WM_NCLBUTTONDOWN||m==WM_NCLBUTTONUP||m==WM_CAPTURECHANGED||m==WM_CANCELMODE||m==WM_MOUSEACTIVATE||m==WM_SYSCOMMAND||m==WM_ENTERMENULOOP||m==WM_EXITMENULOOP))return;
 HWND frame=GetAncestor(h,GA_ROOT);if(Class(frame)!=L"CabinetWClass")return;
 DWORD test=static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(frame,L"HeaderProof.Case")));if(!test)return;
 std::ostringstream out;out<<"{\"case\":"<<test<<",\"tick\":"<<GetTickCount64()<<",\"thread\":"<<GetCurrentThreadId()<<",\"route\":\""<<route<<"\",\"hwnd\":"<<reinterpret_cast<ULONG_PTR>(h)<<",\"class\":"<<Json(Class(h))<<",\"message\":"<<m<<",\"wParam\":"<<w<<",\"x\":"<<GET_X_LPARAM(l)<<",\"y\":"<<GET_Y_LPARAM(l)<<",\"capture\":"<<reinterpret_cast<ULONG_PTR>(GetCapture())<<"}\n";
 auto s=out.str();HANDLE f=CreateFileW(L"C:\\NativeLabResults\\pointer-events.jsonl",FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,0,nullptr);if(f!=INVALID_HANDLE_VALUE){DWORD n{};WriteFile(f,s.data(),static_cast<DWORD>(s.size()),&n,nullptr);CloseHandle(f);}
}
}
extern "C" __declspec(dllexport) LRESULT CALLBACK HeaderObserveCall(int c,WPARAM w,LPARAM l){if(c>=0){auto p=reinterpret_cast<CWPSTRUCT*>(l);Observe(p->hwnd,p->message,p->wParam,p->lParam,"sent");}return CallNextHookEx(nullptr,c,w,l);}
extern "C" __declspec(dllexport) LRESULT CALLBACK HeaderObserveQueue(int c,WPARAM w,LPARAM l){if(c>=0&&w==PM_REMOVE){auto p=reinterpret_cast<MSG*>(l);Observe(p->hwnd,p->message,p->wParam,p->lParam,"queue");}return CallNextHookEx(nullptr,c,w,l);}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
#else
namespace {
unsigned backgroundPhase{};
void Pump(unsigned delay){ULONGLONG end=GetTickCount64()+delay;while(GetTickCount64()<end){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(10);}}
LRESULT CALLBACK Background(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_PAINT){PAINTSTRUCT p{};HDC dc=BeginPaint(h,&p);RECT r{};GetClientRect(h,&r);if(backgroundPhase<2)FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(backgroundPhase?WHITE_BRUSH:BLACK_BRUSH)));else for(int x=0;x<r.right;x+=80){RECT band{x,0,x+80,r.bottom};FillRect(dc,&band,static_cast<HBRUSH>(GetStockObject((x/80+backgroundPhase)%2?WHITE_BRUSH:BLACK_BRUSH)));}EndPaint(h,&p);return 0;}return DefWindowProcW(h,m,w,l);}
void Capture(HWND h,const std::wstring& path){RECT r{};GetWindowRect(h,&r);HDC desktop=GetDC(nullptr),dc=CreateCompatibleDC(desktop);HBITMAP bitmap=CreateCompatibleBitmap(desktop,r.right-r.left,r.bottom-r.top);HGDIOBJ old=SelectObject(dc,bitmap);BitBlt(dc,0,0,r.right-r.left,r.bottom-r.top,desktop,r.left,r.top,SRCCOPY|CAPTUREBLT);Gdiplus::Bitmap png(bitmap,nullptr);CLSID encoder{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0,0,0xf8,0x1e,0xf3,0x2e}};png.Save(path.c_str(),&encoder,nullptr);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,desktop);}
void PhysicalClick(POINT);
void Material(HWND frame,const std::wstring& directory,const std::wstring& prefix=L"combined-material") {
    WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=Background;wc.lpszClassName=L"HeaderInteraction.MaterialBackdrop";
    RegisterClassExW(&wc);
    HWND background=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"",WS_POPUP|WS_VISIBLE,
                                  40,40,1250,900,nullptr,nullptr,nullptr,nullptr);
    SetWindowPos(frame,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    std::ofstream report(directory+L"\\"+prefix+L"-identity.json");report<<"{\"frame\":"<<reinterpret_cast<ULONG_PTR>(frame)<<",\"phases\":[";
    for(unsigned i=0;i<4;++i) {
        backgroundPhase=i;RedrawWindow(background,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW);
        SetForegroundWindow(frame);RECT outer{};GetWindowRect(frame,&outer);
        PhysicalClick({outer.left+450,outer.top+250});Pump(700);
        GUITHREADINFO info{sizeof(info)};GetGUIThreadInfo(GetWindowThreadProcessId(frame,nullptr),&info);
        if(i)report<<',';report<<"{\"phase\":"<<i<<",\"foreground\":"<<reinterpret_cast<ULONG_PTR>(GetForegroundWindow())
              <<",\"active\":"<<reinterpret_cast<ULONG_PTR>(info.hwndActive)<<",\"focus\":"<<reinterpret_cast<ULONG_PTR>(info.hwndFocus)
              <<",\"frameRect\":["<<outer.left<<','<<outer.top<<','<<outer.right<<','<<outer.bottom<<"]}";
        Capture(frame,directory+L"\\"+prefix+L"-"+std::to_wstring(i)+L".png");
    }
    report<<"]}";DestroyWindow(background);
}
HWND Strip(HWND f){HWND result{};EnumChildWindows(f,[](HWND h,LPARAM p)->BOOL{if(Class(h).find(L"Win10NativePatch.ExplorerTabs.v2")==0&&IsWindowVisible(h))*reinterpret_cast<HWND*>(p)=h;return TRUE;},reinterpret_cast<LPARAM>(&result));return result;}
std::wstring Folder(HWND f){ComPtr<IShellWindows> windows;if(FAILED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&windows))))return L"";long n{};windows->get_Count(&n);for(long i=0;i<n;++i){VARIANT index{};index.vt=VT_I4;index.lVal=i;ComPtr<IDispatch> item;if(FAILED(windows->Item(index,&item)))continue;ComPtr<IWebBrowserApp> browser;if(FAILED(item.As(&browser)))continue;SHANDLE_PTR h{};browser->get_HWND(&h);if(reinterpret_cast<HWND>(h)!=f)continue;BSTR url{};browser->get_LocationURL(&url);std::wstring path=url?url:L"";SysFreeString(url);return path;}return L"";}
struct Target{long child{},state{},role{};std::wstring name;RECT rect{};};
std::vector<Target> Targets(HWND f){std::vector<Target> out;HWND strip=Strip(f);ComPtr<IAccessible> a;if(!strip||FAILED(AccessibleObjectFromWindow(strip,OBJID_CLIENT,IID_IAccessible,reinterpret_cast<void**>(a.GetAddressOf()))))return out;long n{};a->get_accChildCount(&n);for(long i=1;i<=n;++i){VARIANT v{};v.vt=VT_I4;v.lVal=i;Target t;t.child=i;BSTR b{};a->get_accName(v,&b);t.name=b?b:L"";SysFreeString(b);long x{},y{},w{},h{};if(SUCCEEDED(a->accLocation(&x,&y,&w,&h,v)))t.rect={x,y,x+w,y+h};VARIANT s{};if(SUCCEEDED(a->get_accState(v,&s))&&s.vt==VT_I4)t.state=s.lVal;VariantClear(&s);VARIANT role{};if(SUCCEEDED(a->get_accRole(v,&role))&&role.vt==VT_I4)t.role=role.lVal;VariantClear(&role);out.push_back(t);}return out;}
std::vector<Target> PageTabs(HWND f){auto all=Targets(f);std::erase_if(all,[](const Target& t){return t.role!=ROLE_SYSTEM_PAGETAB;});return all;}
Target TargetNamed(HWND f,const std::wstring& name){for(auto& t:Targets(f))if(t.name==name)return t;return {};}
size_t Count(HWND f){return PageTabs(f).size();}
void State(std::ostream& o,HWND f){auto targets=Targets(f);o<<"{\"folder\":"<<Json(Folder(f))<<",\"tabCount\":"<<Count(f)<<",\"strip\":"<<reinterpret_cast<ULONG_PTR>(Strip(f))<<",\"targets\":[";bool first=true;for(auto&t:targets){if(!first)o<<',';first=false;o<<"{\"child\":"<<t.child<<",\"name\":"<<Json(t.name)<<",\"state\":"<<t.state<<",\"role\":"<<t.role<<",\"rect\":";Rect(o,t.rect);o<<'}';}o<<"]}";}
void Key(WORD key,bool up=false){INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wVk=key;i.ki.dwFlags=up?KEYEVENTF_KEYUP:0;SendInput(1,&i,sizeof(i));}
void Chord(WORD key){Key(VK_CONTROL);Key(key);Key(key,true);Key(VK_CONTROL,true);Sleep(700);}
void Navigate(HWND frame,const std::wstring& folder) {
    ComPtr<IShellWindows> windows;
    if(FAILED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&windows))))return;
    long count{};windows->get_Count(&count);
    for(long i=0;i<count;++i) {
        VARIANT index{};index.vt=VT_I4;index.lVal=i;
        ComPtr<IDispatch> item;ComPtr<IWebBrowser2> browser;
        if(FAILED(windows->Item(index,&item))||FAILED(item.As(&browser)))continue;
        SHANDLE_PTR handle{};browser->get_HWND(&handle);
        if(reinterpret_cast<HWND>(handle)!=frame)continue;
        VARIANT destination{};destination.vt=VT_BSTR;destination.bstrVal=SysAllocString(folder.c_str());
        VARIANT empty{};browser->Navigate2(&destination,&empty,&empty,&empty,&empty);VariantClear(&destination);
        Pump(1200);return;
    }
}
bool MenuPresent(){bool found=false;EnumWindows([](HWND h,LPARAM p)->BOOL{if(Class(h)==L"#32768"&&IsWindowVisible(h))*reinterpret_cast<bool*>(p)=true;return TRUE;},reinterpret_cast<LPARAM>(&found));return found;}
bool Pane(HWND frame){bool visible=false;EnumChildWindows(frame,[](HWND h,LPARAM p)->BOOL{if(Class(h)==L"NamespaceTreeControl"&&IsWindowVisible(h))*reinterpret_cast<bool*>(p)=true;return TRUE;},reinterpret_cast<LPARAM>(&visible));return visible;}
void PhysicalClick(POINT p){SetCursorPos(p.x,p.y);INPUT in[2]{};for(auto&i:in)i.type=INPUT_MOUSE;in[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;in[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,in,sizeof(INPUT));Sleep(120);SendInput(1,in+1,sizeof(INPUT));Sleep(1000);}
LRESULT NativeHit(HWND frame,POINT p){SetCursorPos(p.x,p.y);Sleep(30);DWORD_PTR hit{};SendMessageTimeoutW(frame,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y),SMTO_ABORTIFHUNG,1000,&hit);return static_cast<LRESULT>(hit);}
POINT CaptionButton(HWND frame,int index){RECT frameRect{},buttons{};GetWindowRect(frame,&frameRect);DwmGetWindowAttribute(frame,DWMWA_CAPTION_BUTTON_BOUNDS,&buttons,sizeof(buttons));return {frameRect.left+buttons.left+(buttons.right-buttons.left)*(index*2+1)/6,frameRect.top+(buttons.top+buttons.bottom)/2};}
bool NativeOutside(HWND frame,const std::wstring& directory){
 bool paneBefore=Pane(frame),qatFound=false,qatToggle=false,qatRestored=false;header::Inspection inspection;
 EnumChildWindows(frame,[](HWND h,LPARAM value)->BOOL{if(Class(h)!=L"NetUIHWND")return TRUE;ComPtr<IAccessible> a;if(SUCCEEDED(AccessibleObjectFromWindow(h,static_cast<DWORD>(OBJID_CLIENT),IID_IAccessible,reinterpret_cast<void**>(a.GetAddressOf()))))reinterpret_cast<header::Inspection*>(value)->Walk(a.Get());return TRUE;},reinterpret_cast<LPARAM>(&inspection));
 for(auto& command:inspection.commands)if(command.name==L"Navigation pane"&&command.hasRect&&!(command.state&(STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_OFFSCREEN))){qatFound=true;POINT p{(command.rect.left+command.rect.right)/2,(command.rect.top+command.rect.bottom)/2};PhysicalClick(p);qatToggle=Pane(frame)!=paneBefore;PhysicalClick(p);qatRestored=Pane(frame)==paneBefore;break;}
 SetPropW(frame,L"HeaderProof.Case",reinterpret_cast<HANDLE>(21));SetForegroundWindow(frame);Key(VK_MENU);Key(VK_SPACE);Key(VK_SPACE,true);Key(VK_MENU,true);Sleep(250);bool menuEnumerated=MenuPresent();GUITHREADINFO menuInfo{sizeof(menuInfo)};GetGUIThreadInfo(GetWindowThreadProcessId(frame,nullptr),&menuInfo);bool systemMenu=(menuInfo.flags&GUI_INMENUMODE)&&menuInfo.hwndMenuOwner==frame;Key(VK_ESCAPE);Key(VK_ESCAPE,true);Sleep(150);
 SetPropW(frame,L"HeaderProof.Case",reinterpret_cast<HANDLE>(22));POINT maximize=CaptionButton(frame,1);LRESULT maxHit=NativeHit(frame,maximize);PhysicalClick(maximize);bool maximized=IsZoomed(frame)!=FALSE;SetPropW(frame,L"HeaderProof.Case",reinterpret_cast<HANDLE>(23));PhysicalClick(CaptionButton(frame,1));bool restored=!IsZoomed(frame);
 SetPropW(frame,L"HeaderProof.Case",reinterpret_cast<HANDLE>(24));POINT minimize=CaptionButton(frame,0);LRESULT minHit=NativeHit(frame,minimize);PhysicalClick(minimize);bool minimized=IsIconic(frame)!=FALSE;ShowWindow(frame,SW_RESTORE);SetForegroundWindow(frame);Sleep(400);
 RECT before{};GetWindowRect(frame,&before);POINT edge{before.right-2,(before.top+before.bottom)/2};LRESULT resizeHit=NativeHit(frame,edge);POINT blank{};bool found=false;auto targets=Targets(frame);if(targets.size()>=3){auto plus=targets[targets.size()-2].rect,overflow=targets.back().rect;if(overflow.left>plus.right+4){blank={(plus.right+overflow.left)/2,(plus.top+plus.bottom)/2};found=NativeHit(frame,blank)==HTCAPTION;}}
 SetPropW(frame,L"HeaderProof.Case",reinterpret_cast<HANDLE>(25));bool dragged=false;if(found){SetCursorPos(blank.x,blank.y);INPUT in{};in.type=INPUT_MOUSE;in.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;SendInput(1,&in,sizeof(in));Sleep(100);SetCursorPos(blank.x+40,blank.y+20);Sleep(100);in.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&in,sizeof(in));Sleep(400);RECT after{};GetWindowRect(frame,&after);dragged=after.left!=before.left||after.top!=before.top;SetWindowPos(frame,nullptr,before.left,before.top,before.right-before.left,before.bottom-before.top,SWP_NOZORDER|SWP_NOACTIVATE);Sleep(300);}
 RemovePropW(frame,L"HeaderProof.Case");bool stripAlive=Strip(frame)!=nullptr;bool passed=qatFound&&qatToggle&&qatRestored&&systemMenu&&maximized&&restored&&minHit==HTMINBUTTON&&minimized&&resizeHit==HTRIGHT&&found&&dragged&&stripAlive;
 std::ofstream out(directory+L"\\combined-native-outside.json");out<<"{\"success\":"<<(passed?"true":"false")<<",\"qatFound\":"<<(qatFound?"true":"false")<<",\"qatToggled\":"<<(qatToggle?"true":"false")<<",\"qatRestored\":"<<(qatRestored?"true":"false")<<",\"systemMenu\":"<<(systemMenu?"true":"false")<<",\"menuEnumerated\":"<<(menuEnumerated?"true":"false")<<",\"menuFlags\":"<<menuInfo.flags<<",\"menuOwner\":"<<reinterpret_cast<ULONG_PTR>(menuInfo.hwndMenuOwner)<<",\"maxHit\":"<<maxHit<<",\"maximized\":"<<(maximized?"true":"false")<<",\"restored\":"<<(restored?"true":"false")<<",\"minHit\":"<<minHit<<",\"minimized\":"<<(minimized?"true":"false")<<",\"resizeHit\":"<<resizeHit<<",\"nativeBlankDragFound\":"<<(found?"true":"false")<<",\"nativeBlankDragMovedFrame\":"<<(dragged?"true":"false")<<",\"stripAlive\":"<<(stripAlive?"true":"false")<<"}";return passed;
}
void Hit(std::ostream& o,HWND f,HWND strip,POINT p){HWND actual=WindowFromPoint(p);POINT local=p;ScreenToClient(f,&local);HWND child=RealChildWindowFromPoint(f,local);DWORD_PTR rootHit{},childHit{};SendMessageTimeoutW(f,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y),SMTO_ABORTIFHUNG,1000,&rootHit);if(strip)SendMessageTimeoutW(strip,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y),SMTO_ABORTIFHUNG,1000,&childHit);GUITHREADINFO info{sizeof(info)};GetGUIThreadInfo(GetWindowThreadProcessId(f,nullptr),&info);o<<"{\"screen\":["<<p.x<<','<<p.y<<"],\"windowFromPoint\":"<<reinterpret_cast<ULONG_PTR>(actual)<<",\"windowClass\":"<<Json(Class(actual))<<",\"realChild\":"<<reinterpret_cast<ULONG_PTR>(child)<<",\"frameHit\":"<<static_cast<LONG_PTR>(rootHit)<<",\"stripHit\":"<<static_cast<LONG_PTR>(childHit)<<",\"capture\":"<<reinterpret_cast<ULONG_PTR>(info.hwndCapture)<<",\"stripExStyle\":"<<GetWindowLongPtrW(strip,GWL_EXSTYLE)<<'}';}
bool Click(HWND f,std::ostream& out,unsigned index,const char* name,RECT r,bool close=false,bool menu=false){size_t countBefore=Count(f);std::wstring folderBefore=Folder(f);(void)close;POINT p{(r.left+r.right)/2,(r.top+r.bottom)/2};SetPropW(f,L"HeaderProof.Case",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(index)));out<<"{\"case\":"<<index<<",\"action\":\""<<name<<"\",\"before\":";State(out,f);out<<",\"hitBefore\":";Hit(out,f,Strip(f),p);SetCursorPos(p.x,p.y);Sleep(100);INPUT down{};down.type=INPUT_MOUSE;down.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;bool sent=SendInput(1,&down,sizeof(down))==1;Sleep(120);Capture(f,L"C:\\NativeLabResults\\combined-pressed-"+std::to_wstring(index)+L".png");out<<",\"hitAfterDown\":";Hit(out,f,Strip(f),p);down.mi.dwFlags=MOUSEEVENTF_LEFTUP;sent=SendInput(1,&down,sizeof(down))==1&&sent;Sleep(900);bool popup=MenuPresent();Capture(f,L"C:\\NativeLabResults\\combined-action-"+std::to_wstring(index)+L".png");size_t countAfter=Count(f);std::wstring folderAfter=Folder(f);std::string action=name;bool passed=sent&&(action=="select-first-tab"?folderAfter==L"file:///C:/NativeLabFolders/Alpha":action=="new-tab"?countAfter==countBefore+1:action=="close-background-tab"?countAfter+1==countBefore&&folderAfter==folderBefore:action=="close-active-tab"?countAfter+1==countBefore&&folderAfter==L"file:///C:/NativeLabFolders/Gamma":popup);out<<" ,\"passed\":"<<(passed?"true":"false");out<<",\"sent\":"<<(sent?"true":"false")<<",\"popupVisible\":"<<(popup?"true":"false")<<",\"after\":";State(out,f);out<<'}';if(menu||popup){Key(VK_ESCAPE);Key(VK_ESCAPE,true);Sleep(200);}RemovePropW(f,L"HeaderProof.Case");return passed;}
}
void NativeContent(HWND frame,const std::wstring& file) {
    std::ofstream out(file);out<<"{\"rowPixels\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(frame,L"Win10NativePatch.HeaderRowPixels.v1"))<<",\"children\":[";
    struct Context{std::ofstream* out;bool first=true;} context{&out};
    EnumChildWindows(frame,[](HWND child,LPARAM value)->BOOL {
        const auto name=Class(child);
        if(name!=L"Address Band Root"&&name!=L"NamespaceTreeControl"&&name!=L"SHELLDLL_DefView"&&name!=L"ShellTabWindowClass"&&name!=L"SysTreeView32")return TRUE;
        auto& state=*reinterpret_cast<Context*>(value);if(!state.first)*state.out<<',';state.first=false;
        RECT r{};GetWindowRect(child,&r);SCROLLINFO scroll{sizeof(scroll),SIF_POS};GetScrollInfo(child,SB_VERT,&scroll);
        *state.out<<"{\"class\":"<<Json(name)<<",\"rect\":";Rect(*state.out,r);*state.out<<",\"scrollPosition\":"<<scroll.nPos<<"}";return TRUE;
    },reinterpret_cast<LPARAM>(&context));out<<"]}";
}
// These are the real Controller's visuals under delivered guest input. The
// accessible tab body deliberately excludes its independently painted close.
bool VisualStates(HWND frame,const std::wstring& directory) {
    auto tabs=PageTabs(frame);if(tabs.size()<2)return false;
    NativeContent(frame,directory+L"\\native-content-caption.json");auto body=tabs[0].rect;
    SetCursorPos((body.left+body.right)/2,(body.top+body.bottom)/2);Pump(250);
    Capture(frame,directory+L"\\state-hover-body.png");
    const int unit=MulDiv(30,static_cast<int>(GetDpiForWindow(frame)),96);
    const auto closeBounds=TargetNamed(frame,L"Close "+tabs[0].name).rect;
    POINT close{(closeBounds.left+closeBounds.right)/2,(closeBounds.top+closeBounds.bottom)/2};
    SetCursorPos(close.x,close.y);Pump(250);Capture(frame,directory+L"\\state-hover-close.png");
    INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
    SendInput(1,&input,sizeof(input));Pump(200);Capture(frame,directory+L"\\state-pressed-close.png");
    SetCursorPos(body.left,body.top+100);input.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&input,sizeof(input));Pump(150);
    auto selected=tabs[1].rect;PhysicalClick({(selected.left+selected.right)/2,(selected.top+selected.bottom)/2});
    Capture(frame,directory+L"\\state-focus.png");
    HWND focus=CreateWindowExW(0,L"STATIC",L"Owned guest focus fixture",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                              20,400,260,100,nullptr,nullptr,nullptr,nullptr);
    SetForegroundWindow(focus);Pump(300);Capture(frame,directory+L"\\state-inactive.png");
    DestroyWindow(focus);SetForegroundWindow(frame);Pump(200);
    SetWindowPos(frame,nullptr,0,0,400,650,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);Pump(750);
    Capture(frame,directory+L"\\state-allocated-row.png");NativeContent(frame,directory+L"\\native-content-row.json");
    {std::ofstream row(directory+L"\\allocated-row-state.json");State(row,frame);}
    const auto rowTabs=Targets(frame);const size_t before=Count(frame);
    bool rowNew=false;std::ofstream rowHit(directory+L"\\allocated-row-hit.json");
    if(rowTabs.size()>=3) {
        auto plus=rowTabs[rowTabs.size()-2].rect;POINT point{(plus.left+plus.right)/2,(plus.top+plus.bottom)/2};SetPropW(frame,L"HeaderProof.Case",reinterpret_cast<HANDLE>(40));Hit(rowHit,frame,Strip(frame),point);PhysicalClick(point);RemovePropW(frame,L"HeaderProof.Case");
        rowNew=Count(frame)==before+1;
        Pump(1200);auto current=Targets(frame);for(const auto& active:current)if((active.state&STATE_SYSTEM_SELECTED)&&!IsRectEmpty(&active.rect)){const auto closeRect=TargetNamed(frame,L"Close "+active.name).rect;PhysicalClick({(closeRect.left+closeRect.right)/2,(closeRect.top+closeRect.bottom)/2});break;}Pump(1000);
    }
    {std::ofstream report(directory+L"\\allocated-row-input.json");report<<"{\"rowPixels\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(frame,L"Win10NativePatch.HeaderRowPixels.v1"))<<",\"newByPhysicalMouse\":"<<(rowNew?"true":"false")<<",\"countRestored\":"<<(Count(frame)==before?"true":"false")<<"}";}
    const bool rowRestored=Count(frame)==before;Capture(frame,directory+L"\\state-allocated-row-settled.png");SetWindowPos(frame,nullptr,0,0,1050,650,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);Pump(1000);NativeContent(frame,directory+L"\\native-content-wide-again.json");return rowNew&&rowRestored;
}
int wmain(int argc,wchar_t**argv){if(argc!=3||!Guest())return 2;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);HWND f=reinterpret_cast<HWND>(wcstoull(argv[1],nullptr,10));if(Class(f)!=L"CabinetWClass")return 2;CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);ULONG_PTR gdip{};Gdiplus::GdiplusStartupInput gdipInput;Gdiplus::GdiplusStartup(&gdip,&gdipInput,nullptr);std::wstring dir=argv[2];wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);std::wstring base=exe;base=base.substr(0,base.find_last_of(L"\\"));HMODULE lab=LoadLibraryW((base+L"\\HeaderLabHook.dll").c_str()),observer=LoadLibraryW((base+L"\\HeaderInteractionHook.dll").c_str());if(!lab||!observer)return 3;DWORD thread=GetWindowThreadProcessId(f,nullptr);HHOOK attach=SetWindowsHookExW(WH_GETMESSAGE,reinterpret_cast<HOOKPROC>(GetProcAddress(lab,"HeaderLabHook")),lab,thread),call=SetWindowsHookExW(WH_CALLWNDPROC,reinterpret_cast<HOOKPROC>(GetProcAddress(observer,"HeaderObserveCall")),observer,thread),queue=SetWindowsHookExW(WH_GETMESSAGE,reinterpret_cast<HOOKPROC>(GetProcAddress(observer,"HeaderObserveQueue")),observer,thread);if(!attach||!call||!queue)return 4;UINT apply=RegisterWindowMessageW(L"Win10NativePatch.HeaderLab.Apply");PostMessageW(f,apply,64,0);Sleep(1400);ShowWindow(f,SW_RESTORE);SetWindowPos(f,HWND_TOPMOST,120,80,1050,650,SWP_SHOWWINDOW);SetForegroundWindow(f);Sleep(500);PhysicalClick({600,350});Navigate(f,L"C:\\NativeLabFolders\\Alpha");PhysicalClick({600,350});Chord('T');Navigate(f,L"C:\\NativeLabFolders\\Beta");Material(f,dir);bool statesReady=VisualStates(f,dir);bool nativeOutside=NativeOutside(f,dir);
std::ofstream out(dir+L"\\header-physical-input.json");out<<"{\"schemaVersion\":1,\"physicalPath\":true,\"stableTabIdentityObservable\":false,\"actions\":[";auto t=PageTabs(f);bool ready=t.size()>=2;{std::ofstream material(dir+L"\\combined-material-state.json");State(material,f);}bool accepted=ready&&nativeOutside&&statesReady;if(ready){accepted=Click(f,out,1,"select-first-tab",t[0].rect)&&accepted;out<<',';t=Targets(f);accepted=Click(f,out,2,"new-tab",t[t.size()-2].rect)&&accepted;if(Count(f)<3)Chord('T');Navigate(f,L"C:\\NativeLabFolders\\Gamma");Chord('1');out<<',';t=Targets(f);accepted=Click(f,out,3,"close-background-tab",TargetNamed(f,L"Close Beta").rect,true)&&accepted;Chord('1');out<<',';t=Targets(f);accepted=Click(f,out,4,"close-active-tab",TargetNamed(f,L"Close Alpha").rect,true)&&accepted;out<<',';t=Targets(f);accepted=Click(f,out,5,"overflow",t.back().rect,false,true)&&accepted;}out<<"],\"success\":"<<(accepted?"true":"false")<<",\"bootstrapReady\":"<<(ready?"true":"false")<<",\"cleanup\":{";PostMessageW(f,apply,0,0);Sleep(500);out<<"\"stripAbsent\":"<<(!Strip(f)?"true":"false")<<",\"activeMarkerAbsent\":"<<(!GetPropW(f,L"Win10NativePatch.TabsActive.v2")?"true":"false")<<"}}\n";Material(f,dir,L"native-reference");UnhookWindowsHookEx(queue);UnhookWindowsHookEx(call);UnhookWindowsHookEx(attach);FreeLibrary(observer);FreeLibrary(lab);SetWindowPos(f,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);Gdiplus::GdiplusShutdown(gdip);CoUninitialize();return accepted?0:1;}
#endif
