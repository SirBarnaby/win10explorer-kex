#include "HeaderLayout.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {
unsigned failures{},checks{};
void Check(const char* name,bool passed) {
    ++checks;if(!passed)++failures;
    std::cout<<(passed?"PASS ":"FAIL ")<<name<<'\n';
}
bool Rect(const RECT& r,LONG l,LONG t,LONG right,LONG b) {
    return r.left==l&&r.top==t&&r.right==right&&r.bottom==b;
}
bool Overlap(RECT a,RECT b) {RECT intersect{};return IntersectRect(&intersect,&a,&b)!=FALSE;}
header::Allocation Row(int width,int height,UINT dpi=96) {
    header::Allocation allocation;allocation.region=header::Region::AllocatedRow;
    allocation.bounds={0,31,width,31+height};allocation.dpi=dpi;return allocation;
}
}
int main() {
    header::NativeGeometry native;
    native.complete=true;native.windowSize={1050,700};native.clientOffset={8,8};native.dpi=96;
    native.paint={0,0,1050,31};native.title={345,8,893,23};native.custom[0]={33,0,344,31};
    const RECT buttons{913,0,1050,31};RECT bounds{};
    Check("native slot converts once to signed frame-client coordinates",
        header::CaptionBounds(native,buttons,152,bounds)&&Rect(bounds,337,-8,885,23));
    native.complete=false;
    Check("unknown native occupancy is not free caption space",!header::CaptionBounds(native,buttons,152,bounds));
    native.complete=true;native.custom[1]={500,0,600,31};
    Check("second native contextual region prevents overlap",!header::CaptionBounds(native,buttons,152,bounds));
    native.custom[1]={};native.title={345,8,470,23};
    Check("narrow native title slot requires native row",!header::CaptionBounds(native,buttons,152,bounds));
    native.title={345,8,920,23};
    Check("native caption buttons remain outside owned area",!header::CaptionBounds(native,buttons,152,bounds));

    const std::vector<std::uint64_t> two{101,505};
    auto layout=header::BuildLayout(Row(600,30),two,505,0);
    Check("two tabs have independently specified widths",Rect(layout.tabs[0].bounds,0,0,200,30)&&Rect(layout.tabs[1].bounds,200,0,400,30));
    Check("body and close split at the same shared edge",Rect(layout.tabs[0].body,0,0,170,30)&&Rect(layout.tabs[0].close,170,0,200,30));
    Check("close glyph stays in independent close target",Rect(layout.tabs[0].closeGlyph,182,12,189,19));
    Check("new and overflow leave free drag space",Rect(layout.newTab,400,0,430,30)&&Rect(layout.overflow,570,0,600,30));
    Check("body hit has stable identity",layout.Hit({169,15})==header::TargetId{header::TargetKind::Tab,101});
    Check("close hit has separate stable identity",layout.Hit({170,15})==header::TargetId{header::TargetKind::Close,101});
    Check("bottom and right edges are excluded",layout.Hit({170,30}).kind==header::TargetKind::None&&layout.Hit({600,15}).kind==header::TargetKind::None);
    Check("blank viewport is not an interactive target",layout.Hit({500,15}).kind==header::TargetKind::None);
    std::vector<std::uint64_t> many;
    for(unsigned i=1;i<=64;++i)many.push_back(i);
    for(const UINT dpi:{96u,120u,144u,192u}){
        const int width=MulDiv(900,dpi,96),height=MulDiv(30,dpi,96);
        auto scaled=header::BuildLayout(Row(width,height,dpi),many,43,0);
        Check("many tabs keep active tab visible at representative DPI",scaled.Find(43)&&scaled.Find(43)->Visible());
        Check("overflow remains reachable at representative DPI",scaled.Hit({width-1,height-1}).kind==header::TargetKind::Overflow);
        bool disjoint=true;
        std::vector<RECT> targets{scaled.newTab,scaled.overflow};
        for(const auto& tab:scaled.tabs)if(tab.Visible()){
            disjoint=disjoint&&!Overlap(tab.body,tab.close);
            targets.push_back(tab.body);targets.push_back(tab.close);
            RECT clipped{};IntersectRect(&clipped,&tab.closeGlyph,&tab.close);
            disjoint=disjoint&&EqualRect(&clipped,&tab.closeGlyph);
        }
        for(size_t a=0;a<targets.size();++a)for(size_t b=a+1;b<targets.size();++b)disjoint=disjoint&&!Overlap(targets[a],targets[b]);
        Check("every target and glyph uses non-overlapping shared geometry",disjoint);
    }
    const std::vector<std::uint64_t> five{1,2,3,4,5};
    auto narrow=header::BuildLayout(Row(152,30),five,5,0);
    auto wide=header::BuildLayout(Row(1060,30),five,5,narrow.firstVisible);
    Check("widening at last active tab fills newly available viewport",wide.firstVisible==0&&wide.Find(1)->Visible()&&wide.Find(5)->Visible());
    auto afterClose=header::BuildLayout(Row(600,30),two,505,4);
    Check("closing end tabs fills available viewport",afterClose.firstVisible==0&&afterClose.Find(101)->Visible());
    auto moved=header::Translate(layout.tabs[1].body,{-1912,-200});
    Check("negative monitor origin preserves shared body size",Rect(moved,-1712,-200,-1542,-170));
    auto reordered=header::BuildLayout(Row(600,30),std::vector<std::uint64_t>{505,101},505,0);
    Check("reorder preserves target identity independently of its index",reordered.Hit({10,10})==header::TargetId{header::TargetKind::Tab,505});
    Check("undersized allocation publishes no targets",!header::BuildLayout(Row(151,30),two,101,0).Usable());
    std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
