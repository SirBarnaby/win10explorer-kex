#pragma once
#include "HeaderLayout.h"
#include <memory>
#include <string>

namespace header {
struct TabVisual {
    std::uint64_t id{}, iconRevision{};
    std::wstring title;
    HICON icon{}; // Caller retains the icon for the duration of Paint.
};
struct VisualState {
    LayoutState layout;
    std::vector<TabVisual> tabs;
    TargetId hot, pressed, focus;
    bool active{}, focused{}, busy{}, canCreate{true};
};
// Owned by the Controller's STA. The HWND and composition target belong only to
// this header; the frame and Ribbon remain the native material owners.
class TabHeaderRenderer {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    TabHeaderRenderer();
    ~TabHeaderRenderer();
    TabHeaderRenderer(const TabHeaderRenderer&) = delete;
    TabHeaderRenderer& operator=(const TabHeaderRenderer&) = delete;
    HRESULT Paint(HWND window, const VisualState&);
    void Reset();
    void ThemeChanged();
};
}
