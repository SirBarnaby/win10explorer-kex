// Guest-only visual/DPI fixture using the production renderer. This does not
// stand in for the actual Explorer input/material acceptance harness.
#include "TabHeaderRenderer.h"
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <fstream>

namespace {
bool Guest(){wchar_t user[256]{};DWORD count=256;return GetUserNameW(user,&count)&&!_wcsicmp(user,L"WDAGUtilityAccount");}
void Pump(unsigned time){auto until=GetTickCount64()+time;while(GetTickCount64()<until){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}}
void Capture(HWND window,const std::wstring& file){
    RECT r{};GetWindowRect(window,&r);HDC desktop=GetDC(nullptr),dc=CreateCompatibleDC(desktop);
    HBITMAP bitmap=CreateCompatibleBitmap(desktop,r.right-r.left,r.bottom-r.top);auto old=SelectObject(dc,bitmap);
    BitBlt(dc,0,0,r.right-r.left,r.bottom-r.top,desktop,r.left,r.top,SRCCOPY|CAPTUREBLT);
    Gdiplus::Bitmap image(bitmap,nullptr);CLSID encoder{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0,0,0xf8,0x1e,0xf3,0x2e}};
    image.Save(file.c_str(),&encoder,nullptr);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,desktop);
}
LRESULT CALLBACK Window(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_ERASEBKGND)return 1;return DefWindowProcW(h,m,w,l);}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2||!Guest())return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ULONG_PTR gdip{};Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&gdip,&input,nullptr);
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Window;wc.lpszClassName=L"HeaderRenderer.GuestStateFixture";
    RegisterClassW(&wc);
    HWND host=CreateWindowExW(0,wc.lpszClassName,L"Renderer state fixture (not Explorer acceptance)",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE,40,80,1160,460,nullptr,nullptr,wc.hInstance,nullptr);
    HWND child=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"",WS_CHILD|WS_VISIBLE,0,80,1100,60,host,nullptr,wc.hInstance,nullptr);
    SHSTOCKICONINFO icon{sizeof(icon)};SHGetStockIconInfo(SIID_FOLDER,SHGSI_ICON|SHGSI_SMALLICON,&icon);
    const std::uint64_t ids[]{1,2,3};
    header::VisualState state;state.active=true;state.focused=true;
    state.tabs={{1,1,L"Alpha",icon.hIcon},{2,1,L"日本語 — Résumé — 📁 — a long folder label whose Unicode text must trim without splitting glyphs",icon.hIcon},{3,1,L"Gamma",icon.hIcon}};
    bool success=host&&child&&icon.hIcon;
    std::ofstream report(std::wstring(argv[1])+L"\\renderer-state-fixture.json");report<<"{\"productionRendererSource\":true,\"actualExplorerAcceptance\":false,\"drawResults\":[";
    {
        header::TabHeaderRenderer renderer;
        unsigned index=0;
        for(UINT dpi:{96u,120u,144u,192u}) {
            const int width=MulDiv(540,static_cast<int>(dpi),96),height=MulDiv(30,static_cast<int>(dpi),96);
            SetWindowPos(child,nullptr,0,80,width,height,SWP_NOACTIVATE|SWP_NOZORDER);
            header::Allocation allocation;allocation.region=header::Region::AllocatedRow;allocation.bounds={0,0,width,height};allocation.dpi=dpi;
            state.layout=header::BuildLayout(allocation,ids,2,0);
            const HRESULT hr=renderer.Paint(child,state);success=SUCCEEDED(hr)&&success;
            Pump(150);Capture(host,std::wstring(argv[1])+L"\\renderer-dpi-"+std::to_wstring(dpi)+L".png");
            if(index++)report<<',';report<<static_cast<LONG>(hr);
        }
        state.busyTab=state.layout.active;state.canCreate=false;
        HRESULT hr=renderer.Paint(child,state);success=SUCCEEDED(hr)&&success;
        Pump(150);Capture(host,std::wstring(argv[1])+L"\\renderer-busy-disabled.png");
        report<<','<<static_cast<LONG>(hr);
        state.busyTab=0;renderer.ThemeChanged();
        hr=renderer.Paint(child,state);success=SUCCEEDED(hr)&&success;
        Pump(150);Capture(host,std::wstring(argv[1])+L"\\renderer-create-disabled.png");report<<','<<static_cast<LONG>(hr);
        renderer.Reset();
    }
    DestroyWindow(child);DestroyWindow(host);if(icon.hIcon)DestroyIcon(icon.hIcon);
    report<<"],\"success\":"<<(success?"true":"false")<<",\"ownedWindowsReleased\":true}";
    Gdiplus::GdiplusShutdown(gdip);CoUninitialize();return success?0:1;
}
