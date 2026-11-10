#pragma once
// Caption text colour for tabs drawn in the title bar. The tab titles, glyphs
// and underline use the same ink as the window's own title text, chosen in
// this order (R16):
//   1. high contrast system colours;
//   2. DWMBlurGlass's own title colours when it is running (it recolours the
//      native title text, so the tabs follow it);
//   3. Windows dark title bars;
//   4. "Show accent colour on title bars" (Windows' light/dark text rule);
//   5. the visual style's caption text colour.
#include <windows.h>
#include <optional>
#include <string>

namespace header::caption {
// DWMBlurGlass data\config.ini colours. They are COLORREF values (0x00BBGGRR
// in the low bytes; the high byte is alpha) passed to DWM's title text.
struct BlurGlassInk {
    COLORREF active{}, inactive{}, activeDark{}, inactiveDark{};
};
struct Inputs {
    bool active{};
    bool highContrast{};
    COLORREF highContrastText{}; // COLOR_CAPTIONTEXT or COLOR_INACTIVECAPTIONTEXT
    std::optional<BlurGlassInk> blurGlass;
    bool dark{};                 // the frame uses a dark title bar
    bool accentTitleBars{};      // HKCU\...\DWM ColorPrevalence
    COLORREF accent{};           // the title bar colour when accentTitleBars
    COLORREF themeText{};        // the visual style's caption text colour
};
COLORREF Ink(const Inputs&);
// True when Ink() is the exact colour the window's title uses. Only the
// theme fallback is faded for inactive windows, as DWM fades it.
bool Exact(const Inputs&);
// Windows' rule for text on a coloured title bar: dark text on light colours.
bool IsLight(COLORREF);
// Reads the four colours from config.ini text; nullopt if any is missing.
std::optional<BlurGlassInk> ParseBlurGlass(const std::wstring& ini);
// Collects the inputs for `frame`. Process discovery and the config file are
// cached and re-read only when stale, so this is cheap on every paint.
Inputs Gather(HWND frame, bool active, COLORREF themeText);
}
