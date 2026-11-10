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
    for(const auto& tab:tabs){
        if(Contains(tab.close,p))return {TargetKind::Close,tab.id};
        if(Contains(tab.body,p))return {TargetKind::Tab,tab.id};
    }
    if(Contains(newTab,p))return {TargetKind::NewTab,0};
    if(Contains(overflow,p))return {TargetKind::Overflow,0};
    return {};
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
                        std::uint64_t active,size_t firstVisible) {
    LayoutState result;result.allocation=allocation;result.active=active;
    const int width=allocation.bounds.right-allocation.bounds.left;
    const int height=allocation.bounds.bottom-allocation.bounds.top;
    const int unit=MulDiv(30,static_cast<int>(allocation.dpi),96);
    const int minimum=MulDiv(92,static_cast<int>(allocation.dpi),96);
    const int maximum=MulDiv(200,static_cast<int>(allocation.dpi),96);
    if(allocation.region==Region::Unavailable||ids.empty()||width<minimum+unit*2||height<=0)return result;
    result.viewport={0,0,width,height};
    const int available=width-unit*2;
    const int tabWidth=std::clamp(available/static_cast<int>(ids.size()),minimum,maximum);
    const size_t capacity=std::max<size_t>(1,available/tabWidth);
    const auto found=std::find(ids.begin(),ids.end(),active);
    const size_t activeIndex=found==ids.end()?0:static_cast<size_t>(found-ids.begin());
    firstVisible=std::min(firstVisible,ids.size()>capacity?ids.size()-capacity:0);
    if(activeIndex<firstVisible)firstVisible=activeIndex;
    if(activeIndex>=firstVisible+capacity)firstVisible=activeIndex-capacity+1;
    result.firstVisible=firstVisible;
    int x=0;
    result.tabs.reserve(ids.size());
    for(size_t i=0;i<ids.size();++i){
        TabGeometry tab;tab.id=ids[i];
        if(i>=firstVisible&&i<firstVisible+capacity){
            tab.bounds={x,0,std::min(x+tabWidth,available),height};
            tab.close={tab.bounds.right-unit,0,tab.bounds.right,height};
            tab.body={x,0,tab.close.left,height};
            const int cx=(tab.close.left+tab.close.right)/2,cy=height/2;
            const int radius=MulDiv(3,static_cast<int>(allocation.dpi),96);
            tab.closeGlyph={cx-radius,cy-radius,cx+radius+1,cy+radius+1};
            x+=tabWidth;
        }
        result.tabs.push_back(tab);
    }
    result.newTab={x,0,x+unit,height};
    result.overflow={width-unit,0,width,height};
    return result;
}
}
