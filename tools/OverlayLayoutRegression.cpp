// Portable regression tests for the actual F7 geometry and gesture implementation.
// g++ -std=c++20 -Wall -Wextra -pedantic tools/OverlayLayoutRegression.cpp -o build/overlay-test
#include "../src/Environment/OverlayLayout.h"
#include <cassert>
#include <iostream>
#include <set>
#include <utility>

using namespace tuningoverlay;
static void Inside(const OverlayLayout& l) {
    assert(l.panel.x>=0&&l.panel.y>=0);
    assert(l.panel.w>=l.minWidth&&l.panel.h>=l.minHeight);
    assert(l.panel.x+l.panel.w<=l.screenWidth+.01f);
    assert(l.panel.y+l.panel.h<=l.screenHeight+.01f);
}
static void ReachEverySetting(OverlayLayout& l) {
    std::set<int> seen;
    for(int t=0;t<int(Tabs().size());++t) {
        const auto button=l.TabButton(t);
        assert(l.HitTab(button.x+1,button.y+1)==t);
        assert(l.panel.Contains(button.x+button.w-1,button.y+button.h-1));
        assert(button.y+button.h<=l.panel.y+l.panel.h-34); // Sidebar never overlaps footer.
        l.SelectTab(t);l.Scroll(-10000);
        const auto reset=l.ResetButton();
        assert(l.panel.Contains(reset.x+reset.w*.5f,reset.y+reset.h*.5f));
        assert(!l.Content().Contains(reset.x+reset.w*.5f,reset.y+reset.h*.5f));
        assert(l.HitItem(reset.x+reset.w*.5f,reset.y+reset.h*.5f)==-1);
        assert(!l.Title().Contains(reset.x+reset.w*.5f,reset.y+reset.h*.5f));
        assert(!l.ResizeGrip().Contains(reset.x+reset.w*.5f,reset.y+reset.h*.5f));
        for(int offset=0;offset<=l.MaxScroll(t);++offset) {
            assert(l.offsets[t]==offset);
            const auto& tab=Tabs()[t];
            for(int i=tab.begin+offset;i<std::min(tab.end,tab.begin+offset+l.VisibleRows());++i) {
                auto row=l.Row(i);
                assert(l.HitItem(row.x+1,row.y+1)==i);
                assert(l.HitItem(row.x+row.w-1,row.y+row.h-1)==i);
                assert(!l.ResizeGrip().Contains(row.x+row.w-1,row.y+row.h-1));
                assert(row.y+row.h<=l.panel.y+l.panel.h-34);
                assert(items[i].kind!=KIND_HEADER);
                const auto control=items[i].kind==KIND_SLIDER?l.Slider(i):l.Toggle(i);
                assert(l.HitItem(control.x+1,control.y+1)==i);
                seen.insert(i);
            }
            auto content=l.Content();
            assert(l.HitItem(content.x-1,content.y)==-1);
            assert(l.HitItem(content.x+content.w,content.y)==-1);
            assert(l.HitItem(content.x,content.y+content.h)==-1);
            l.Scroll(1);
        }
        l.Scroll(10000);assert(l.offsets[t]==l.MaxScroll(t));
        l.Scroll(-10000);assert(l.offsets[t]==0);
    }
    int count=0;
    for(int i=0;i<int(items.size());++i)if(items[i].kind!=KIND_HEADER){assert(seen.count(i));++count;}
    assert(count==int(seen.size()));
}
int main() {
    // Pinned so adding a tab is a deliberate act: the loop below then proves
    // every control in it is still reachable and hit-testable at every
    // supported resolution and DPI. Was 10 before AMBIENT OCCLUSION.
    assert(Tabs().size()==11);
    std::set<std::pair<std::wstring,std::wstring>> keys;
    for(const auto& item:items)if(item.kind!=KIND_HEADER) {
        assert(keys.emplace(item.section,item.key).second);
        assert(item.lo<=item.value&&item.value<=item.hi&&item.step>0);
        if(item.kind==KIND_SLIDER) {
            Box track{100,100,136,20};
            assert(SliderValue(item,-500,track)==item.lo);
            assert(SliderValue(item,track.x+track.w,track)==item.hi);
            assert(SliderValue(item,10000,track)==item.hi);
            int previous=item.lo;
            for(int x=100;x<=236;++x) {
                const int value=SliderValue(item,float(x),track);
                assert(value>=previous&&value>=item.lo&&value<=item.hi);
                assert(value==item.hi||(value-item.lo)%item.step==0);previous=value;
            }
        }
    }
    for(const auto& [width,height]:{std::pair{640,480},{800,600},{1280,720},{1920,1080},{2560,1440},{3840,2160}}) {
        for(float dpi:{1.f,1.25f,1.5f,2.f}) {
            OverlayLayout l;l.Fit(width,height,dpi);Inside(l);ReachEverySetting(l);
            // Drag to both extremes, retaining dimensions and keeping the title reachable.
            auto title=l.Title();const auto before=l.panel;
            assert(l.Begin(title.x+10,title.y+10));assert(l.gesture==OverlayLayout::Gesture::Move);
            l.Fit(width,height,dpi); // Draw's per-frame layout must not end a drag.
            assert(l.gesture==OverlayLayout::Gesture::Move);
            l.Drag(-10000,-10000);Inside(l);assert(l.panel.x==0&&l.panel.y==0);
            l.Drag(10000,10000);Inside(l);assert(l.panel.w==before.w&&l.panel.h==before.h);
            l.Cancel();assert(l.gesture==OverlayLayout::Gesture::None);
            // Moving returns to a useful anchor before resizing.
            title=l.Title();l.Begin(title.x+10,title.y+10);l.Drag(10,10);l.Cancel();
            auto grip=l.ResizeGrip();const float anchorX=l.panel.x,anchorY=l.panel.y;
            assert(l.Begin(grip.x+12,grip.y+12));assert(l.gesture==OverlayLayout::Gesture::Resize);
            l.Drag(-10000,-10000);Inside(l);
            assert(l.panel.w==l.minWidth&&l.panel.h==l.minHeight);
            assert(l.panel.x==anchorX&&l.panel.y==anchorY);
            ReachEverySetting(l);
            grip=l.ResizeGrip();l.Begin(grip.x+12,grip.y+12);l.Drag(10000,10000);Inside(l);
            assert(l.panel.x==anchorX&&l.panel.y==anchorY);
            l.Cancel();const auto settled=l.panel;l.Drag(0,0);
            assert(l.panel.x==settled.x&&l.panel.y==settled.y&&l.panel.w==settled.w&&l.panel.h==settled.h);
            // Resolution/DPI changes cancel an active gesture and re-clamp all content.
            title=l.Title();l.Begin(title.x+10,title.y+10);l.Fit(320,240,1);
            assert(l.gesture==OverlayLayout::Gesture::None);Inside(l);ReachEverySetting(l);
        }
    }
    OverlayLayout l;l.Fit(1920,1080,1);l.panel.h=l.minHeight;l.Clamp();
    int cloudTab=-1;
    for(int t=0;t<int(Tabs().size());++t)if(CompactLabel(items[Tabs()[t].header])=="VOLUMETRIC CLOUDS")cloudTab=t;
    assert(cloudTab>=0);
    l.SelectTab(cloudTab);assert(l.MaxScroll(cloudTab)>0);
    auto thumb=l.ScrollThumb();assert(l.Begin(thumb.x+1,thumb.y+1));
    assert(l.gesture==OverlayLayout::Gesture::Scroll);
    l.Drag(thumb.x+1,10000);assert(l.offsets[cloudTab]==l.MaxScroll(cloudTab));
    l.Drag(thumb.x+1,-10000);assert(l.offsets[cloudTab]==0);l.Cancel();
    l.Scroll(3);l.SelectTab(0);l.SelectTab(cloudTab);assert(l.offsets[cloudTab]==3);
    l.panel.h=1000;l.Clamp();assert(l.offsets[cloudTab]==0); // Growing removes stale scroll offsets.
    auto close=l.Close();assert(!l.Begin(close.x+1,close.y+1));
    auto reset=l.ResetButton();assert(!l.Begin(reset.x+1,reset.y+1));
    auto row=l.Row(Tabs()[cloudTab].begin);assert(!l.Begin(row.x+1,row.y+1));
    // A physical client position maps to the same hit on a differently-sized backbuffer.
    l.Fit(2560,1440,1.5f);auto tab=l.TabButton(4);
    float clientX=(tab.x+1)*l.scale*1280/2560,clientY=(tab.y+1)*l.scale*720/1440;
    assert(l.HitTab(clientX*2560/1280/l.scale,clientY*1440/720/l.scale)==4);
    for(int i=0;i<int(items.size());++i)assert(items[i].value==defaults[i]);
    // Scope, title, close, footer and sidebar are disjoint from editable rows.
    // Both scope buttons must also be distinct, on-panel targets.
    for(int scope=0;scope<2;++scope) {
        const auto b=l.Scope(scope);
        assert(l.HitItem(b.x+1,b.y+1)==-1);
        assert(!l.Title().Contains(b.x+1,b.y+1));
        assert(l.panel.Contains(b.x+1,b.y+1));
        assert(l.panel.Contains(b.x+b.w-1,b.y+b.h-1));
        assert(b.Contains(b.x+1,b.y+1)&&b.Contains(b.x+b.w-1,b.y+b.h-1));
    }
    assert(l.Scope(0).x+l.Scope(0).w<=l.Scope(1).x);
    for(bool down:{false,true}) {
        const auto b=l.ScrollButton(down);
        assert(l.HitItem(b.x+1,b.y+1)==-1);
        assert(!l.ResizeGrip().Contains(b.x+b.w-1,b.y+b.h-1));
    }
    std::cout<<"F7 overlay: all "<<keys.size()<<" settings reachable; tab, move, resize, scroll, scale and slider regressions passed\n";
}
