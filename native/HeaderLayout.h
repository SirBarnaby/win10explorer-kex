#pragma once
#include <windows.h>
#include <cstdint>
#include <span>
#include <vector>

namespace header {
enum class Region { Unavailable, Caption, AllocatedRow };
enum class TargetKind { None, Tab, Close, NewTab, Overflow };
struct TargetId {
    TargetKind kind{TargetKind::None};
    std::uint64_t tab{};
    bool operator==(const TargetId&) const = default;
};
// These rectangles are copied from the exact native paint while its borrowed
// metrics argument is alive. Native fields are window relative, in physical pixels.
struct NativeGeometry {
    RECT paint{}, title{}, custom[2]{};
    SIZE windowSize{};
    POINT clientOffset{};
    UINT dpi{};
    bool complete{};
};
struct Allocation {
    Region region{Region::Unavailable};
    RECT bounds{}; // Frame-client coordinates; the strip's local origin is (0,0).
    NativeGeometry native;
    std::uint64_t owner{}; // Distinguishes detach/re-attach even if HWND/revision repeat.
    std::uint64_t revision{};
    UINT dpi{96};
};
struct TabGeometry {
    std::uint64_t id{};
    RECT bounds{}, body{}, close{}, closeGlyph{}; // Strip-client coordinates.
    bool Visible() const;
};
struct LayoutState {
    Allocation allocation;
    RECT viewport{}, newTab{}, overflow{};
    std::vector<TabGeometry> tabs;
    std::uint64_t active{};
    size_t firstVisible{};
    TargetId Hit(POINT point) const;
    const TabGeometry* Find(std::uint64_t id) const;
    bool Usable() const;
};
// Complete native title/custom geometry and caption buttons must all be known.
bool CaptionBounds(const NativeGeometry&, const RECT& buttons, int minimumWidth, RECT& frameClient);
LayoutState BuildLayout(const Allocation&, std::span<const std::uint64_t> tabs,
                        std::uint64_t active, size_t firstVisible);
RECT Translate(RECT, POINT origin);
}
