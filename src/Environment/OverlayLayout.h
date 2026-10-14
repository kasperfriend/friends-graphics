#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include "OverlayItems.h"

namespace tuningoverlay {
// Platform-independent geometry: drawing and input use these same rectangles.
// All coordinates here are logical pixels; only the Win32/D3D boundary scales.
struct Box {
    float x=0,y=0,w=0,h=0;
    bool Contains(float px,float py) const {return px>=x&&py>=y&&px<x+w&&py<y+h;}
};
struct Tab {int header,begin,end;};
inline const std::vector<Tab>& Tabs() {
    static const auto tabs=[] {
        std::vector<Tab> result;
        for(int i=0;i<int(items.size());++i)if(items[i].kind==KIND_HEADER) {
            if(!result.empty())result.back().end=i;
            result.push_back({i,i+1,int(items.size())});
        }
        return result;
    }();
    return tabs;
}
inline std::string CompactLabel(const Item& item) {
    std::string result=item.label;
    if(item.kind==KIND_HEADER) {
        if(result.starts_with("--- "))result.erase(0,4);
        if(result.ends_with(" ---"))result.resize(result.size()-4);
    }else if(const auto hint=result.find(" (");hint!=result.npos)result.resize(hint);
    return result;
}
inline int SliderValue(const Item& item,float px,const Box& track) {
    const float t=std::clamp((px-track.x)/track.w,0.f,1.f);
    // Preserve both endpoints even for ranges not divisible by the step.
    if(t>=1)return item.hi;
    return std::clamp(item.lo+int((item.hi-item.lo)*t/item.step+.5f)*item.step,item.lo,item.hi);
}
struct OverlayLayout {
    static constexpr float minWidth=760,minHeight=570,rowHeight=36;
    Box panel{24,24,940,710};
    float scale=1,screenWidth=1920,screenHeight=1080;
    int activeTab=0;
    std::vector<int> offsets=std::vector<int>(Tabs().size(),0);
    enum class Gesture {None,Move,Resize,Scroll};
    Gesture gesture=Gesture::None;
    Box startPanel;
    float startX=0,startY=0;
    int startScroll=0;

    void Cancel(){gesture=Gesture::None;}
    void Clamp() {
        panel.w=std::clamp(panel.w,minWidth,screenWidth);
        panel.h=std::clamp(panel.h,minHeight,screenHeight);
        panel.x=std::clamp(panel.x,0.f,screenWidth-panel.w);
        panel.y=std::clamp(panel.y,0.f,screenHeight-panel.h);
        for(size_t i=0;i<offsets.size();++i)offsets[i]=std::clamp(offsets[i],0,MaxScroll(int(i)));
    }
    void Fit(int width,int height,float dpi) {
        if(width<=0||height<=0)return;
        float next=std::clamp(std::max(height/1440.f,std::min(dpi,1.15f)),.85f,1.5f);
        next=std::min({next,width/minWidth,height/minHeight});
        const float sw=std::max(minWidth,width/next),sh=std::max(minHeight,height/next);
        if(next!=scale||sw!=screenWidth||sh!=screenHeight)Cancel();
        scale=next;screenWidth=sw;screenHeight=sh;Clamp();
    }
    Box Title() const {return {panel.x,panel.y,panel.w-40,32};}
    Box Close() const {return {panel.x+panel.w-36,panel.y+4,28,24};}
    Box ResizeGrip() const {return {panel.x+panel.w-24,panel.y+panel.h-24,24,24};}
    Box Scope(int scope) const {
        const float width=(panel.w-166)/2;
        return {panel.x+150+scope*(width+8),panel.y+132,width,26};
    }
    Box TabButton(int tab) const {return {panel.x+12,panel.y+204+tab*34,212,30};}
    Box Content() const {return {panel.x+240,panel.y+240,panel.w-270,float(VisibleRows())*rowHeight};}
    Box ResetButton() const {const auto c=Content();return {c.x+c.w-142,panel.y+202,142,26};}
    int VisibleRows() const {return std::max(1,int((panel.h-276)/rowHeight));}
    int MaxScroll(int tab) const {return std::max(0,Tabs()[tab].end-Tabs()[tab].begin-VisibleRows());}
    void SelectTab(int tab) {if(tab>=0&&tab<int(Tabs().size())){activeTab=tab;Cancel();}}
    void Scroll(int rows) {offsets[activeTab]=std::clamp(offsets[activeTab]+rows,0,MaxScroll(activeTab));}
    Box Row(int index) const {
        auto b=Content();b.y+=(index-Tabs()[activeTab].begin-offsets[activeTab])*rowHeight;b.h=rowHeight;return b;
    }
    Box Slider(int index) const {auto b=Row(index);return {b.x+b.w-146,b.y+8,136,20};}
    Box Toggle(int index) const {auto b=Row(index);return {b.x+b.w-68,b.y+6,58,24};}
    Box ScrollTrack() const {auto b=Content();return {b.x+b.w+8,b.y,8,b.h};}
    Box ScrollThumb() const {
        auto b=ScrollTrack();const int count=Tabs()[activeTab].end-Tabs()[activeTab].begin;
        const float height=std::min(b.h,std::max(24.f,b.h*VisibleRows()/std::max(1,count)));
        b.y+=(b.h-height)*offsets[activeTab]/std::max(1,MaxScroll(activeTab));b.h=height;return b;
    }
    Box ScrollButton(bool down) const {return {panel.x+panel.w-(down?68.f:100.f),panel.y+panel.h-28,28,22};}
    int HitTab(float x,float y) const {
        for(int i=0;i<int(Tabs().size());++i)if(TabButton(i).Contains(x,y))return i;
        return -1;
    }
    int HitItem(float x,float y) const {
        const auto b=Content();if(!b.Contains(x,y))return -1;
        const int index=Tabs()[activeTab].begin+offsets[activeTab]+int((y-b.y)/rowHeight);
        return index<Tabs()[activeTab].end?index:-1;
    }
    bool Begin(float x,float y) {
        startX=x;startY=y;startPanel=panel;startScroll=offsets[activeTab];
        if(ResizeGrip().Contains(x,y))gesture=Gesture::Resize;
        else if(Title().Contains(x,y))gesture=Gesture::Move;
        else if(MaxScroll(activeTab)&&ScrollTrack().Contains(x,y)) {
            const auto thumb=ScrollThumb();
            if(!thumb.Contains(x,y))Scroll(y<thumb.y?-VisibleRows():VisibleRows());
            startScroll=offsets[activeTab];gesture=Gesture::Scroll;
        }else return false;
        return true;
    }
    void Drag(float x,float y) {
        if(gesture==Gesture::Move) {
            panel.x=startPanel.x+x-startX;panel.y=startPanel.y+y-startY;Clamp();
        }else if(gesture==Gesture::Resize) {
            // Keep the top-left fixed while resizing, including at screen edges.
            panel.w=std::clamp(startPanel.w+x-startX,minWidth,screenWidth-panel.x);
            panel.h=std::clamp(startPanel.h+y-startY,minHeight,screenHeight-panel.y);Clamp();
        }else if(gesture==Gesture::Scroll) {
            const float travel=ScrollTrack().h-ScrollThumb().h;
            if(travel>0)offsets[activeTab]=std::clamp(startScroll+int(std::round((y-startY)*MaxScroll(activeTab)/travel)),0,MaxScroll(activeTab));
        }
    }
};
}
