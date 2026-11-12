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
    auto layout=header::BuildLayout(Row(600,30),two,505);
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
        auto scaled=header::BuildLayout(Row(width,height,dpi),many,43);
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
    auto narrow=header::BuildLayout(Row(152,30),five,5);
    auto wide=header::BuildLayout(Row(1060,30),five,5,{narrow.scroll});
    Check("widening at last active tab fills newly available viewport",wide.firstVisible==0&&wide.Find(1)->Visible()&&wide.Find(5)->Visible());
    auto afterClose=header::BuildLayout(Row(600,30),two,505,{400});
    Check("closing end tabs fills available viewport",afterClose.firstVisible==0&&afterClose.Find(101)->Visible());
    auto moved=header::Translate(layout.tabs[1].body,{-1912,-200});
    Check("negative monitor origin preserves shared body size",Rect(moved,-1712,-200,-1542,-170));
    auto reordered=header::BuildLayout(Row(600,30),std::vector<std::uint64_t>{505,101},505);
    Check("reorder preserves target identity independently of its index",reordered.Hit({10,10})==header::TargetId{header::TargetKind::Tab,505});
    Check("undersized allocation publishes no targets",!header::BuildLayout(Row(151,30),two,101).Usable());
    // ---- Overflow (R7-R10) ----
    std::vector<std::uint64_t> ids;for(unsigned i=1;i<=500;++i)ids.push_back(i);
    for(const UINT dpi:{96u,120u,144u,192u}){
        const int scale=static_cast<int>(dpi);
        const int width=MulDiv(900,scale,96),height=MulDiv(30,scale,96),unit=MulDiv(30,scale,96),minimum=MulDiv(76,scale,96);
        for(const size_t count:{size_t{1},size_t{8},size_t{64},size_t{120},size_t{500}}){
            const std::span<const std::uint64_t> some(ids.data(),count);
            const std::uint64_t active=count>2?count-2:1;
            auto row=header::BuildLayout(Row(width,height,dpi),some,active);
            const auto* shown=row.Find(active);
            const bool reachable=shown&&shown->Visible()&&shown->bounds.left>=row.tabViewport.left&&shown->bounds.right<=row.tabViewport.right;
            Check("active tab is fully inside the tab viewport",reachable);
            const bool expectOverflow=static_cast<long long>(minimum)*static_cast<long long>(count)>width-unit*2;
            Check("overflow starts only below the minimum width",row.overflowing==expectOverflow);
            if(row.overflowing){
                Check("overflowing tabs use the 76 DIP minimum",row.tabWidth==minimum);
                Check("scroll arrows bracket the tab viewport",Rect(row.scrollLeft,0,0,unit,height)&&row.tabViewport.left==unit&&
                      row.scrollRight.left==row.tabViewport.right&&row.scrollRight.right==width-unit*2);
                Check("new tab pins to the right end while overflowing",Rect(row.newTab,width-unit*2,0,width-unit,height)&&Rect(row.overflow,width-unit,0,width,height));
                Check("maximum scroll covers the content",row.maxScroll==row.contentWidth-(row.tabViewport.right-row.tabViewport.left)&&row.contentWidth==minimum*static_cast<int>(count));
            } else Check("no arrows without overflow",IsRectEmpty(&row.scrollLeft)&&IsRectEmpty(&row.scrollRight));
            bool clipped=true;size_t visibleCount=0;
            for(const auto& tab:row.tabs)if(tab.Visible()){++visibleCount;clipped=clipped&&tab.bounds.right>row.tabViewport.left&&tab.bounds.left<row.tabViewport.right;}
            const int fits=(row.tabViewport.right-row.tabViewport.left)/std::max(1,row.tabWidth)+2;
            Check("only tabs meeting the viewport are realised",clipped&&visibleCount<=static_cast<size_t>(fits));
        }
    }
    const std::span<const std::uint64_t> hundred(ids.data(),120);
    auto start=header::BuildLayout(Row(900,30),hundred,1,{0});
    Check("scroll start fades only the right edge",start.scroll==0&&!start.fadeLeft&&start.fadeRight);
    auto end=header::BuildLayout(Row(900,30),hundred,1,{1<<30,0,false});
    Check("scroll clamps to the end and fades only the left edge",end.scroll==end.maxScroll&&end.fadeLeft&&!end.fadeRight);
    auto negative=header::BuildLayout(Row(900,30),hundred,1,{-50,0,false});
    Check("negative scroll clamps to zero",negative.scroll==0);
    auto away=header::BuildLayout(Row(900,30),hundred,1,{2000,0,false});
    Check("wheel scrolling may leave the active tab hidden",away.scroll==2000&&!away.Find(1)->Visible());
    Check("reveal returns the offset that shows the active tab",away.Reveal(1)==0);
    Check("a wheel notch moves one tab and touchpad deltas move in proportion",header::ScrollStep(away,120)==76&&header::ScrollStep(away,-30)==-19);
    const int partial=away.tabs[2000/76].bounds.left;
    Check("a partly hidden tab keeps its full width",partial<away.tabViewport.left&&away.tabs[2000/76].bounds.right-partial==76);
    Check("arrow hits win over the tab scrolled under them",away.Hit({5,15}).kind==header::TargetKind::ScrollLeft&&
          away.Hit({away.scrollRight.left+2,15}).kind==header::TargetKind::ScrollRight);
    // Narrow tabs (< 140 DIP) show the close button only when active.
    const std::vector<std::uint64_t> eight{1,2,3,4,5,6,7,8};
    auto clipRow=header::BuildLayout(Row(1100,30),eight,3);
    const auto* inactive=clipRow.Find(2);const auto* selected=clipRow.Find(3);
    Check("narrow inactive tab hides its close button",clipRow.tabWidth<140&&inactive&&!inactive->showClose&&IsRectEmpty(&inactive->close)&&EqualRect(&inactive->body,&inactive->bounds));
    Check("narrow active tab keeps its close button",selected&&selected->showClose&&!IsRectEmpty(&selected->close));
    Check("narrow inactive tab is hit as a tab where its close would be",clipRow.Hit({inactive->bounds.right-5,15})==header::TargetId{header::TargetKind::Tab,2});
    auto roomy=header::BuildLayout(Row(1300,30),eight,3);
    Check("tabs at 140 DIP and wider all show close buttons",roomy.tabWidth>=140&&roomy.Find(2)->showClose);
    // Width lock (R10): closing with the mouse keeps the next close under the pointer.
    const std::vector<std::uint64_t> three{1,2,3};
    auto locked=header::BuildLayout(Row(900,30),three,1,{0,150});
    Check("locked width is kept while the pointer stays",locked.tabWidth==150&&Rect(locked.newTab,450,0,480,30));
    auto lockedOverflow=header::BuildLayout(Row(400,30),ids.size()>10?std::span<const std::uint64_t>(ids.data(),10):hundred,1,{0,150});
    Check("a locked row may still overflow and scroll",lockedOverflow.overflowing&&lockedOverflow.tabWidth==150);
    std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
