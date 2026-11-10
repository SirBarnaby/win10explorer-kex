#pragma once
#include <windows.h>
#include <cstdint>
#include <span>
#include <vector>

namespace header {
enum class Region { Unavailable, Caption, AllocatedRow };
enum class TargetKind { None, Tab, Close, NewTab, Overflow, ScrollLeft, ScrollRight };
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
    // Strip-client coordinates. A tab partly scrolled out keeps its full
    // bounds (drawing and hit-testing clip to tabViewport); a tab entirely
    // outside the viewport has empty rectangles.
    RECT bounds{}, body{}, close{}, closeGlyph{};
    int x{};               // Left edge in content coordinates (before scrolling).
    bool showClose{true};  // Narrow tabs show their close button only when active.
    bool Visible() const;
};
// Inputs the controller owns across layouts.
struct LayoutInput {
    int scroll{};          // Content pixels scrolled out on the left.
    int lockedWidth{};     // Non-zero while closing with the mouse keeps widths.
    bool revealActive{true};
};
struct LayoutState {
    Allocation allocation;
    RECT viewport{}, newTab{}, overflow{};
    RECT tabViewport{};                 // Where tabs are drawn and hit.
    RECT scrollLeft{}, scrollRight{};   // Empty unless the row overflows.
    std::vector<TabGeometry> tabs;
    std::uint64_t active{};
    size_t firstVisible{};
    int tabWidth{}, contentWidth{}, scroll{}, maxScroll{};
    bool overflowing{}, fadeLeft{}, fadeRight{};
    TargetId Hit(POINT point) const;
    const TabGeometry* Find(std::uint64_t id) const;
    bool Usable() const;
    // Scroll offset that shows `id` completely (unchanged when it already is).
    int Reveal(std::uint64_t id) const;
};
// Firefox tab-strip metrics in DIP.
inline constexpr int TabMinimumDip = 76, TabMaximumDip = 200, TabClipDip = 140, ControlDip = 30;
// Complete native title/custom geometry and caption buttons must all be known.
bool CaptionBounds(const NativeGeometry&, const RECT& buttons, int minimumWidth, RECT& frameClient);
LayoutState BuildLayout(const Allocation&, std::span<const std::uint64_t> tabs,
                        std::uint64_t active, const LayoutInput& input = {});
// One scroll step: a wheel notch moves one tab; touchpad deltas move in
// proportion (pixel exact).
int ScrollStep(const LayoutState&, int wheelDelta);
RECT Translate(RECT, POINT origin);
}
