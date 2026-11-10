#pragma once
#include <windows.h>

namespace patch::mvp {
inline constexpr wchar_t HostClass[] = L"Win10NativePatch.ExplorerMvpHost.v1";
inline constexpr wchar_t AttachResult[] = L"Win10NativePatch.MvpAttachResult.v1";
inline constexpr wchar_t SessionHandoffCapability[] = L"Win10NativePatch.MvpSessionHandoff.v1";
inline constexpr wchar_t StopRequest[] = L"Win10NativePatch.MvpStopRequest.v1";
inline constexpr wchar_t StopCommitted[] = L"Win10NativePatch.MvpStopCommitted.v1";
inline bool Stopping(HWND host){return GetPropW(host,StopRequest)||GetPropW(host,StopCommitted);}
inline bool StopHost(HWND host,HANDLE process,bool preserve,DWORD timeout=5000){
    ULONG_PTR request=0;
    if(GetPropW(host,SessionHandoffCapability)){
        if(Stopping(host))return WaitForSingleObject(process,timeout)==WAIT_OBJECT_0;
        request=((static_cast<ULONG_PTR>(GetCurrentProcessId())<<32)|(GetTickCount64()&0xffffffffu))&(~ULONG_PTR{}>>1);
        if(!request)request=1;
        if(!SetPropW(host,StopRequest,reinterpret_cast<HANDLE>(request)))return false;
    }
    if(PostMessageW(host,WM_CLOSE,preserve?1:0,static_cast<LPARAM>(request))&&WaitForSingleObject(process,timeout)==WAIT_OBJECT_0)return true;
    if(request){
        if(reinterpret_cast<ULONG_PTR>(GetPropW(host,StopRequest))==request)RemovePropW(host,StopRequest);
        // An uncommitted callback observes cancellation before detaching. A
        // committed stop remains visible until the process exits, so recovery
        // cannot mistake a host on its way out for a ready runtime.
        DWORD_PTR acknowledged{};
        SendMessageTimeoutW(host,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&acknowledged);
        if(WaitForSingleObject(process,0)==WAIT_OBJECT_0)return true;
    }
    return false;
}
// Bit0 requests a preserved session; the remaining bits bind cancellable stops.
inline WPARAM DetachParameters(bool preserve,ULONG_PTR request){return (request<<1)|(preserve?1:0);}
inline ULONG_PTR DetachRequest(WPARAM parameters){return parameters>>1;}
inline bool PreserveSession(WPARAM parameters){return (parameters&1)!=0;}
inline UINT AttachMessage() { static UINT value=RegisterWindowMessageW(L"Win10NativePatch.MvpAttach.v1");return value; }
inline UINT DetachMessage() { static UINT value=RegisterWindowMessageW(L"Win10NativePatch.MvpDetach.v1");return value; }
}
