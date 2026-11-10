#include "TabHeaderRenderer.h"
#include <algorithm>
#include <cmath>
#include <d2d1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <wincodec.h>
#include <wrl/client.h>

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
}
// Two states draw the same pixels. Allocation owner/revision only track
// identity, so they are not compared.
bool SamePixels(const VisualState& a,const VisualState& b) {
    const auto same=[](const RECT& x,const RECT& y){return EqualRect(&x,&y)!=FALSE;};
    const auto& l=a.layout;const auto& r=b.layout;
    if(!same(l.allocation.bounds,r.allocation.bounds)||l.allocation.dpi!=r.allocation.dpi||!same(l.viewport,r.viewport)||
       !same(l.newTab,r.newTab)||!same(l.overflow,r.overflow)||l.active!=r.active||l.tabs.size()!=r.tabs.size()||
       a.tabs.size()!=b.tabs.size()||!(a.hot==b.hot)||!(a.pressed==b.pressed)||!(a.focus==b.focus)||a.active!=b.active||
       a.focused!=b.focused||a.canCreate!=b.canCreate||a.busyTab!=b.busyTab||!same(l.tabViewport,r.tabViewport)||
       !same(l.scrollLeft,r.scrollLeft)||!same(l.scrollRight,r.scrollRight)||l.fadeLeft!=r.fadeLeft||l.fadeRight!=r.fadeRight)return false;
    for(size_t i=0;i<l.tabs.size();++i) {
        const auto& x=l.tabs[i];const auto& y=r.tabs[i];
        if(x.id!=y.id||x.showClose!=y.showClose||!same(x.bounds,y.bounds)||!same(x.body,y.body)||!same(x.close,y.close)||
           !same(x.closeGlyph,y.closeGlyph))return false;
    }
    for(size_t i=0;i<a.tabs.size();++i) {
        const auto& x=a.tabs[i];const auto& y=b.tabs[i];
        if(x.id!=y.id||x.iconRevision!=y.iconRevision||x.icon!=y.icon||x.title!=y.title)return false;
    }
    return true;
}
struct TabHeaderRenderer::Impl {
    ComPtr<ID3D11Device> graphics;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionSurface> surface;
    ComPtr<ID2D1Factory> drawing;
    ComPtr<IDWriteFactory> writing;
    ComPtr<IWICImagingFactory> imaging;
    ComPtr<IDWriteTextFormat> font;
    ComPtr<IDWriteInlineObject> ellipsis;
    struct IconSource {
        std::uint64_t id{},revision{};
        HICON handle{};
        ComPtr<IWICBitmap> pixels;
    };
    std::vector<IconSource> icons;
    HWND window{};
    UINT width{},height{},dpi{};
    bool firstSurface{true};
    // DirectComposition keeps presented content; an identical state is not redrawn.
    VisualState painted;
    bool hasPainted{};
    HRESULT Initialize(HWND owned) {
        HRESULT hr=S_OK;
        if(!drawing&&FAILED(hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,drawing.GetAddressOf())))return hr;
        if(!writing&&FAILED(hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(writing.GetAddressOf()))))return hr;
        if(!imaging&&FAILED(hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&imaging))))return hr;
        if(!graphics) {
            if(FAILED(hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&graphics,nullptr,nullptr)))return hr;
            ComPtr<IDXGIDevice> dxgi;
            if(FAILED(hr=graphics.As(&dxgi))||FAILED(hr=DCompositionCreateDevice(dxgi.Get(),IID_PPV_ARGS(&composition)))||
               FAILED(hr=composition->CreateTargetForHwnd(owned,TRUE,&target))||
               FAILED(hr=composition->CreateVisual(&visual)))return hr;
            window=owned;
        }
        return window==owned?S_OK:E_INVALIDARG;
    }
    HRESULT Font(UINT scale) {
        if(font&&dpi==scale)return S_OK;
        font.Reset();ellipsis.Reset();dpi=scale;
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if(!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,scale))
            return HRESULT_FROM_WIN32(GetLastError());
        const auto& lf=metrics.lfCaptionFont;
        ComPtr<IDWriteGdiInterop> interop;
        ComPtr<IDWriteFont> nativeFont;
        HRESULT hr=writing->GetGdiInterop(&interop);
        if(FAILED(hr)||FAILED(hr=interop->CreateFontFromLOGFONT(&lf,&nativeFont)))return hr;
        const float size=static_cast<float>(std::abs(lf.lfHeight));
        if(size==0)return E_INVALIDARG;
        if(FAILED(hr=writing->CreateTextFormat(lf.lfFaceName,nullptr,nativeFont->GetWeight(),nativeFont->GetStyle(),
                nativeFont->GetStretch(),size,L"",&font)))return hr;
        if(FAILED(hr=font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER))||
           FAILED(hr=font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP))||
           FAILED(hr=writing->CreateEllipsisTrimmingSign(font.Get(),&ellipsis)))return hr;
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
        return font->SetTrimming(&trim,ellipsis.Get());
    }
    HRESULT IconPixels(const TabVisual& tab,IWICBitmap** result) {
        *result=nullptr;
        if(!tab.icon)return S_FALSE;
        for(auto& icon:icons)if(icon.id==tab.id&&icon.revision==tab.iconRevision&&icon.handle==tab.icon)
            return icon.pixels.CopyTo(result);
        ComPtr<IWICBitmap> source,pixels;
        ComPtr<IWICFormatConverter> converter;
        HRESULT hr=imaging->CreateBitmapFromHICON(tab.icon,&source);
        if(FAILED(hr)||FAILED(hr=imaging->CreateFormatConverter(&converter))||
           FAILED(hr=converter->Initialize(source.Get(),GUID_WICPixelFormat32bppPBGRA,
                     WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))||
           FAILED(hr=imaging->CreateBitmapFromSource(converter.Get(),WICBitmapCacheOnLoad,&pixels)))return hr;
        icons.push_back({tab.id,tab.iconRevision,tab.icon,pixels});
        return pixels.CopyTo(result);
    }
    HRESULT Draw(ID2D1RenderTarget* draw,const VisualState& state) {
        HIGHCONTRASTW hc{sizeof(hc)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0);
        const bool contrast=(hc.dwFlags&HCF_HIGHCONTRASTON)!=0;
        const bool caption=state.layout.allocation.region==Region::Caption;
        // Open theme data only on our HWND. Existing native control handles are
        // borrowed for this draw and must never be closed or reassociated.
        Theme native(window,caption?L"WINDOW":L"TOOLBAR",caption?nullptr:NativeControlTheme(window,L"ToolbarWindow32"));
        const int part=caption?WP_CAPTION:TP_BUTTON;
        const int normal=caption?(state.active?CS_ACTIVE:CS_INACTIVE):TS_NORMAL;
        const COLORREF nativeText=GetSysColor(caption?(state.active?COLOR_CAPTIONTEXT:COLOR_INACTIVECAPTIONTEXT):COLOR_BTNTEXT);
        const COLORREF text=contrast?nativeText:native.Get(part,normal,TMT_TEXTCOLOR,nativeText);
        const COLORREF disabled=GetSysColor(COLOR_GRAYTEXT);
        const COLORREF accent=GetSysColor(COLOR_HIGHLIGHT);
        const COLORREF selectedText=contrast?GetSysColor(COLOR_HIGHLIGHTTEXT):text;
        const COLORREF selectedFill=contrast?accent:GetSysColor(COLOR_WINDOW);
        const COLORREF hotFill=contrast?accent:GetSysColor(COLOR_HIGHLIGHT);
        ComPtr<ID2D1SolidColorBrush> brush;
        HRESULT hr=draw->CreateSolidColorBrush(Color(text),&brush);
        if(FAILED(hr))return hr;
        auto fill=[&](RECT r,COLORREF color,float opacity) {
            if(IsRectEmpty(&r))return;
            brush->SetColor(Color(color,opacity));draw->FillRectangle(Box(r),brush.Get());
        };
        auto line=[&](float x1,float y1,float x2,float y2,COLORREF color,float opacity,float thickness) {
            brush->SetColor(Color(color,opacity));
            draw->DrawLine(D2D1::Point2F(x1,y1),D2D1::Point2F(x2,y2),brush.Get(),thickness);
        };
        const UINT scale=state.layout.allocation.dpi;
        auto px=[&](int n){return MulDiv(n,static_cast<int>(scale),96);};
        const float stroke=static_cast<float>(std::max(1,px(1)));
        draw->Clear(D2D1::ColorF(0,0,0,0));
        // The native caption stays transparent in every provider/theme mode.
        // A reserved client row uses its native rebar theme, including classic
        // and high-contrast system colors, rather than extending glass downward.
        if(!caption) {
            Theme rebar(window,L"REBAR",NativeControlTheme(window,L"ReBarWindow32"));
            fill(state.layout.viewport,contrast?GetSysColor(COLOR_BTNFACE):
                 rebar.Get(RP_BACKGROUND,0,TMT_FILLCOLOR,GetSysColor(COLOR_BTNFACE)),1.f);
        }
        // Tabs draw inside their own viewport. While scrolled, the clipped
        // edges fade out through an opacity mask, as in Firefox's tab strip.
        const RECT tabArea=state.layout.tabViewport;
        ComPtr<ID2D1Layer> fadeLayer;
        bool faded=false;
        if(state.layout.fadeLeft||state.layout.fadeRight) {
            const float left=static_cast<float>(tabArea.left),right=static_cast<float>(tabArea.right);
            const float span=std::max(1.f,right-left),edge=std::min(span/2.f,static_cast<float>(px(24)))/span;
            const D2D1_GRADIENT_STOP stops[]{
                {0.f,D2D1::ColorF(0,0,0,state.layout.fadeLeft?0.f:1.f)},{edge,D2D1::ColorF(0,0,0,1.f)},
                {1.f-edge,D2D1::ColorF(0,0,0,1.f)},{1.f,D2D1::ColorF(0,0,0,state.layout.fadeRight?0.f:1.f)}};
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> mask;
            if(SUCCEEDED(draw->CreateGradientStopCollection(stops,4,&collection))&&
               SUCCEEDED(draw->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(left,0),D2D1::Point2F(right,0)),
                                                          collection.Get(),&mask))&&
               SUCCEEDED(draw->CreateLayer(nullptr,&fadeLayer))) {
                draw->PushLayer(D2D1::LayerParameters(Box(tabArea),nullptr,D2D1_ANTIALIAS_MODE_ALIASED,
                                                      D2D1::IdentityMatrix(),1.f,mask.Get()),fadeLayer.Get());
                faded=true;
            }
        }
        if(!faded)draw->PushAxisAlignedClip(Box(tabArea),D2D1_ANTIALIAS_MODE_ALIASED);
        hr=DrawTabs(draw,state,brush.Get(),{text,disabled,accent,selectedText,selectedFill,hotFill,contrast});
        if(faded)draw->PopLayer();else draw->PopAxisAlignedClip();
        if(FAILED(hr))return hr;
        auto affordance=[&](TargetId target,RECT bounds,bool enabled) {
            if(!enabled)return;
            if(state.pressed==target&&state.hot==target)fill(bounds,hotFill,contrast?1.f:.30f);
            else if(state.hot==target)fill(bounds,hotFill,contrast?1.f:.16f);
        };
        const bool newEnabled=state.canCreate;
        affordance({TargetKind::NewTab,0},state.layout.newTab,newEnabled);
        affordance({TargetKind::Overflow,0},state.layout.overflow,true);
        auto controlInk=[&](TargetId id,bool enabled) {
            return !enabled?disabled:contrast&&state.hot==id?GetSysColor(COLOR_HIGHLIGHTTEXT):text;
        };
        const auto plus=state.layout.newTab,overflow=state.layout.overflow;
        const float cx=(plus.left+plus.right)/2.f,cy=(plus.top+plus.bottom)/2.f,r=static_cast<float>(px(4));
        const COLORREF plusInk=controlInk({TargetKind::NewTab,0},newEnabled);
        line(cx-r,cy,cx+r,cy,plusInk,1.f,stroke);line(cx,cy-r,cx,cy+r,plusInk,1.f,stroke);
        const float ox=(overflow.left+overflow.right)/2.f,oy=(overflow.top+overflow.bottom)/2.f;
        const COLORREF overflowInk=controlInk({TargetKind::Overflow,0},true);
        line(ox-r,oy-r/2,ox,oy+r/2,overflowInk,1.f,stroke);
        line(ox,oy+r/2,ox+r,oy-r/2,overflowInk,1.f,stroke);
        // Scroll arrows: chevrons that grey out at either end.
        auto arrow=[&](TargetKind kind,RECT bounds,bool enabled,float direction) {
            if(IsRectEmpty(&bounds))return;
            affordance({kind,0},bounds,enabled);
            const COLORREF ink=controlInk({kind,0},enabled);
            const float ax=(bounds.left+bounds.right)/2.f,ay=(bounds.top+bounds.bottom)/2.f,half=r/2.f;
            line(ax+half*direction,ay-r,ax-half*direction,ay,ink,1.f,stroke);
            line(ax-half*direction,ay,ax+half*direction,ay+r,ink,1.f,stroke);
        };
        arrow(TargetKind::ScrollLeft,state.layout.scrollLeft,state.layout.fadeLeft,1.f);
        arrow(TargetKind::ScrollRight,state.layout.scrollRight,state.layout.fadeRight,-1.f);
        if(state.focused&&(state.focus.kind==TargetKind::NewTab||state.focus.kind==TargetKind::Overflow)) {
            RECT focus=state.focus.kind==TargetKind::NewTab?plus:overflow;InflateRect(&focus,-px(3),-px(3));
            brush->SetColor(Color(text,.8f));draw->DrawRectangle(Box(focus),brush.Get(),stroke);
        }
        std::erase_if(icons,[&](const IconSource& cached) {
            return std::none_of(state.tabs.begin(),state.tabs.end(),[&](const TabVisual& tab) {
                return tab.id==cached.id&&tab.iconRevision==cached.revision&&tab.icon==cached.handle;
            });
        });
        return S_OK;
    }
    struct Inks {COLORREF text,disabled,accent,selectedText,selectedFill,hotFill;bool contrast;};
    HRESULT DrawTabs(ID2D1RenderTarget* draw,const VisualState& state,ID2D1SolidColorBrush* brush,const Inks& inks) {
        const auto [text,disabled,accent,selectedText,selectedFill,hotFill,contrast]=inks;
        auto fill=[&](RECT r,COLORREF color,float opacity) {
            if(IsRectEmpty(&r))return;
            brush->SetColor(Color(color,opacity));draw->FillRectangle(Box(r),brush);
        };
        auto line=[&](float x1,float y1,float x2,float y2,COLORREF color,float opacity,float thickness) {
            brush->SetColor(Color(color,opacity));
            draw->DrawLine(D2D1::Point2F(x1,y1),D2D1::Point2F(x2,y2),brush,thickness);
        };
        auto affordance=[&](TargetId target,RECT bounds,bool enabled) {
            if(!enabled)return;
            if(state.pressed==target&&state.hot==target)fill(bounds,hotFill,contrast?1.f:.30f);
            else if(state.hot==target)fill(bounds,hotFill,contrast?1.f:.16f);
        };
        const UINT scale=state.layout.allocation.dpi;
        auto px=[&](int n){return MulDiv(n,static_cast<int>(scale),96);};
        const float stroke=static_cast<float>(std::max(1,px(1)));
        HRESULT hr=S_OK;
        for(const auto& tab:state.tabs) {
            const auto* geometry=state.layout.Find(tab.id);
            if(!geometry||!geometry->Visible())continue;
            const bool selected=tab.id==state.layout.active;
            // A slow tab shows a clock on its icon; the rest of the strip stays
            // usable so the user can retarget at once.
            constexpr bool enabled=true;
            const TargetId body{TargetKind::Tab,tab.id},close{TargetKind::Close,tab.id};
            if(selected)fill(geometry->bounds,selectedFill,contrast?1.f:(state.active?.26f:.16f));
            // A body hover includes its close button; the independent button
            // receives an additional affordance using the shared close bounds.
            if(enabled&&(state.hot==body||state.hot==close))
                fill(geometry->bounds,hotFill,contrast?1.f:.10f);
            affordance(body,geometry->body,enabled);
            affordance(close,geometry->close,enabled);
            const bool highlighted=contrast&&(selected||state.hot==body||state.hot==close);
            COLORREF ink=enabled?(highlighted?selectedText:text):disabled;
            const float opacity=state.active?1.f:.82f;
            const RECT bounds=geometry->bounds;
            if(selected)line(static_cast<float>(bounds.left),bounds.bottom-stroke/2,
                            static_cast<float>(bounds.right),bounds.bottom-stroke/2,
                            contrast?selectedText:accent,state.active?1.f:.65f,std::max(1.f,static_cast<float>(px(2))));
            ComPtr<IWICBitmap> pixels;
            hr=IconPixels(tab,&pixels);
            if(FAILED(hr))return hr;
            const int pad=px(8),icon=px(16);
            if(pixels) {
                ComPtr<ID2D1Bitmap> bitmap;
                if(FAILED(hr=draw->CreateBitmapFromWicBitmap(pixels.Get(),nullptr,&bitmap)))return hr;
                const float y=(bounds.top+bounds.bottom-icon)/2.f;
                draw->DrawBitmap(bitmap.Get(),D2D1::RectF(static_cast<float>(bounds.left+pad),y,
                    static_cast<float>(bounds.left+pad+icon),y+icon),enabled?opacity:.5f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            auto label=Box(geometry->body);label.left+=pad+icon+pad;label.right-=px(4);
            brush->SetColor(Color(ink,opacity));
            if(label.right>label.left)draw->DrawTextW(tab.title.data(),static_cast<UINT>(tab.title.size()),font.Get(),label,
                                                    brush,D2D1_DRAW_TEXT_OPTIONS_CLIP);
            // Narrow tabs keep the close button on the selected tab only.
            if(geometry->showClose&&!IsRectEmpty(&geometry->closeGlyph)) {
                const auto glyph=geometry->closeGlyph;
                line(glyph.left+.5f,glyph.top+.5f,glyph.right-.5f,glyph.bottom-.5f,ink,opacity,stroke);
                line(glyph.right-.5f,glyph.top+.5f,glyph.left+.5f,glyph.bottom-.5f,ink,opacity,stroke);
            }
            if(state.focused&&(state.focus==body||state.focus==close)) {
                RECT focus=state.focus==body?geometry->body:geometry->close;InflateRect(&focus,-px(3),-px(3));
                brush->SetColor(Color(ink,.8f));draw->DrawRectangle(Box(focus),brush,stroke);
            }
            if(state.busyTab&&tab.id==state.busyTab) {
                const float x=bounds.left+pad+icon/2.f,y=(bounds.top+bounds.bottom)/2.f,r=px(6)*1.f;
                brush->SetColor(Color(text,.8f));draw->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),r,r),brush,stroke);
                line(x,y,x,y-r*.6f,text,1.f,stroke);line(x,y,x+r*.5f,y,text,1.f,stroke);
            }
        }
        return hr;
    }
    HRESULT Paint(HWND owned,const VisualState& state) {
        if(hasPainted&&dpi&&window==owned&&SamePixels(painted,state))return S_FALSE;
        hasPainted=false;
        HRESULT hr=Initialize(owned);
        if(FAILED(hr)||FAILED(hr=Font(state.layout.allocation.dpi)))return hr;
        const UINT w=state.layout.viewport.right,h=state.layout.viewport.bottom;
        if(!w||!h)return E_INVALIDARG;
        if(width!=w||height!=h||!surface) {
            surface.Reset();
            if(FAILED(hr=composition->CreateSurface(w,h,DXGI_FORMAT_B8G8R8A8_UNORM,
                            DXGI_ALPHA_MODE_PREMULTIPLIED,&surface)))return hr;
            width=w;height=h;
        }
        ComPtr<IDXGISurface> pixels;
        POINT offset{};
        if(FAILED(hr=surface->BeginDraw(nullptr,IID_PPV_ARGS(&pixels),&offset)))return hr;
        // This is the DXGI-surface route: it owns a separate D2D render-target
        // draw pair, followed by the composition surface's own EndDraw.
        ComPtr<ID2D1RenderTarget> draw;
        auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96.f,96.f);
        hr=drawing->CreateDxgiSurfaceRenderTarget(pixels.Get(),&properties,&draw);
        if(SUCCEEDED(hr)) {
            draw->BeginDraw();
            draw->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),static_cast<float>(offset.y)));
            draw->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            draw->PushAxisAlignedClip(Box(state.layout.viewport),D2D1_ANTIALIAS_MODE_ALIASED);
            try {hr=Draw(draw.Get(),state);}
            catch(...) {hr=E_OUTOFMEMORY;}
            draw->PopAxisAlignedClip();
            const HRESULT ended=draw->EndDraw();
            if(SUCCEEDED(hr))hr=ended;
        }
        // Even factory/icon/font/draw failures must release an acquired draw.
        const HRESULT ended=surface->EndDraw();
        if(FAILED(hr))return hr;
        if(FAILED(ended))return ended;
        if(FAILED(hr=visual->SetContent(surface.Get()))||FAILED(hr=target->SetRoot(visual.Get()))||
           FAILED(hr=composition->Commit()))return hr;
        // Establish readiness once for the new target. Ordinary hover, resize,
        // focus and metadata paints never synchronously wait for the compositor.
        if(firstSurface) {
            if(FAILED(hr=composition->WaitForCommitCompletion()))return hr;
            firstSurface=false;
        }
        painted=state;hasPainted=true;
        return S_OK;
    }
    ~Impl() {
        if(target)target->SetRoot(nullptr);
        if(composition)composition->Commit();
    }
};
TabHeaderRenderer::TabHeaderRenderer()=default;
TabHeaderRenderer::~TabHeaderRenderer()=default;
HRESULT TabHeaderRenderer::Paint(HWND window,const VisualState& state) {
    if(!state.layout.Usable())return E_INVALIDARG;
    try {
        if(!impl_)impl_=std::make_shared<Impl>();
        // Resource creation may enter COM. Reentrant disable releases the
        // owner's reference without destroying a draw still on this STA stack.
        auto drawing=impl_;
        return drawing->Paint(window,state);
    } catch(...) {return E_OUTOFMEMORY;}
}
void TabHeaderRenderer::Reset(){impl_.reset();}
void TabHeaderRenderer::ThemeChanged(){if(impl_){impl_->dpi=0;impl_->hasPainted=false;}}
}
