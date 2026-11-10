#include "TabHeaderRenderer.h"
#include <algorithm>
#include <cmath>
#include <climits>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <memory>
#include <unordered_set>
#include "TabMotion.h"

using Microsoft::WRL::ComPtr;
namespace header {
namespace {
D2D1_RECT_F Box(RECT r) {
    return D2D1::RectF(static_cast<float>(r.left),static_cast<float>(r.top),
                      static_cast<float>(r.right),static_cast<float>(r.bottom));
}
D2D1_COLOR_F Color(COLORREF value,float alpha=1.f) {
    return D2D1::ColorF(GetRValue(value)/255.f,GetGValue(value)/255.f,GetBValue(value)/255.f,alpha);
}
struct Theme {
    HTHEME handle{};
    bool owned{};
    Theme(HWND window,const wchar_t* name,HTHEME borrowed=nullptr):handle(borrowed) {
        if(!handle){handle=OpenThemeData(window,name);owned=true;}
    }
    ~Theme(){if(handle&&owned)CloseThemeData(handle);}
    COLORREF Get(int part,int state,int property,COLORREF fallback) const {
        COLORREF value{};
        return handle&&SUCCEEDED(GetThemeColor(handle,part,state,property,&value))?value:fallback;
    }
};
HTHEME NativeControlTheme(HWND owned,const wchar_t* wanted) {
    struct Search{const wchar_t* name;HTHEME theme{};} search{wanted};
    EnumChildWindows(GetAncestor(owned,GA_ROOT),[](HWND child,LPARAM value)->BOOL {
        auto& found=*reinterpret_cast<Search*>(value);wchar_t name[128]{};
        GetClassNameW(child,name,128);
        if(!wcscmp(name,found.name)&&IsWindowVisible(child)) {
            found.theme=GetWindowTheme(child);if(found.theme)return FALSE;
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    return search.theme;
}
struct Edge {
    LONG left{}, top{}, right{}, bottom{};
    bool operator==(const Edge&) const = default;
};
Edge EdgeOf(const RECT& r) { return {r.left, r.top, r.right, r.bottom}; }
// Every colour a surface uses, so a theme or caption-colour change repaints it.
struct Inks {
    COLORREF text{}, disabled{}, accent{}, selectedText{}, selectedFill{}, hotFill{}, highlightText{}, background{};
    bool contrast{}, caption{}, active{};
    bool operator==(const Inks&) const = default;
};
enum class Part : unsigned char { None, Body, Close };
Part PartOf(const TargetId& target, std::uint64_t id) {
    if (target.tab != id) return Part::None;
    if (target.kind == TargetKind::Tab) return Part::Body;
    return target.kind == TargetKind::Close ? Part::Close : Part::None;
}
TargetId ControlOf(const TargetId& target) {
    const bool control = target.kind == TargetKind::Overflow || target.kind == TargetKind::ScrollLeft ||
                         target.kind == TargetKind::ScrollRight;
    return control ? target : TargetId{};
}
// What one tab surface shows; equal keys mean equal pixels.
struct TabKey {
    std::wstring title;
    HICON icon{};
    std::uint64_t iconRevision{};
    int width{}, height{}, fadeLeft{INT_MIN}, fadeRight{INT_MIN}, fadeSpan{};
    UINT dpi{};
    bool showClose{}, selected{}, focused{}, busy{};
    Part hot{}, pressed{}, focus{};
    Inks inks;
    bool operator==(const TabKey&) const = default;
};
struct ControlsKey {
    int width{}, height{};
    UINT dpi{};
    Edge overflow, scrollLeft, scrollRight;
    bool fadeLeft{}, fadeRight{}, focused{};
    TargetId hot, pressed, focus;
    Inks inks;
    bool operator==(const ControlsKey&) const = default;
};
struct NewTabKey {
    int width{}, height{};
    UINT dpi{};
    bool enabled{}, hot{}, pressed{}, focus{};
    Inks inks;
    bool operator==(const NewTabKey&) const = default;
};
constexpr motion::Timing Instant{0, motion::EaseOut};
}
// The strip is a small DirectComposition tree:
//   root -> controls (row background, overflow chevron, scroll arrows)
//        -> viewport (clipped to the tab area) -> scroller (offset = -scroll)
//              -> tabs layer -> one visual and surface per realised tab
//              -> drag layer (the tab under the pointer while reordering)
//        -> new-tab button
// Each surface repaints only when what it shows changes. Movement, growth and
// fades are compositor animations, so they stay smooth while Explorer's UI
// thread is busy creating a folder view.
struct TabHeaderRenderer::Impl {
    struct Layer {
        ComPtr<IDCompositionVisual2> visual;
        ComPtr<IDCompositionSurface> surface;
        int width{}, height{};
        bool painted{};
    };
    struct Slot : Layer {
        std::uint64_t id{};
        ComPtr<IDCompositionRectangleClip> clip;
        ComPtr<IDCompositionEffectGroup> effect;
        TabKey key;
        motion::Transition x, right;
        std::uint64_t ghostEnd{};
        bool ghost{}, dragged{}, seen{};
    };
    struct Picture {
        std::uint64_t id{}, revision{};
        HICON handle{};
        ComPtr<ID2D1Bitmap> bitmap;
    };
    ComPtr<ID2D1Factory1> drawing;
    ComPtr<IDWriteFactory> writing;
    ComPtr<IWICImagingFactory> imaging;
    ComPtr<ID3D11Device> graphics;
    ComPtr<ID2D1Device> drawingDevice;
    ComPtr<IDCompositionDesktopDevice> composition;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDWriteTextFormat> font;
    ComPtr<IDWriteInlineObject> ellipsis;
    // Device resources: created once, shared by every surface of the device.
    ComPtr<ID2D1SolidColorBrush> brush;
    std::vector<Picture> pictures;
    ComPtr<IDCompositionVisual2> root, viewport, scroller, tabsLayer, dragLayer;
    ComPtr<IDCompositionRectangleClip> viewportClip;
    Layer controls, newTab;
    ControlsKey controlsKey;
    NewTabKey newTabKey;
    motion::Transition scroll, newTabX;
    Edge placedViewport{-1, -1, -1, -1};
    std::vector<std::unique_ptr<Slot>> slots;
    std::vector<std::uint64_t> previousIds;
    int previousWidth{}, previousStripWidth{}, previousStripHeight{};
    UINT previousDpi{};
    HWND window{};
    UINT dpi{};
    bool firstSurface{true}, built{}, hasPainted{};

    HRESULT Initialize(HWND owned) {
        if (composition) return window == owned ? S_OK : E_INVALIDARG;
        HRESULT hr = S_OK;
        if (!drawing && FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                                      reinterpret_cast<void**>(drawing.GetAddressOf()))))
            return hr;
        if (!writing && FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                        reinterpret_cast<IUnknown**>(writing.GetAddressOf()))))
            return hr;
        if (!imaging && FAILED(hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging))))
            return hr;
        ComPtr<ID3D11Device> device;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Device> d2d;
        ComPtr<IDCompositionDesktopDevice> desktop;
        ComPtr<IDCompositionTarget> owner;
        if (FAILED(hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                          D3D11_SDK_VERSION, &device, nullptr, nullptr)) ||
            FAILED(hr = device.As(&dxgi)) || FAILED(hr = drawing->CreateDevice(dxgi.Get(), &d2d)) ||
            FAILED(hr = DCompositionCreateDevice2(d2d.Get(), IID_PPV_ARGS(&desktop))) ||
            FAILED(hr = desktop->CreateTargetForHwnd(owned, TRUE, &owner)))
            return hr;
        graphics = device;
        drawingDevice = d2d;
        composition = desktop;
        target = owner;
        window = owned;
        return S_OK;
    }
    HRESULT Font(UINT scale) {
        if (font && dpi == scale) return S_OK;
        font.Reset();
        ellipsis.Reset();
        dpi = scale;
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, scale))
            return HRESULT_FROM_WIN32(GetLastError());
        const auto& lf = metrics.lfCaptionFont;
        ComPtr<IDWriteGdiInterop> interop;
        ComPtr<IDWriteFont> nativeFont;
        HRESULT hr = writing->GetGdiInterop(&interop);
        if (FAILED(hr) || FAILED(hr = interop->CreateFontFromLOGFONT(&lf, &nativeFont))) return hr;
        const float size = static_cast<float>(std::abs(lf.lfHeight));
        if (size == 0) return E_INVALIDARG;
        if (FAILED(hr = writing->CreateTextFormat(lf.lfFaceName, nullptr, nativeFont->GetWeight(), nativeFont->GetStyle(),
                                                  nativeFont->GetStretch(), size, L"", &font)))
            return hr;
        if (FAILED(hr = font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)) ||
            FAILED(hr = font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) ||
            FAILED(hr = writing->CreateEllipsisTrimmingSign(font.Get(), &ellipsis)))
            return hr;
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        return font->SetTrimming(&trim, ellipsis.Get());
    }
    HRESULT Build() {
        if (built) return S_OK;
        HRESULT hr = S_OK;
        if (FAILED(hr = composition->CreateVisual(&root)) || FAILED(hr = composition->CreateVisual(&controls.visual)) ||
            FAILED(hr = composition->CreateVisual(&viewport)) || FAILED(hr = composition->CreateVisual(&scroller)) ||
            FAILED(hr = composition->CreateVisual(&tabsLayer)) || FAILED(hr = composition->CreateVisual(&dragLayer)) ||
            FAILED(hr = composition->CreateVisual(&newTab.visual)) || FAILED(hr = composition->CreateRectangleClip(&viewportClip)))
            return hr;
        // Explicit references keep the stacking unambiguous: controls, tabs,
        // then the dragged tab and the new-tab button above them.
        if (FAILED(hr = root->AddVisual(controls.visual.Get(), FALSE, nullptr)) ||
            FAILED(hr = root->AddVisual(viewport.Get(), TRUE, controls.visual.Get())) ||
            FAILED(hr = root->AddVisual(newTab.visual.Get(), TRUE, viewport.Get())) ||
            FAILED(hr = viewport->AddVisual(scroller.Get(), FALSE, nullptr)) ||
            FAILED(hr = scroller->AddVisual(tabsLayer.Get(), FALSE, nullptr)) ||
            FAILED(hr = scroller->AddVisual(dragLayer.Get(), TRUE, tabsLayer.Get())) ||
            FAILED(hr = viewport->SetClip(viewportClip.Get())) || FAILED(hr = target->SetRoot(root.Get())))
            return hr;
        built = true;
        return S_OK;
    }
    // Starts `value` towards `to` from what is on screen now.
    template <class Set, class Animate>
    HRESULT Move(motion::Transition& value, double to, motion::Timing timing, std::uint64_t now, Set set, Animate animate) {
        const double from = value.At(now);
        value = {from, to, now, timing.milliseconds, timing.curve};
        if (timing.milliseconds <= 0 || std::abs(from - to) < 1e-3) {
            value = {to, to, now, 0, timing.curve};
            return set(static_cast<float>(to));
        }
        ComPtr<IDCompositionAnimation> animation;
        HRESULT hr = composition->CreateAnimation(&animation);
        if (FAILED(hr)) return hr;
        const double seconds = timing.milliseconds / 1000.0;
        for (const auto& s : motion::Segments(timing.curve, from, to, seconds))
            if (FAILED(hr = animation->AddCubic(s.begin, static_cast<float>(s.a), static_cast<float>(s.b),
                                                static_cast<float>(s.c), static_cast<float>(s.d))))
                return hr;
        if (FAILED(hr = animation->End(seconds, static_cast<float>(to)))) return hr;
        return animate(animation.Get());
    }
    HRESULT Prepare(Layer& layer, int width, int height) {
        if (layer.surface && layer.width == width && layer.height == height) return S_OK;
        layer.surface.Reset();
        layer.painted = false;
        if (width <= 0 || height <= 0) {
            layer.width = layer.height = 0;
            return layer.visual->SetContent(nullptr);
        }
        HRESULT hr = composition->CreateSurface(static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_B8G8R8A8_UNORM,
                                                DXGI_ALPHA_MODE_PREMULTIPLIED, &layer.surface);
        if (FAILED(hr)) return hr;
        layer.width = width;
        layer.height = height;
        return layer.visual->SetContent(layer.surface.Get());
    }
    template <class Painter>
    HRESULT DrawSurface(Layer& layer, Painter painter) {
        if (!layer.surface) return S_OK;
        ComPtr<ID2D1DeviceContext> dc;
        POINT offset{};
        HRESULT hr = layer.surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(dc.GetAddressOf()), &offset);
        if (FAILED(hr)) return hr;
        dc->SetDpi(96.f, 96.f);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        // The surface may be part of a shared atlas: stay inside our own rectangle.
        dc->PushAxisAlignedClip(D2D1::RectF(0, 0, static_cast<float>(layer.width), static_cast<float>(layer.height)),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (!brush) hr = dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush);
        if (SUCCEEDED(hr)) {
            try { hr = painter(dc.Get()); }
            catch (...) { hr = E_OUTOFMEMORY; }
        }
        dc->PopAxisAlignedClip();
        // Even icon/font/draw failures must release an acquired draw.
        const HRESULT ended = layer.surface->EndDraw();
        if (FAILED(hr)) return hr;
        if (FAILED(ended)) return ended;
        layer.painted = true;
        return S_OK;
    }
    Inks MakeInks(const VisualState& state) const {
        HIGHCONTRASTW hc{sizeof(hc)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
        Inks inks;
        inks.contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
        inks.caption = state.layout.allocation.region == Region::Caption;
        inks.active = state.active;
        // Open theme data only on our HWND. Existing native control handles are
        // borrowed for this paint and must never be closed or reassociated.
        Theme native(window, inks.caption ? L"WINDOW" : L"TOOLBAR", inks.caption ? nullptr : NativeControlTheme(window, L"ToolbarWindow32"));
        const int part = inks.caption ? WP_CAPTION : TP_BUTTON;
        const int normal = inks.caption ? (state.active ? CS_ACTIVE : CS_INACTIVE) : TS_NORMAL;
        const COLORREF nativeText =
            GetSysColor(inks.caption ? (state.active ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT) : COLOR_BTNTEXT);
        inks.text = inks.contrast ? nativeText : native.Get(part, normal, TMT_TEXTCOLOR, nativeText);
        inks.disabled = GetSysColor(COLOR_GRAYTEXT);
        inks.accent = GetSysColor(COLOR_HIGHLIGHT);
        inks.highlightText = GetSysColor(COLOR_HIGHLIGHTTEXT);
        inks.selectedText = inks.contrast ? inks.highlightText : inks.text;
        inks.selectedFill = inks.contrast ? inks.accent : GetSysColor(COLOR_WINDOW);
        inks.hotFill = inks.contrast ? inks.accent : GetSysColor(COLOR_HIGHLIGHT);
        // The native caption stays transparent in every provider/theme mode. A
        // reserved client row uses its native rebar theme, including classic
        // and high-contrast system colours, rather than extending glass downward.
        if (!inks.caption) {
            Theme rebar(window, L"REBAR", NativeControlTheme(window, L"ReBarWindow32"));
            inks.background = inks.contrast ? GetSysColor(COLOR_BTNFACE)
                                            : rebar.Get(RP_BACKGROUND, 0, TMT_FILLCOLOR, GetSysColor(COLOR_BTNFACE));
        }
        return inks;
    }
    HRESULT PictureOf(ID2D1DeviceContext* dc, const TabVisual& tab, ID2D1Bitmap** result) {
        *result = nullptr;
        if (!tab.icon) return S_FALSE;
        for (auto& picture : pictures)
            if (picture.id == tab.id && picture.revision == tab.iconRevision && picture.handle == tab.icon)
                return picture.bitmap.CopyTo(result);
        ComPtr<IWICBitmap> source;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<ID2D1Bitmap> bitmap;
        HRESULT hr = imaging->CreateBitmapFromHICON(tab.icon, &source);
        if (FAILED(hr) || FAILED(hr = imaging->CreateFormatConverter(&converter)) ||
            FAILED(hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                              WICBitmapPaletteTypeCustom)) ||
            FAILED(hr = dc->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap)))
            return hr;
        pictures.push_back({tab.id, tab.iconRevision, tab.icon, bitmap});
        return bitmap.CopyTo(result);
    }
    void Fill(ID2D1DeviceContext* dc, RECT r, COLORREF color, float opacity) {
        if (IsRectEmpty(&r)) return;
        brush->SetColor(Color(color, opacity));
        dc->FillRectangle(Box(r), brush.Get());
    }
    void Line(ID2D1DeviceContext* dc, float x1, float y1, float x2, float y2, COLORREF color, float opacity, float thickness) {
        brush->SetColor(Color(color, opacity));
        dc->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush.Get(), thickness);
    }
    // Hover and press feedback shared by tabs and controls.
    void Affordance(ID2D1DeviceContext* dc, RECT bounds, bool hot, bool pressed, const Inks& inks) {
        if (pressed && hot) Fill(dc, bounds, inks.hotFill, inks.contrast ? 1.f : .30f);
        else if (hot) Fill(dc, bounds, inks.hotFill, inks.contrast ? 1.f : .16f);
    }
    // Firefox fades the tab strip's scrolled edges; each edge tab carries its
    // share of that mask in its own coordinates.
    HRESULT PushFade(ID2D1DeviceContext* dc, const TabKey& key, bool& pushed) {
        pushed = false;
        const bool left = key.fadeLeft != INT_MIN, right = key.fadeRight != INT_MIN;
        if (!left && !right) return S_OK;
        const float span = static_cast<float>(std::max(1, key.fadeSpan));
        const float from = left ? static_cast<float>(key.fadeLeft) : -2 * span;
        const float to = right ? static_cast<float>(key.fadeRight) : static_cast<float>(key.width) + 2 * span;
        const float extent = std::max(1.f, to - from), edge = std::min(.5f, span / extent);
        const D2D1_GRADIENT_STOP stops[]{{0.f, D2D1::ColorF(0, 0, 0, left ? 0.f : 1.f)},
                                         {edge, D2D1::ColorF(0, 0, 0, 1.f)},
                                         {1.f - edge, D2D1::ColorF(0, 0, 0, 1.f)},
                                         {1.f, D2D1::ColorF(0, 0, 0, right ? 0.f : 1.f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> mask;
        HRESULT hr = dc->CreateGradientStopCollection(stops, 4, &collection);
        if (FAILED(hr) || FAILED(hr = dc->CreateLinearGradientBrush(
                              D2D1::LinearGradientBrushProperties(D2D1::Point2F(from, 0), D2D1::Point2F(to, 0)), collection.Get(), &mask)))
            return hr;
        dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(0, 0, static_cast<float>(key.width), static_cast<float>(key.height)), nullptr,
                                             D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), 1.f, mask.Get()),
                      nullptr);
        pushed = true;
        return S_OK;
    }
    HRESULT DrawTab(ID2D1DeviceContext* dc, const TabVisual& tab, const TabKey& key) {
        const Inks& inks = key.inks;
        auto px = [&](int n) { return MulDiv(n, static_cast<int>(key.dpi), 96); };
        const float stroke = static_cast<float>(std::max(1, px(1)));
        const auto geometry = LocalTab(key.width, key.height, key.dpi, key.showClose);
        bool faded = false;
        HRESULT hr = PushFade(dc, key, faded);
        if (FAILED(hr)) return hr;
        const RECT bounds = geometry.bounds;
        if (key.selected) Fill(dc, bounds, inks.selectedFill, inks.contrast ? 1.f : (inks.active ? .26f : .16f));
        // A body hover includes its close button; the independent button gets
        // an additional affordance over the shared close bounds.
        if (key.hot != Part::None) Fill(dc, bounds, inks.hotFill, inks.contrast ? 1.f : .10f);
        Affordance(dc, geometry.body, key.hot == Part::Body, key.pressed == Part::Body, inks);
        Affordance(dc, geometry.close, key.hot == Part::Close, key.pressed == Part::Close, inks);
        const bool highlighted = inks.contrast && (key.selected || key.hot != Part::None);
        const COLORREF ink = highlighted ? inks.selectedText : inks.text;
        const float opacity = inks.active ? 1.f : .82f;
        if (key.selected)
            Line(dc, static_cast<float>(bounds.left), bounds.bottom - stroke / 2, static_cast<float>(bounds.right),
                 bounds.bottom - stroke / 2, inks.contrast ? inks.selectedText : inks.accent, inks.active ? 1.f : .65f,
                 std::max(1.f, static_cast<float>(px(2))));
        const int pad = px(8), icon = px(16);
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(hr = PictureOf(dc, tab, &bitmap))) {
            if (faded) dc->PopLayer();
            return hr;
        }
        if (bitmap) {
            const float y = (bounds.top + bounds.bottom - icon) / 2.f;
            dc->DrawBitmap(bitmap.Get(), D2D1::RectF(static_cast<float>(bounds.left + pad), y, static_cast<float>(bounds.left + pad + icon), y + icon),
                           opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        auto label = Box(geometry.body);
        label.left += pad + icon + pad;
        label.right -= px(4);
        brush->SetColor(Color(ink, opacity));
        if (label.right > label.left)
            dc->DrawTextW(tab.title.data(), static_cast<UINT>(tab.title.size()), font.Get(), label, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        // Narrow tabs keep the close button on the selected tab only.
        if (geometry.showClose && !IsRectEmpty(&geometry.closeGlyph)) {
            const auto glyph = geometry.closeGlyph;
            Line(dc, glyph.left + .5f, glyph.top + .5f, glyph.right - .5f, glyph.bottom - .5f, ink, opacity, stroke);
            Line(dc, glyph.right - .5f, glyph.top + .5f, glyph.left + .5f, glyph.bottom - .5f, ink, opacity, stroke);
        }
        if (key.focused && key.focus != Part::None) {
            RECT focus = key.focus == Part::Body ? geometry.body : geometry.close;
            InflateRect(&focus, -px(3), -px(3));
            brush->SetColor(Color(ink, .8f));
            dc->DrawRectangle(Box(focus), brush.Get(), stroke);
        }
        if (key.busy) {
            // A slow tab shows a clock on its icon; the rest of the strip stays usable.
            const float x = bounds.left + pad + icon / 2.f, y = (bounds.top + bounds.bottom) / 2.f, r = px(6) * 1.f;
            brush->SetColor(Color(inks.text, .8f));
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), brush.Get(), stroke);
            Line(dc, x, y, x, y - r * .6f, inks.text, 1.f, stroke);
            Line(dc, x, y, x + r * .5f, y, inks.text, 1.f, stroke);
        }
        if (faded) dc->PopLayer();
        return S_OK;
    }
    HRESULT DrawControls(ID2D1DeviceContext* dc, const VisualState& state, const Inks& inks) {
        const auto& layout = state.layout;
        auto px = [&](int n) { return MulDiv(n, static_cast<int>(layout.allocation.dpi), 96); };
        const float stroke = static_cast<float>(std::max(1, px(1))), r = static_cast<float>(px(4));
        if (!inks.caption) Fill(dc, layout.viewport, inks.background, 1.f);
        auto ink = [&](TargetKind kind, bool enabled) {
            if (!enabled) return inks.disabled;
            return inks.contrast && state.hot.kind == kind ? inks.highlightText : inks.text;
        };
        auto feedback = [&](TargetKind kind, RECT bounds, bool enabled) {
            if (enabled) Affordance(dc, bounds, state.hot.kind == kind, state.pressed.kind == kind && state.hot.kind == kind, inks);
        };
        const RECT overflow = layout.overflow;
        feedback(TargetKind::Overflow, overflow, true);
        const float ox = (overflow.left + overflow.right) / 2.f, oy = (overflow.top + overflow.bottom) / 2.f;
        Line(dc, ox - r, oy - r / 2, ox, oy + r / 2, ink(TargetKind::Overflow, true), 1.f, stroke);
        Line(dc, ox, oy + r / 2, ox + r, oy - r / 2, ink(TargetKind::Overflow, true), 1.f, stroke);
        // Scroll arrows: chevrons that grey out at either end.
        auto arrow = [&](TargetKind kind, RECT bounds, bool enabled, float direction) {
            if (IsRectEmpty(&bounds)) return;
            feedback(kind, bounds, enabled);
            const float ax = (bounds.left + bounds.right) / 2.f, ay = (bounds.top + bounds.bottom) / 2.f, half = r / 2.f;
            Line(dc, ax + half * direction, ay - r, ax - half * direction, ay, ink(kind, enabled), 1.f, stroke);
            Line(dc, ax - half * direction, ay, ax + half * direction, ay + r, ink(kind, enabled), 1.f, stroke);
        };
        arrow(TargetKind::ScrollLeft, layout.scrollLeft, layout.fadeLeft, 1.f);
        arrow(TargetKind::ScrollRight, layout.scrollRight, layout.fadeRight, -1.f);
        if (state.focused && state.focus.kind == TargetKind::Overflow) {
            RECT focus = overflow;
            InflateRect(&focus, -px(3), -px(3));
            brush->SetColor(Color(inks.text, .8f));
            dc->DrawRectangle(Box(focus), brush.Get(), stroke);
        }
        return S_OK;
    }
    HRESULT DrawNewTab(ID2D1DeviceContext* dc, const NewTabKey& key) {
        const Inks& inks = key.inks;
        auto px = [&](int n) { return MulDiv(n, static_cast<int>(key.dpi), 96); };
        const float stroke = static_cast<float>(std::max(1, px(1))), r = static_cast<float>(px(4));
        const RECT bounds{0, 0, key.width, key.height};
        if (key.enabled) Affordance(dc, bounds, key.hot, key.pressed, inks);
        const COLORREF ink = !key.enabled ? inks.disabled : inks.contrast && key.hot ? inks.highlightText : inks.text;
        const float cx = key.width / 2.f, cy = key.height / 2.f;
        Line(dc, cx - r, cy, cx + r, cy, ink, 1.f, stroke);
        Line(dc, cx, cy - r, cx, cy + r, ink, 1.f, stroke);
        if (key.focus) {
            RECT focus = bounds;
            InflateRect(&focus, -px(3), -px(3));
            brush->SetColor(Color(inks.text, .8f));
            dc->DrawRectangle(Box(focus), brush.Get(), stroke);
        }
        return S_OK;
    }
    Slot* FindSlot(std::uint64_t id) {
        for (auto& slot : slots)
            if (slot->id == id && !slot->ghost) return slot.get();
        return nullptr;
    }
    HRESULT CreateSlot(std::uint64_t id, Slot*& created) {
        auto slot = std::make_unique<Slot>();
        slot->id = id;
        HRESULT hr = S_OK;
        if (FAILED(hr = composition->CreateVisual(&slot->visual)) || FAILED(hr = composition->CreateRectangleClip(&slot->clip)) ||
            FAILED(hr = slot->clip->SetLeft(0.f)) || FAILED(hr = slot->clip->SetTop(0.f)) ||
            FAILED(hr = slot->visual->SetClip(slot->clip.Get())) || FAILED(hr = tabsLayer->AddVisual(slot->visual.Get(), FALSE, nullptr)))
            return hr;
        created = slot.get();
        slots.push_back(std::move(slot));
        return S_OK;
    }
    TabKey KeyFor(const VisualState& state, const TabGeometry& geometry, const TabVisual& tab, const Inks& inks, int fade) const {
        const auto& layout = state.layout;
        const int width = layout.tabWidth, visible = layout.tabViewport.right - layout.tabViewport.left;
        const int viewLeft = layout.scroll, viewRight = layout.scroll + visible;
        TabKey key;
        key.title = tab.title;
        key.icon = tab.icon;
        key.iconRevision = tab.iconRevision;
        key.width = width;
        key.height = layout.viewport.bottom;
        key.dpi = layout.allocation.dpi;
        key.showClose = geometry.showClose;
        key.selected = tab.id == layout.active;
        key.focused = state.focused;
        key.busy = state.busyTab && tab.id == state.busyTab;
        key.hot = PartOf(state.hot, tab.id);
        key.pressed = state.pressed == state.hot ? PartOf(state.pressed, tab.id) : Part::None;
        key.focus = PartOf(state.focus, tab.id);
        key.inks = inks;
        if (tab.id != state.dragTab) {
            key.fadeSpan = fade;
            if (layout.fadeLeft && geometry.x < viewLeft + fade && geometry.x + width > viewLeft) key.fadeLeft = viewLeft - geometry.x;
            if (layout.fadeRight && geometry.x + width > viewRight - fade && geometry.x < viewRight) key.fadeRight = viewRight - geometry.x;
        }
        return key;
    }
    HRESULT UpdateTab(Slot& slot, const VisualState& state, const TabGeometry& geometry, const TabVisual& tab, const Inks& inks,
                      motion::Timing timing, bool fresh, bool opening, std::uint64_t now, int fade, bool& changed) {
        const auto& layout = state.layout;
        const int width = layout.tabWidth, height = layout.viewport.bottom;
        const bool dragged = tab.id == state.dragTab;
        HRESULT hr = S_OK;
        if (dragged != slot.dragged) {
            if (FAILED(hr = (slot.dragged ? dragLayer : tabsLayer)->RemoveVisual(slot.visual.Get())) ||
                FAILED(hr = (dragged ? dragLayer : tabsLayer)->AddVisual(slot.visual.Get(), FALSE, nullptr)))
                return hr;
            slot.dragged = dragged;
            changed = true;
        }
        // The dragged tab follows the pointer exactly; everything else glides.
        const double x = dragged ? state.dragX : geometry.x;
        if (fresh || slot.x.to != x) {
            if (FAILED(hr = Move(slot.x, x, fresh || dragged ? Instant : timing, now, [&](float v) { return slot.visual->SetOffsetX(v); },
                                 [&](IDCompositionAnimation* a) { return slot.visual->SetOffsetX(a); })))
                return hr;
            changed = true;
        }
        auto setRight = [&](float v) { return slot.clip->SetRight(v); };
        auto animateRight = [&](IDCompositionAnimation* a) { return slot.clip->SetRight(a); };
        if (fresh && opening) {
            // A new tab grows from nothing while its content fades in.
            constexpr motion::Timing open{motion::OpenMs, motion::EaseOut};
            if (!slot.effect && FAILED(hr = composition->CreateEffectGroup(&slot.effect))) return hr;
            if (FAILED(hr = slot.visual->SetEffect(slot.effect.Get()))) return hr;
            motion::Transition opacity{};
            slot.right = {};
            if (FAILED(hr = Move(slot.right, width, open, now, setRight, animateRight)) ||
                FAILED(hr = Move(opacity, 1.0, open, now, [&](float v) { return slot.effect->SetOpacity(v); },
                                 [&](IDCompositionAnimation* a) { return slot.effect->SetOpacity(a); })))
                return hr;
        } else if (fresh || slot.right.to != width) {
            if (FAILED(hr = Move(slot.right, width, fresh ? Instant : timing, now, setRight, animateRight))) return hr;
            changed = true;
        }
        if (slot.height != height && FAILED(hr = slot.clip->SetBottom(static_cast<float>(height)))) return hr;
        if (FAILED(hr = Prepare(slot, width, height))) return hr;
        TabKey key = KeyFor(state, geometry, tab, inks, fade);
        if (!slot.painted || !(key == slot.key)) {
            if (FAILED(hr = DrawSurface(slot, [&](ID2D1DeviceContext* dc) { return DrawTab(dc, tab, key); }))) return hr;
            slot.key = std::move(key);
            changed = true;
        }
        return S_OK;
    }
    HRESULT UpdateTabs(const VisualState& state, const Inks& inks, std::vector<std::uint64_t> ids, motion::Change change,
                       motion::Timing timing, bool animate, std::uint64_t now, bool& changed) {
        const auto& layout = state.layout;
        std::unordered_set<std::uint64_t> before;
        if (animate && change == motion::Change::Membership) before.insert(previousIds.begin(), previousIds.end());
        const int visible = layout.tabViewport.right - layout.tabViewport.left;
        const int fade = std::min(visible / 2, MulDiv(24, static_cast<int>(layout.allocation.dpi), 96));
        // Realise what is visible plus what a running scroll passes through.
        const double shown = -scroll.At(now), wanted = layout.scroll;
        const double low = std::min(shown, wanted), high = std::max(shown, wanted) + visible;
        for (auto& slot : slots) slot->seen = false;
        HRESULT hr = S_OK;
        for (size_t i = 0; i < layout.tabs.size(); ++i) {
            const auto& geometry = layout.tabs[i];
            const TabVisual* tab = i < state.tabs.size() && state.tabs[i].id == geometry.id ? &state.tabs[i] : nullptr;
            if (!tab)
                for (const auto& candidate : state.tabs)
                    if (candidate.id == geometry.id) tab = &candidate;
            if (!tab) continue;
            Slot* slot = FindSlot(geometry.id);
            const double left = geometry.x, right = left + layout.tabWidth;
            bool realise = (right > low && left < high) || geometry.id == state.dragTab;
            if (!realise && slot && slot->x.Running(now)) {
                const double at = slot->x.At(now);
                realise = at + layout.tabWidth > low && at < high;
            }
            if (!realise) continue;
            const bool fresh = !slot;
            if (fresh && FAILED(hr = CreateSlot(geometry.id, slot))) return hr;
            slot->seen = true;
            const bool opening = fresh && !before.empty() && !before.contains(geometry.id);
            if (FAILED(hr = UpdateTab(*slot, state, geometry, *tab, inks, timing, fresh, opening, now, fade, changed))) return hr;
        }
        for (auto it = slots.begin(); it != slots.end();) {
            Slot& slot = **it;
            if (slot.seen) { ++it; continue; }
            const bool removed = std::find(ids.begin(), ids.end(), slot.id) == ids.end();
            if (removed && !slot.ghost && animate && change == motion::Change::Membership) {
                // A closed tab shrinks where it was while its neighbours slide in.
                slot.ghost = true;
                slot.ghostEnd = now + motion::CloseMs;
                if (FAILED(hr = Move(slot.right, 0, {motion::CloseMs, motion::EaseOut}, now,
                                     [&](float v) { return slot.clip->SetRight(v); },
                                     [&](IDCompositionAnimation* a) { return slot.clip->SetRight(a); })))
                    return hr;
                changed = true;
                ++it;
                continue;
            }
            if (slot.ghost && now < slot.ghostEnd) { ++it; continue; }
            if (FAILED(hr = (slot.dragged ? dragLayer : tabsLayer)->RemoveVisual(slot.visual.Get()))) return hr;
            it = slots.erase(it);
            changed = true;
        }
        previousIds = std::move(ids);
        previousWidth = layout.tabWidth;
        return S_OK;
    }
    HRESULT Paint(HWND owned, const VisualState& state) {
        HRESULT hr = Initialize(owned);
        if (FAILED(hr) || FAILED(hr = Font(state.layout.allocation.dpi)) || FAILED(hr = Build())) return hr;
        const auto& layout = state.layout;
        const int stripWidth = layout.viewport.right, stripHeight = layout.viewport.bottom;
        if (stripWidth <= 0 || stripHeight <= 0) return E_INVALIDARG;
        const UINT scale = layout.allocation.dpi;
        const std::uint64_t now = GetTickCount64();
        // Motion follows the user's own changes; a resized or re-scaled strip is
        // laid out at once.
        const bool animate = state.animate && hasPainted && stripWidth == previousStripWidth &&
                             stripHeight == previousStripHeight && scale == previousDpi;
        const Inks inks = MakeInks(state);
        bool changed = !hasPainted;
        const ControlsKey controlsWanted{stripWidth, stripHeight, scale, EdgeOf(layout.overflow), EdgeOf(layout.scrollLeft),
                                         EdgeOf(layout.scrollRight), layout.fadeLeft, layout.fadeRight, state.focused,
                                         ControlOf(state.hot), ControlOf(state.pressed), ControlOf(state.focus), inks};
        if (FAILED(hr = Prepare(controls, stripWidth, stripHeight))) return hr;
        if (!controls.painted || !(controlsWanted == controlsKey)) {
            if (FAILED(hr = DrawSurface(controls, [&](ID2D1DeviceContext* dc) { return DrawControls(dc, state, inks); }))) return hr;
            controlsKey = controlsWanted;
            changed = true;
        }
        const RECT area = layout.tabViewport;
        if (!(EdgeOf(area) == placedViewport)) {
            if (FAILED(hr = viewport->SetOffsetX(static_cast<float>(area.left))) ||
                FAILED(hr = viewport->SetOffsetY(static_cast<float>(area.top))) || FAILED(hr = viewportClip->SetLeft(0.f)) ||
                FAILED(hr = viewportClip->SetTop(0.f)) || FAILED(hr = viewportClip->SetRight(static_cast<float>(area.right - area.left))) ||
                FAILED(hr = viewportClip->SetBottom(static_cast<float>(area.bottom - area.top))))
                return hr;
            placedViewport = EdgeOf(area);
            changed = true;
        }
        if (!hasPainted || scroll.to != -layout.scroll) {
            // Scrolling and scroll-into-view glide (Firefox: 150 ms ease-out).
            const motion::Timing glide = animate ? motion::Timing{motion::SettleMs, motion::EaseOut} : Instant;
            if (FAILED(hr = Move(scroll, -static_cast<double>(layout.scroll), glide, now, [&](float v) { return scroller->SetOffsetX(v); },
                                 [&](IDCompositionAnimation* a) { return scroller->SetOffsetX(a); })))
                return hr;
            changed = true;
        }
        std::vector<std::uint64_t> ids;
        ids.reserve(layout.tabs.size());
        for (const auto& tab : layout.tabs) ids.push_back(tab.id);
        const auto change = motion::Classify(previousIds, ids, previousWidth, layout.tabWidth);
        const motion::Timing movement = animate ? motion::TimingFor(change) : Instant;
        if (FAILED(hr = UpdateTabs(state, inks, std::move(ids), change, movement, animate, now, changed))) return hr;
        const RECT plus = layout.newTab;
        const NewTabKey newTabWanted{plus.right - plus.left, plus.bottom - plus.top, scale, state.canCreate,
                                     state.hot.kind == TargetKind::NewTab,
                                     state.pressed.kind == TargetKind::NewTab && state.hot.kind == TargetKind::NewTab,
                                     state.focused && state.focus.kind == TargetKind::NewTab, inks};
        if (FAILED(hr = Prepare(newTab, newTabWanted.width, newTabWanted.height))) return hr;
        if (!newTab.painted || !(newTabWanted == newTabKey)) {
            if (FAILED(hr = DrawSurface(newTab, [&](ID2D1DeviceContext* dc) { return DrawNewTab(dc, newTabWanted); }))) return hr;
            newTabKey = newTabWanted;
            changed = true;
        }
        if (!hasPainted || newTabX.to != plus.left) {
            // The new-tab button travels with the end of the tab row.
            if (FAILED(hr = Move(newTabX, plus.left, hasPainted ? movement : Instant, now,
                                 [&](float v) { return newTab.visual->SetOffsetX(v); },
                                 [&](IDCompositionAnimation* a) { return newTab.visual->SetOffsetX(a); })) ||
                FAILED(hr = newTab.visual->SetOffsetY(static_cast<float>(plus.top))))
                return hr;
            changed = true;
        }
        std::erase_if(pictures, [&](const Picture& cached) {
            return std::none_of(state.tabs.begin(), state.tabs.end(), [&](const TabVisual& tab) {
                return tab.id == cached.id && tab.iconRevision == cached.revision && tab.icon == cached.handle;
            });
        });
        previousStripWidth = stripWidth;
        previousStripHeight = stripHeight;
        previousDpi = scale;
        hasPainted = true;
        if (!changed) return S_FALSE;
        if (FAILED(hr = composition->Commit())) return hr;
        // Establish readiness once for the new target. Ordinary hover, resize,
        // focus and metadata paints never synchronously wait for the compositor.
        if (firstSurface) {
            if (FAILED(hr = composition->WaitForCommitCompletion())) return hr;
            firstSurface = false;
        }
        return S_OK;
    }
    unsigned PendingMs() const {
        const std::uint64_t now = GetTickCount64();
        std::uint64_t end = std::max(scroll.End(), newTabX.End());
        for (const auto& slot : slots) end = std::max({end, slot->x.End(), slot->right.End(), slot->ghost ? slot->ghostEnd : 0});
        return end > now ? static_cast<unsigned>(end - now) : 0;
    }
    void Invalidate() {
        dpi = 0;
        controls.painted = newTab.painted = false;
        for (auto& slot : slots) slot->painted = false;
    }
    ~Impl() {
        if (target) target->SetRoot(nullptr);
        if (composition) composition->Commit();
    }
};
TabHeaderRenderer::TabHeaderRenderer() = default;
TabHeaderRenderer::~TabHeaderRenderer() = default;
HRESULT TabHeaderRenderer::Paint(HWND window, const VisualState& state) {
    if (!state.layout.Usable()) return E_INVALIDARG;
    try {
        if (!impl_) impl_ = std::make_shared<Impl>();
        // Resource creation may enter COM. Reentrant disable releases the
        // owner's reference without destroying a draw still on this STA stack.
        auto drawing = impl_;
        return drawing->Paint(window, state);
    } catch (...) { return E_OUTOFMEMORY; }
}
unsigned TabHeaderRenderer::PendingMs() const { return impl_ ? impl_->PendingMs() : 0; }
void TabHeaderRenderer::Reset() { impl_.reset(); }
void TabHeaderRenderer::ThemeChanged() {
    if (impl_) impl_->Invalidate();
}
}
