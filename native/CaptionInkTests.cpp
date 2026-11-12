// Caption ink policy (R16) with injected inputs, and the DWMBlurGlass config
// parser against the user's real config. "--probe" prints what this desktop
// would use, read-only.
#include "CaptionInk.h"
#include <cstdio>

using namespace header::caption;
namespace {
int checks = 0, failures = 0;
void Check(const char* name, bool pass) {
    ++checks;
    if (!pass) { ++failures; std::printf("FAIL %s\n", name); }
}
constexpr COLORREF Theme = RGB(1, 2, 3), Contrast = RGB(255, 255, 0);
// The user's DWMBlurGlass config (evidence/header-rebuild-20261104/dwmblurglass-config.ini).
const wchar_t* UserConfig = L"[config]\r\napplyglobal=false\r\nactiveTextColor=4278190080\r\ninactiveTextColor=4294967295\r\n"
                            L"activeTextColorDark=4280492835\r\ninactiveTextColorDark=4294967295\r\nactiveBlendColor=1016896668\r\n";
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--probe")) {
        HWND frame = FindWindowW(L"CabinetWClass", nullptr);
        for (bool active : {true, false}) {
            const auto in = Gather(frame, active, Theme);
            std::printf("{\"active\":%s,\"frame\":%llu,\"highContrast\":%s,\"blurGlass\":%s,\"dark\":%s,\"accentTitleBars\":%s,\"accent\":\"%06lX\",\"ink\":\"%06lX\"}\n",
                        active ? "true" : "false", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(frame)),
                        in.highContrast ? "true" : "false", in.blurGlass ? "true" : "false", in.dark ? "true" : "false",
                        in.accentTitleBars ? "true" : "false", static_cast<unsigned long>(in.accent), static_cast<unsigned long>(Ink(in)));
        }
        return 0;
    }
    const auto user = ParseBlurGlass(UserConfig);
    Check("parse-user-config", user.has_value());
    if (user) {
        Check("parse-active", user->active == 0xFF000000u && user->inactive == 0xFFFFFFFFu);
        Check("parse-dark", user->activeDark == 0xFF232323u && user->inactiveDark == 0xFFFFFFFFu);
    }
    Check("parse-missing-key", !ParseBlurGlass(L"[config]\nactiveTextColor=1\ninactiveTextColor=2\nactiveTextColorDark=3\n"));
    Check("parse-garbage", !ParseBlurGlass(L"activeTextColor=x\ninactiveTextColor=2\nactiveTextColorDark=3\ninactiveTextColorDark=4\n"));
    const auto prefixed = ParseBlurGlass(L"activeTextColorDark=3\nactiveTextColor=1\ninactiveTextColorDark=4\ninactiveTextColor=2\n");
    Check("parse-prefix-not-confused", prefixed && prefixed->active == 1);

    Inputs in;
    in.themeText = Theme;
    in.active = true;
    Check("theme-default", Ink(in) == Theme);
    in.dark = true;
    Check("dark-active-white", Ink(in) == RGB(255, 255, 255));
    in.active = false;
    Check("dark-inactive-grey", Ink(in) == RGB(170, 170, 170));
    in.dark = false;
    in.accentTitleBars = true;
    in.accent = RGB(0, 120, 215); // Windows default blue: white text
    in.active = true;
    Check("accent-dark-colour-white-text", Ink(in) == RGB(255, 255, 255));
    in.accent = RGB(255, 185, 0); // gold: black text
    Check("accent-light-colour-black-text", Ink(in) == RGB(0, 0, 0));
    in.active = false;
    Check("accent-inactive-uses-theme", Ink(in) == Theme);
    in.dark = true;
    in.active = true;
    Check("accent-wins-in-dark-mode", Ink(in) == RGB(0, 0, 0));
    in.blurGlass = user;
    Check("blurglass-dark-active", Ink(in) == RGB(0x23, 0x23, 0x23));
    in.dark = false;
    Check("blurglass-light-active", Ink(in) == RGB(0, 0, 0));
    in.active = false;
    Check("blurglass-light-inactive", Ink(in) == RGB(255, 255, 255));
    in.blurGlass = BlurGlassInk{RGB(10, 20, 30) | 0xFF000000u, 0, 0, 0};
    in.active = true;
    Check("blurglass-colorref-order", Ink(in) == RGB(10, 20, 30));
    Check("exact-blurglass", Exact(in));
    in.blurGlass.reset();
    in.dark = false;
    in.accentTitleBars = false;
    in.active = false;
    Check("theme-not-exact", !Exact(in));
    in.highContrast = true;
    in.highContrastText = Contrast;
    Check("high-contrast-first", Ink(in) == Contrast);
    Check("is-light-rule", IsLight(RGB(255, 255, 255)) && !IsLight(RGB(0, 0, 0)) && !IsLight(RGB(0, 120, 215)) && IsLight(RGB(255, 185, 0)));
    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
