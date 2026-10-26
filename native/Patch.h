#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <array>
#include <vector>

namespace patch {
inline constexpr wchar_t SettingsKey[] = L"Software\\Win10NativePatch\\v2";
inline constexpr wchar_t SnapWindowClass[] = L"Win10NativePatch.NativeSnap.v2";
inline constexpr wchar_t TabsActiveProperty[] = L"Win10NativePatch.TabsActive.v2";
inline constexpr wchar_t TabsClassId[] = L"{73F7B7DC-C022-4862-A3EA-7BD04EAE73A9}";
enum Feature : DWORD { ExplorerTabs=1, TouchpadGestures=2, KeyboardShortcuts=4, RestoreTabs=8 };
struct Config {
    DWORD size=sizeof(Config);
    DWORD version=2;
    DWORD flags=ExplorerTabs|TouchpadGestures;
    DWORD keyboardModifiers=MOD_CONTROL|MOD_ALT|MOD_WIN;
    std::array<DWORD,4> keyboardKeys{VK_LEFT,VK_UP,VK_RIGHT,VK_DOWN};
    std::array<DWORD,8> reserved{};
};
enum class EffectiveState { Disabled, Enabled, Pending, Unavailable };
struct GestureStatus {
    EffectiveState state=EffectiveState::Unavailable;
    bool precisionTouchpad=false;
    bool privateBindingsActive=false;
    std::wstring detail;
};
struct Diagnostics {
    bool supportedWindows=false;
    bool tabsInstalled=false;
    unsigned explorerWindows=0;
    unsigned tabWindows=0;
    bool snapRunning=false;
    GestureStatus gestures;
    std::wstring detail;
};
Config LoadConfig();
bool ValidateConfig(const Config&,std::wstring& error);
bool SaveConfig(const Config&,std::wstring& error);
bool ApplyConfig(const Config&,GestureStatus&,std::wstring& error);
bool ConfigureGestures(bool enable,GestureStatus&,std::wstring& error);
GestureStatus ReadGestureStatus();
bool VerifyGestureProfile(std::wstring& detail);
bool ReconcileSnapRuntime(const Config&,std::wstring& error);
bool SupportedWindows();
bool TabsInstalled();
Diagnostics ReadDiagnostics();
UINT SettingsChangedMessage();
void NotifySettingsChanged();
void OpenOptions(HWND owner=nullptr);
std::wstring ModuleDirectory();
std::wstring SiblingPath(const wchar_t* name);
void Log(const wchar_t* component,const std::wstring& message);
int RunConfigSelfTests(const std::wstring& reportPath);
int RunGestureProbe(const std::wstring& reportPath);
// Gesture transport is deliberately separate from optional physical arrow shortcuts.
inline constexpr std::array<DWORD,4> GestureKeys{VK_F13,VK_F14,VK_F15,VK_F16};
inline constexpr DWORD GestureModifiers=MOD_CONTROL|MOD_ALT|MOD_WIN;
inline constexpr DWORD TouchpadModifierEncoding=0x15;
}
