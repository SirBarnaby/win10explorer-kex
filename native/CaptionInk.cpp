#include "CaptionInk.h"
#include <dwmapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <vector>

namespace header::caption {
namespace {
// Windows' dark title bar text: white when active, light grey when inactive.
constexpr COLORREF DarkActive = RGB(255, 255, 255), DarkInactive = RGB(170, 170, 170);

COLORREF Rgb(COLORREF value) { return value & 0x00FFFFFF; }

std::optional<COLORREF> Value(const std::wstring& ini, const wchar_t* key) {
    // Keys are matched at the start of a line, case-insensitively, inside any
    // section (DWMBlurGlass writes a single [config] section).
    const size_t length = wcslen(key);
    size_t line = 0;
    while (line < ini.size()) {
        size_t end = ini.find_first_of(L"\r\n", line);
        if (end == std::wstring::npos) end = ini.size();
        size_t start = ini.find_first_not_of(L" \t", line);
        if (start < end && end - start > length && !_wcsnicmp(ini.c_str() + start, key, length)) {
            size_t equals = ini.find_first_not_of(L" \t", start + length);
            if (equals < end && ini[equals] == L'=') {
                const wchar_t* text = ini.c_str() + equals + 1;
                wchar_t* stop{};
                const unsigned long long parsed = wcstoull(text, &stop, 10);
                if (stop != text && parsed <= 0xFFFFFFFFull) return static_cast<COLORREF>(parsed);
            }
        }
        line = end + 1;
    }
    return std::nullopt;
}

bool DarkTitleBar(HWND frame) {
    // DWMWA_USE_IMMERSIVE_DARK_MODE is 20 (19 before 20H1). Explorer sets it
    // on its frames when apps use the dark mode.
    for (DWORD attribute : {20u, 19u}) {
        BOOL dark = FALSE;
        if (frame && SUCCEEDED(DwmGetWindowAttribute(frame, attribute, &dark, sizeof(dark)))) return dark != FALSE;
    }
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

DWORD Dword(const wchar_t* value, DWORD fallback) {
    DWORD data = fallback, size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", value, RRF_RT_REG_DWORD, nullptr, &data, &size) !=
        ERROR_SUCCESS)
        return fallback;
    return data;
}

COLORREF AccentColour() {
    // The colour DWM paints an accent title bar with (0xAARRGGBB). The
    // AccentColor value (0xAABBGGRR) is only a fallback: writing it alone does
    // not recolour title bars.
    DWORD argb{};
    BOOL opaque{};
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque))) return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    DWORD value{}, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) == ERROR_SUCCESS)
        return Rgb(value);
    return GetSysColor(COLOR_ACTIVECAPTION);
}

std::wstring ReadText(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    std::vector<char> bytes;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 1 << 20) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read{};
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) read = 0;
        bytes.resize(read);
    }
    CloseHandle(file);
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
        return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / sizeof(wchar_t));
    size_t skip = bytes.size() >= 3 && !memcmp(bytes.data(), "\xEF\xBB\xBF", 3) ? 3 : 0;
    const int count = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, static_cast<int>(bytes.size() - skip), nullptr, 0);
    std::wstring text(static_cast<size_t>(std::max(count, 0)), L'\0');
    if (count > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, static_cast<int>(bytes.size() - skip), text.data(), count);
    return text;
}

// The config of the running DWMBlurGlass, if any. Explorer has one per frame
// thread, so the cache is shared and locked; the process list is re-read
// every few seconds and the file only when its write time changes.
class BlurGlassCache {
    std::mutex lock_;
    ULONGLONG processChecked_{}, fileChecked_{};
    std::wstring config_;
    FILETIME written_{};
    std::optional<BlurGlassInk> ink_;

    static std::wstring RunningConfig() {
        std::wstring result;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return result;
        PROCESSENTRY32W entry{sizeof(entry)};
        for (BOOL more = Process32FirstW(snapshot, &entry); more && result.empty(); more = Process32NextW(snapshot, &entry)) {
            if (_wcsicmp(entry.szExeFile, L"DWMBlurGlass.exe") != 0) continue;
            // Limited query access is granted even for its elevated host.
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            wchar_t path[MAX_PATH * 2]{};
            DWORD length = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &length)) {
                std::wstring image(path, length);
                const size_t slash = image.find_last_of(L'\\');
                if (slash != std::wstring::npos) result = image.substr(0, slash) + L"\\data\\config.ini";
            }
            CloseHandle(process);
        }
        CloseHandle(snapshot);
        return result;
    }

public:
    std::optional<BlurGlassInk> Get() {
        std::lock_guard guard(lock_);
        const ULONGLONG now = GetTickCount64();
        if (!processChecked_ || now - processChecked_ >= 5000) {
            processChecked_ = now;
            auto config = RunningConfig();
            if (config != config_) {
                config_ = std::move(config);
                written_ = {};
                ink_.reset();
                fileChecked_ = 0;
            }
        }
        if (config_.empty()) return std::nullopt;
        if (!fileChecked_ || now - fileChecked_ >= 1000) {
            fileChecked_ = now;
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(config_.c_str(), GetFileExInfoStandard, &data)) {
                ink_.reset();
                written_ = {};
            } else if (CompareFileTime(&data.ftLastWriteTime, &written_) != 0) {
                written_ = data.ftLastWriteTime;
                ink_ = ParseBlurGlass(ReadText(config_));
            }
        }
        return ink_;
    }
};
BlurGlassCache blurGlass;
} // namespace

bool IsLight(COLORREF colour) {
    return 5 * GetGValue(colour) + 2 * GetRValue(colour) + GetBValue(colour) > 8 * 128;
}

COLORREF Ink(const Inputs& in) {
    if (in.highContrast) return in.highContrastText;
    if (in.blurGlass) {
        const auto& ink = *in.blurGlass;
        return Rgb(in.dark ? (in.active ? ink.activeDark : ink.inactiveDark) : (in.active ? ink.active : ink.inactive));
    }
    // An active accent title bar is drawn in the accent colour in light and dark
    // mode alike; inactive ones fall back to the mode's own caption.
    if (in.accentTitleBars && in.active) return IsLight(in.accent) ? RGB(0, 0, 0) : RGB(255, 255, 255);
    if (in.dark) return in.active ? DarkActive : DarkInactive;
    return in.themeText;
}

bool Exact(const Inputs& in) {
    return in.highContrast || in.blurGlass || (in.accentTitleBars && in.active) || in.dark;
}

std::optional<BlurGlassInk> ParseBlurGlass(const std::wstring& ini) {
    const auto active = Value(ini, L"activeTextColor"), inactive = Value(ini, L"inactiveTextColor"),
               activeDark = Value(ini, L"activeTextColorDark"), inactiveDark = Value(ini, L"inactiveTextColorDark");
    if (!active || !inactive || !activeDark || !inactiveDark) return std::nullopt;
    return BlurGlassInk{*active, *inactive, *activeDark, *inactiveDark};
}

Inputs Gather(HWND frame, bool active, COLORREF themeText) {
    Inputs in;
    in.active = active;
    HIGHCONTRASTW contrast{sizeof(contrast)};
    in.highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                      (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    in.highContrastText = GetSysColor(active ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT);
    in.themeText = themeText;
    if (in.highContrast) return in;
    in.blurGlass = blurGlass.Get();
    in.dark = DarkTitleBar(frame);
    in.accentTitleBars = Dword(L"ColorPrevalence", 0) != 0;
    if (in.accentTitleBars) in.accent = AccentColour();
    return in;
}
}
