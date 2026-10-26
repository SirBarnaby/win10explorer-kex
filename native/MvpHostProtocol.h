#pragma once
#include <windows.h>

namespace patch::mvp {
inline constexpr wchar_t HostClass[] = L"Win10NativePatch.ExplorerMvpHost.v1";
inline constexpr wchar_t AttachResult[] = L"Win10NativePatch.MvpAttachResult.v1";
inline UINT AttachMessage() { static UINT value=RegisterWindowMessageW(L"Win10NativePatch.MvpAttach.v1");return value; }
inline UINT DetachMessage() { static UINT value=RegisterWindowMessageW(L"Win10NativePatch.MvpDetach.v1");return value; }
}
