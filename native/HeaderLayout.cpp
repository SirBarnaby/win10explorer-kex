#include "HeaderLayout.h"
#include <algorithm>

namespace header {
namespace {
bool Contains(const RECT& r, POINT p) {
    return p.x>=r.left && p.x<r.right && p.y>=r.top && p.y<r.bottom;
}
bool Empty(const RECT& r) { return r.right<=r.left || r.bottom<=r.top; }
bool Overlap(const RECT& a,const RECT& b) {
    return !Empty(a)&&!Empty(b)&&a.left<b.right&&b.left<a.right&&a.top<b.bottom&&b.top<a.bottom;
}
}
bool TabGeometry::Visible() const { return !Empty(bounds); }
bool LayoutState::Usable() const { return allocation.region!=Region::Unavailable&&!Empty(viewport); }
const TabGeometry* LayoutState::Find(std::uint64_t id) const {
    for(const auto& tab:tabs)if(tab.id==id)return &tab;
    return nullptr;
}
TargetId LayoutState::Hit(POINT p) const {
    if(!Usable()||!Contains(viewport,p))return {};
    if(Contains(tabViewport,p))for(const auto& tab:tabs){
        if(Contains(tab.close,p))return {TargetKind::Close,tab.id};
        if(Contains(tab.body,p))return {TargetKind::Tab,tab.id};
    }
    if(Contains(scrollLeft,p))return {TargetKind::ScrollLeft,0};
    if(Contains(scrollRight,p))return {TargetKind::ScrollRight,0};
    if(Contains(newTab,p))return {TargetKind::NewTab,0};
    if(Contains(overflow,p))return {TargetKind::Overflow,0};
    return {};
}
int LayoutState::Reveal(std::uint64_t id) const {
    for(size_t i=0;i<tabs.size();++i)if(tabs[i].id==id){
        const int visible=tabViewport.right-tabViewport.left,left=tabs[i].x;
        int target=scroll;
        if(left<target)target=left;
        else if(left+tabWidth>target+visible)target=left+tabWidth-visible;
        return std::clamp(target,0,maxScroll);
    }
    return scroll;
}
TabGeometry LocalTab(int width,int height,UINT dpi,bool showClose) {
    TabGeometry tab;tab.showClose=showClose;tab.bounds={0,0,width,height};tab.body=tab.bounds;
    if(showClose){
        const int unit=MulDiv(ControlDip,static_cast<int>(dpi),96),radius=MulDiv(3,static_cast<int>(dpi),96);
        tab.close={width-unit,0,width,height};tab.body.right=tab.close.left;
        const int cx=(tab.close.left+tab.close.right)/2,cy=height/2;
        tab.closeGlyph={cx-radius,cy-radius,cx+radius+1,cy+radius+1};
    }
    return tab;
}
int ScrollStep(const LayoutState& layout,int wheelDelta) {
    return MulDiv(layout.tabWidth,wheelDelta,WHEEL_DELTA);
}
RECT Translate(RECT r,POINT origin) {
    r.left+=origin.x;r.right+=origin.x;r.top+=origin.y;r.bottom+=origin.y;return r;
}
bool CaptionBounds(const NativeGeometry& native,const RECT& buttons,int minimumWidth,RECT& out) {
    out={};
    if(!native.complete||Empty(native.paint)||Empty(native.title)||Empty(buttons))return false;
    RECT slot{native.title.left,buttons.top,native.title.right,buttons.bottom};
    if(slot.left<0||slot.top<0||slot.right>native.windowSize.cx||slot.bottom>native.windowSize.cy||
       slot.right-slot.left<minimumWidth||Overlap(slot,buttons))return false;
    for(const auto& occupied:native.custom)if(Overlap(slot,occupied))return false;
    out=Translate(slot,{-native.clientOffset.x,-native.clientOffset.y});
    return true;
}
LayoutState BuildLayout(const Allocation& allocation,std::span<const std::uint64_t> ids,
                        std::uint64_t active,const LayoutInput& input) {
    LayoutState result;result.allocation=allocation;result.active=active;
    const int dpi=static_cast<int>(allocation.dpi);
    const int width=allocation.bounds.right-allocation.bounds.left;
    const int height=allocation.bounds.bottom-allocation.bounds.top;
    const int unit=MulDiv(ControlDip,dpi,96);
    const int minimum=MulDiv(TabMinimumDip,dpi,96),maximum=MulDiv(TabMaximumDip,dpi,96);
    // The allocation request reserves one 92 DIP tab plus the two controls.
    const int usable=MulDiv(92,dpi,96);
    if(allocation.region==Region::Unavailable||ids.empty()||width<usable+unit*2||height<=0)return result;
    result.viewport={0,0,width,height};
    const int count=static_cast<int>(ids.size()),available=width-unit*2;
    const int tabWidth=input.lockedWidth>0?input.lockedWidth:std::clamp(available/count,minimum,maximum);
    result.overflowing=static_cast<long long>(tabWidth)*count>available;
    // Scroll arrows appear while overflowing when one minimum tab still fits
    // between them; the new-tab button then pins to the right end.
    RECT area{0,0,available,height};
    if(result.overflowing&&available-unit*2>=minimum){
        result.scrollLeft={0,0,unit,height};
        area={unit,0,width-unit*3,height};
        result.scrollRight={area.right,0,area.right+unit,height};
    }
    result.tabViewport=area;result.tabWidth=tabWidth;result.contentWidth=tabWidth*count;
    const int visible=area.right-area.left;
    result.maxScroll=std::max(0,result.contentWidth-visible);
    int scroll=std::clamp(input.scroll,0,result.maxScroll);
    const auto found=std::find(ids.begin(),ids.end(),active);
    if(input.revealActive&&found!=ids.end()){
        const int left=static_cast<int>(found-ids.begin())*tabWidth;
        if(left<scroll)scroll=left;
        else if(left+tabWidth>scroll+visible)scroll=left+tabWidth-visible;
        scroll=std::clamp(scroll,0,result.maxScroll);
    }
    result.scroll=scroll;result.fadeLeft=scroll>0;result.fadeRight=scroll<result.maxScroll;
    const bool narrow=tabWidth<MulDiv(TabClipDip,dpi,96);
    bool anyVisible=false;
    result.tabs.reserve(ids.size());
    for(size_t i=0;i<ids.size();++i){
        TabGeometry tab;tab.id=ids[i];tab.x=static_cast<int>(i)*tabWidth;
        tab.showClose=!narrow||ids[i]==active;
        const int left=area.left+tab.x-scroll,right=left+tabWidth;
        if(right>area.left&&left<area.right){
            if(!anyVisible){result.firstVisible=i;anyVisible=true;}
            const auto local=LocalTab(tabWidth,height,allocation.dpi,tab.showClose);
            const POINT origin{left,0};
            tab.bounds=Translate(local.bounds,origin);tab.body=Translate(local.body,origin);
            if(tab.showClose){tab.close=Translate(local.close,origin);tab.closeGlyph=Translate(local.closeGlyph,origin);}
        }
        result.tabs.push_back(tab);
    }
    if(result.overflowing)result.newTab={width-unit*2,0,width-unit,height};
    else {const int end=area.left+result.contentWidth;result.newTab={end,0,end+unit,height};}
    result.overflow={width-unit,0,width,height};
    return result;
}
}
