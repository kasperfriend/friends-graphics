#include "src/Environment/LocationTuning.h"
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include "src/Environment/OverlayFont.h"
#include "src/Environment/EnvironmentProfileManager.h"
#include "src/Environment/MenuDiagnostics.h"
#include "src/Environment/OverlayLayout.h"
#include "src/D3D9/ScopedStateBlock.h"
#include "src/Environment/WindDebug.h"

namespace tuningoverlay {
inline std::wstring ini;
inline bool visible=false,f7Down=false,mouseDown=false;
inline int hover=-1,dragItem=-1;
inline std::string saveError;
inline OverlayLayout layout;
inline std::atomic<int> wheelDelta{0};
inline std::atomic<int> cursorId{32512}; // IDC_ARROW; updated by the shared hit tests.
inline std::atomic<bool> inputInterrupted{false};
inline void CancelInteraction(){
 hover=-1;dragItem=-1;layout.Cancel();wheelDelta.store(0);cursorId.store(32512);
}

// The game's own window (captured from the D3D device in Draw). Mouse
// coordinates and the input hook below must both refer to this window, not
// to whatever happens to be foreground: polling GetForegroundWindow made
// the hit-test jump whenever focus flickered.
inline HWND gameWindow=nullptr;
inline HWND inputWindow=nullptr;
inline WNDPROC originalInputProc=nullptr;
// While the menu is up the game must not see the same clicks: without this,
// pressing a toggle also clicks the world behind the panel and dragging a
// slider orbits the camera, which warps/hides the cursor WoW is dragging
// with - the menu then hit-tests a cursor the player is not looking at.
// Mouse messages are swallowed only while `visible`; every other message,
// and everything once the menu closes, goes to the original procedure.
// Buttons and drags poll GetCursorPos / GetAsyncKeyState. Wheel deltas and
// focus/capture interruptions are collected here for the next update.
inline LRESULT CALLBACK InputWndProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
 if(message==WM_KILLFOCUS||message==WM_CANCELMODE||message==WM_CAPTURECHANGED)
  inputInterrupted.store(true);
 if(visible){
  switch(message){
   case WM_MOUSEWHEEL:
    wheelDelta.fetch_add(static_cast<short>(HIWORD(wParam)));return 0;
   case WM_LBUTTONDOWN:case WM_LBUTTONUP:case WM_LBUTTONDBLCLK:
   case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_RBUTTONDBLCLK:
   case WM_MBUTTONDOWN:case WM_MBUTTONUP:case WM_MBUTTONDBLCLK:
   case WM_MOUSEMOVE:case WM_MOUSEHWHEEL:
    return 0;
   case WM_SETCURSOR:
    if(LOWORD(lParam)==HTCLIENT){SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(cursorId.load())));return TRUE;}
    break;
  }
 }
 return originalInputProc?CallWindowProcW(originalInputProc,window,message,wParam,lParam):DefWindowProcW(window,message,wParam,lParam);
}
inline void ReleaseInputHook(){
 if(!inputWindow)return;
 if(IsWindow(inputWindow)&&reinterpret_cast<WNDPROC>(GetWindowLongPtrW(inputWindow,GWLP_WNDPROC))==&InputWndProc)
  SetWindowLongPtrW(inputWindow,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(originalInputProc));
 inputWindow=nullptr;originalInputProc=nullptr;
}
inline void InstallInputHook(HWND window){
 if(!window||!IsWindow(window))return;
 if(inputWindow==window&&originalInputProc)return;
 ReleaseInputHook();
 SetLastError(0);
 const LONG_PTR previous=SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&InputWndProc));
 if(previous&&previous!=reinterpret_cast<LONG_PTR>(&InputWndProc)){inputWindow=window;originalInputProc=reinterpret_cast<WNDPROC>(previous);}
 else if(!previous&&GetLastError())renderer::menudiagnostics::Write("INPUT HOOK FAILED win32="+std::to_string(GetLastError()));
}
inline void Configure(const std::wstring& base){CancelInteraction();ini=base+L"GraphicsEffects.ini";for(auto& i:items){if(i.kind==KIND_HEADER)continue;i.value=std::clamp(static_cast<int>(renderer::locationtuning::ReadEditorInt(i.section,i.key,defaults[&i-items.data()],ini.c_str())),i.lo,i.hi);}}
inline bool Save(Item& i){
 if(i.kind==KIND_HEADER)return false;
 SetLastError(0);
 if(renderer::locationtuning::Save(i.section,i.key,i.value)){saveError.clear();return true;}
 const DWORD error=GetLastError();
 saveError="Save failed (Windows "+std::to_string(error)+"). Send MenuDiagnostics.log";
 renderer::menudiagnostics::Write("SAVE FAILED control="+std::string(i.label)+" value="+std::to_string(i.value)+" win32="+std::to_string(error));
 renderer::EnvironmentProfileManager::Instance().WriteMenuReport("save failed");
 i.value=std::clamp(static_cast<int>(renderer::locationtuning::ReadEditorInt(i.section,i.key,defaults[&i-items.data()],ini.c_str())),i.lo,i.hi);
 return false;
}
inline bool ResetActiveTab(){
 if(layout.activeTab<0||layout.activeTab>=int(Tabs().size()))return false;
 const auto tab=Tabs()[layout.activeTab];bool changed=false,failed=false;
 for(int index=tab.begin;index<tab.end;++index){
  auto& item=items[index];const int value=std::clamp(defaults[index],item.lo,item.hi);
  const bool itemChanged=item.value!=value;item.value=value;
  if(Save(item))changed=itemChanged||changed;else failed=true;
 }
 if(failed)saveError="Reset incomplete; see MenuDiagnostics.log";
 else {saveError.clear();renderer::menudiagnostics::Write("RESET DEFAULTS tab="+std::string(items[tab.header].label));}
 return changed;
}
inline const std::array<unsigned char,7>& Glyph(char c){
 if(c>='a'&&c<='z')c=char(c-'a'+'A');
 static const std::array<unsigned char,7> blank{};
 static const std::array<unsigned char,7> dash={0,0,0,31,0,0,0};
 static const std::array<unsigned char,7> caret={4,10,17,0,0,0,0};
 static const std::array<unsigned char,7> slash={1,2,4,8,16,0,0};
 static const std::array<unsigned char,7> amp={12,18,12,10,18,19,13};
 static const std::array<unsigned char,7> glyphs[]={
  {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},
  {31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
  {14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
  {14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
  {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
  {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
  {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
  {2,6,10,18,31,2,2},{31,16,16,30,1,1,30},{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},
  {14,17,17,14,17,17,14},{14,17,17,15,1,1,14}
 };
 if(c>='A'&&c<='Z')return glyphs[c-'A'];if(c>='0'&&c<='9')return glyphs[26+c-'0'];
 if(c=='^')return caret;if(c=='-')return dash;if(c=='/')return slash;if(c=='&')return amp;return blank;
}
struct V {float x,y,z,w;DWORD color;};
inline void Rect(std::vector<V>& out,float x,float y,float w,float h,DWORD color){V a{x,y,0,1,color},b{x+w,y,0,1,color},c{x,y+h,0,1,color},d{x+w,y+h,0,1,color};out.insert(out.end(),{a,b,c,c,b,d});}
inline void BitmapText(std::vector<V>& out,float x,float y,const char* s,DWORD color,float scale=2.f){for(;*s;++s,x+=6*scale){if(*s==' '){continue;}auto& g=Glyph(*s);for(int yy=0;yy<7;++yy)for(int xx=0;xx<5;++xx)if(g[yy]&(16>>xx))Rect(out,x+xx*scale,y+yy*scale,scale,scale,color);}}

inline OverlayFont font;
inline OverlayFont windDebugFont;
inline float uiScale=1;
inline std::vector<std::string> environmentLines;
inline int renderWidth=0,renderHeight=0;
inline void Layout(int width,int height,HWND window){
 using GetDpi=UINT(WINAPI*)(HWND);
 auto getDpi=reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
 const float oldScale=layout.scale,oldWidth=layout.screenWidth,oldHeight=layout.screenHeight;
 layout.Fit(width,height,getDpi&&window?getDpi(window)/96.f:1.f);
 if(oldScale!=layout.scale||oldWidth!=layout.screenWidth||oldHeight!=layout.screenHeight)CancelInteraction();
 uiScale=layout.scale;
}
inline int HitScope(float px,float py){
 for(int i=0;i<2;++i)if(layout.Scope(i).Contains(px,py))return i;
 return -1;
}
inline bool SelectScope(int scope){
 if(!renderer::locationtuning::editable||scope<0||scope>1||(scope==1&&!renderer::locationtuning::active.areaId))return false;
 renderer::locationtuning::editZone=scope==0;dragItem=-1;
 for(auto& i:items)if(i.kind!=KIND_HEADER)i.value=std::clamp(static_cast<int>(renderer::locationtuning::ReadEditorInt(i.section,i.key,defaults[&i-items.data()],ini.c_str())),i.lo,i.hi);
 return true;
}
inline bool Update(){
 DWORD pid=0;HWND foreground=GetForegroundWindow();GetWindowThreadProcessId(foreground,&pid);
 HWND window=gameWindow&&IsWindow(gameWindow)?gameWindow:foreground;
 const bool focused=pid==GetCurrentProcessId()&&!IsIconic(window);
 const bool down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
 const bool f7=(GetAsyncKeyState(VK_F7)&0x8000)!=0;
 if(focused&&f7&&!f7Down){
  visible=!visible;CancelInteraction();mouseDown=down;
  if(visible)renderer::EnvironmentProfileManager::Instance().WriteMenuReport("F7 opened");
 }
 f7Down=f7;
 if(visible)InstallInputHook(window);else ReleaseInputHook();
 const bool interrupted=inputInterrupted.exchange(false);
 if(!visible||!focused||interrupted){CancelInteraction();mouseDown=down;return false;}
 RECT client{};
 if(!GetClientRect(window,&client)||client.right<=0||client.bottom<=0){CancelInteraction();mouseDown=down;return false;}
 Layout(renderWidth?renderWidth:client.right,renderHeight?renderHeight:client.bottom,window);
 POINT p{};
 if(!GetCursorPos(&p)||!ScreenToClient(window,&p)){CancelInteraction();mouseDown=down;return false;}
 const float px=(renderWidth?float(p.x)*renderWidth/client.right:float(p.x))/uiScale;
 const float py=(renderHeight?float(p.y)*renderHeight/client.bottom:float(p.y))/uiScale;
 if(!down){dragItem=-1;layout.Cancel();}
 const bool pressed=down&&!mouseDown;
 bool changed=false;
 // Consume a wheel only over the settings pane, and never during a drag.
 int wheel=wheelDelta.exchange(0);
 if(!down&&px>=layout.Content().x&&layout.panel.Contains(px,py)){
  const int steps=wheel/WHEEL_DELTA;
  layout.Scroll(-steps*3);
  wheelDelta.fetch_add(wheel-steps*WHEEL_DELTA);
 }
 if(pressed){
  if(layout.Close().Contains(px,py)){
   visible=false;CancelInteraction();ReleaseInputHook();mouseDown=down;return false;
  }
  if(layout.Begin(px,py)){dragItem=-1;}
  else if(const int tab=layout.HitTab(px,py);tab>=0){layout.SelectTab(tab);dragItem=-1;}
  else if(renderer::locationtuning::editable&&HitScope(px,py)>=0){SelectScope(HitScope(px,py));}
  else if(layout.ScrollButton(false).Contains(px,py)){layout.Scroll(-1);}
  else if(layout.ScrollButton(true).Contains(px,py)){layout.Scroll(1);}
  else if(layout.ResetButton().Contains(px,py)){dragItem=-1;changed=ResetActiveTab();}
  else if(const int index=layout.HitItem(px,py);index>=0){
   auto& item=items[index];
   if(item.kind==KIND_TOGGLE){item.value=!item.value;changed=Save(item);}
   // Labels are not slider tracks: clicking a label must not reset its value.
   else if(layout.Slider(index).Contains(px,py))dragItem=index;
  }
 }
 if(down&&layout.gesture!=OverlayLayout::Gesture::None)layout.Drag(px,py);
 else if(down&&dragItem>=0){
  auto& item=items[dragItem];const int value=SliderValue(item,px,layout.Slider(dragItem));
  if(value!=item.value){item.value=value;const bool saved=Save(item);changed=saved||changed;if(!saved)dragItem=-1;}
 }
 hover=layout.gesture==OverlayLayout::Gesture::None?(dragItem>=0?dragItem:layout.HitItem(px,py)):-1;
 const int cursor=layout.gesture==OverlayLayout::Gesture::Resize||layout.ResizeGrip().Contains(px,py)?32642:
  layout.gesture==OverlayLayout::Gesture::Move||layout.Title().Contains(px,py)?32646:32512;
 cursorId.store(cursor);
 SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(cursor)));
 mouseDown=down;return changed;
}
inline void SetupOverlayState(IDirect3DDevice9* d){
 d->SetVertexShader(nullptr);d->SetPixelShader(nullptr);d->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);d->SetTexture(0,nullptr);
 d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
 d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
 d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);d->SetRenderState(D3DRS_ZENABLE,FALSE);d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
 d->SetRenderState(D3DRS_LIGHTING,FALSE);d->SetRenderState(D3DRS_FOGENABLE,FALSE);d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
 d->SetRenderState(D3DRS_STENCILENABLE,FALSE);d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
 d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
 d->SetRenderState(D3DRS_COLORWRITEENABLE,15);
}
inline void DrawWindDebug(IDirect3DDevice9* d,const renderer::WindState& wind,const std::vector<std::string>& extra={}){
 if(!d||(!wind.debugView&&extra.empty()))return;
 renderer::ScopedStateBlock state(d);
 if(FAILED(state.CaptureResult()))return;
 D3DVIEWPORT9 viewport{};
 if(FAILED(d->GetViewport(&viewport))||!viewport.Width||!viewport.Height)return;
 // Text HUD only. No world arrows/particles, no inputs intercepted and no
 // changes to the simulated state. It remains readable with F7 closed. The box
 // is sized by the rows it actually has, so an effect that adds a diagnostic row
 // gets the room for it instead of a line drawn over the panel below.
 const float content=126.f+22.f*float(extra.size());
 const float scale=std::min({1.f,float(viewport.Width)/620.f,float(viewport.Height)/(content+16.f)});
 const float width=600*scale,height=content*scale;
 const float x=float(viewport.X+viewport.Width)-width-8*scale,y=float(viewport.Y)+8*scale;
 auto lines=renderer::WindDebugLines(wind);
 std::vector<std::string> rows(lines.begin(),lines.end());
 rows.insert(rows.end(),extra.begin(),extra.end());
 const bool hasFont=windDebugFont.Ensure(d,std::max(6,int(std::round(16*scale))));
 std::vector<V> vertices;
 Rect(vertices,x,y,width,height,0xed101e28);
 if(!hasFont)for(size_t i=0;i<rows.size();++i)
  BitmapText(vertices,x+10*scale,y+(9+22*float(i))*scale,rows[i].c_str(),0xffdce5ea,1.2f*scale);
 SetupOverlayState(d);
 d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,UINT(vertices.size()/3),vertices.data(),sizeof(V));
 if(hasFont)for(size_t i=0;i<rows.size();++i)
  windDebugFont.Draw(d,x+10*scale,y+(8+22*float(i))*scale,rows[i],i?0xffdce5ea:0xff8effbb,x+width-8*scale);
}
inline void Draw(IDirect3DDevice9* d){
 if(!visible||!d)return;
 static std::map<std::string,HRESULT> lastErrors;
 auto check=[&](HRESULT result,const char* stage){auto& previous=lastErrors[stage];if(FAILED(result)&&result!=previous)renderer::menudiagnostics::Write(std::string("DRAW FAILED ")+stage+" HRESULT="+std::to_string(result));previous=result;return SUCCEEDED(result);};
 renderer::ScopedStateBlock state(d);
 if(!check(state.CaptureResult(),"CaptureState"))return;
 D3DVIEWPORT9 viewport{};d->GetViewport(&viewport);D3DDEVICE_CREATION_PARAMETERS creation{};d->GetCreationParameters(&creation);
 if(creation.hFocusWindow)gameWindow=creation.hFocusWindow;
 environmentLines=renderer::EnvironmentProfileManager::Instance().CompactDebugLines();
 renderWidth=int(viewport.Width);renderHeight=int(viewport.Height);
 Layout(viewport.Width,viewport.Height,creation.hFocusWindow);
 static int lastWidth=0,lastHeight=0;
 if(renderWidth!=lastWidth||renderHeight!=lastHeight){
  RECT client{};GetClientRect(creation.hFocusWindow,&client);
  renderer::menudiagnostics::Write("LAYOUT viewport="+std::to_string(renderWidth)+"x"+std::to_string(renderHeight)+" client="+std::to_string(client.right)+"x"+std::to_string(client.bottom)+" scale="+std::to_string(uiScale));
  lastWidth=renderWidth;lastHeight=renderHeight;
 }
 struct Label {float x,y,right;std::string text;DWORD color;};std::vector<Label> labels;std::vector<V> v;
 const auto panel=layout.panel;const float x=panel.x,y=panel.y,w=panel.w;
 auto label=[&](float xx,float yy,const std::string& text,DWORD color,float right){labels.push_back({xx*uiScale,yy*uiScale,right*uiScale,text,color});};
 auto box=[&](Box b,DWORD color){Rect(v,b.x*uiScale,b.y*uiScale,b.w*uiScale,b.h*uiScale,color);};
 box(panel,0xf5121820);box({x,y,w,32},0xff1b3340);
 label(x+12,y+6,"Modern WoW Renderer",0xff8effbb,x+w-310);
 label(x+w-295,y+6,"Drag title to move  /  F7 to close",0xffb5cbd6,x+w-42);
 box(layout.Close(),0xff263f4b);label(x+w-28,y+6,"X",0xffdce5ea,x+w-10);
 // Four lines, no more: the "Settings for:" scope row is laid out below them, so a fifth
 // environment line would be drawn under a widget. Anything the player has to be told
 // about a missing location therefore has to fit ON line 1, which is why
 // CompactDebugLines composes "Location unavailable: <cause>  |  Map/Zone/Area: <ids>"
 // rather than spreading the diagnosis over a line nobody renders.
 for(size_t i=0;i<environmentLines.size()&&i<4;++i)
  label(x+12,y+38+22*float(i),environmentLines[i],i?0xffb5cbd6:0xffffdc82,x+w-12);
 const auto& manager=renderer::EnvironmentProfileManager::Instance();
 const bool zone=renderer::locationtuning::editZone||!renderer::locationtuning::active.areaId;
 label(x+12,y+137,"Settings for:",0xffdce5ea,x+142);
 if(renderer::locationtuning::editable){
  for(int scope=0;scope<2;++scope){
   const auto b=layout.Scope(scope);const bool enabled=scope==0||renderer::locationtuning::active.areaId;
   box(b,enabled&&((scope==0)==zone)?0xff246b47:0xff26343b);
   const std::string text=scope==0?"Entire zone - "+manager.ZoneName():enabled?"This subarea - "+manager.AreaName():"Subarea unavailable (Area 0)";
   label(b.x+8,b.y+4,text,enabled?0xffdce5ea:0xff81919c,b.x+b.w-8);
  }
 }else{
  box({x+150,y+132,w-162,26},0xff246b47);
  label(x+158,y+137,"Entire game - GraphicsEffects.ini",0xffdce5ea,x+w-12);
 }
 label(x+12,y+168,!saveError.empty()?saveError:renderer::locationtuning::editable?
  "Subarea settings override zone settings. Image & color always applies globally.":
  "Location not verified: all edits apply globally. Changes save automatically.",
  saveError.empty()?0xffb5cbd6:0xffffaaaa,x+w-12);
 box({x+8,y+198,220,panel.h-234},0xff101e28);
 for(int tab=0;tab<int(Tabs().size());++tab){
  const auto b=layout.TabButton(tab);const bool selected=tab==layout.activeTab;
  if(selected){box(b,0xff234537);box({b.x,b.y,3,b.h},0xff45c985);}
  label(b.x+10,b.y+6,CompactLabel(items[Tabs()[tab].header]),selected?0xff9dffc0:0xffb5cbd6,b.x+b.w-6);
 }
 const auto content=layout.Content();const auto& tab=Tabs()[layout.activeTab];
 const auto resetButton=layout.ResetButton();
 label(content.x,y+206,CompactLabel(items[tab.header]),0xffffdc82,resetButton.x-8);
 box(resetButton,0xff263f4b);label(resetButton.x+8,resetButton.y+5,"RESET DEFAULTS",0xff9dffc0,resetButton.x+resetButton.w-6);
 const int first=tab.begin+layout.offsets[layout.activeTab];
 const int end=std::min(tab.end,first+layout.VisibleRows());
 for(int index=first;index<end;++index){
  const auto& item=items[index];const auto b=layout.Row(index);
  box({b.x,b.y,b.w,b.h-2},index==hover?0xff243a46:((index-first)%2?0xff16232d:0xff192933));
  if(item.kind==KIND_TOGGLE){
   const auto toggle=layout.Toggle(index);
   label(b.x+8,b.y+9,CompactLabel(item),0xffdce5ea,toggle.x-12);
   box(toggle,item.value?0xff246b47:0xff4a3232);
   label(toggle.x+14,toggle.y+4,item.value?"ON":"OFF",item.value?0xff9dffc0:0xffffaaaa,toggle.x+toggle.w-4);
  }else{
   const auto track=layout.Slider(index);
   const std::string full=item.label;const auto hint=full.find(" (");
   label(b.x+8,b.y+(hint==full.npos?9:1),CompactLabel(item),index==hover?0xffffffff:0xffdce5ea,track.x-62);
   if(hint!=full.npos)label(b.x+8,b.y+18,full.substr(hint+2,full.size()-hint-3),0xff819ca8,track.x-62);
   label(track.x-56,b.y+9,ValueLabel(item),0xffffdc82,track.x-6);
   box({track.x,track.y+7,track.w,6},0xff30424d);
   const float t=float(item.value-item.lo)/std::max(1,item.hi-item.lo);
   box({track.x,track.y+7,track.w*t,6},0xff45c985);
   box({track.x+track.w*t-3,track.y+2,6,16},0xff9dffc0);
  }
 }
 if(layout.MaxScroll(layout.activeTab)){
  box(layout.ScrollTrack(),0xff26343b);box(layout.ScrollThumb(),0xff668b91);
 }
 box({x,y+panel.h-34,w,1},0xff2a3d48);
 label(x+12,y+panel.h-25,"Drag corner to resize",0xff8effbb,x+220);
 label(content.x,y+panel.h-25,std::to_string(first-tab.begin+1)+"-"+std::to_string(end-tab.begin)+" / "+std::to_string(tab.end-tab.begin)+"   Scroll for more",0xffb5cbd6,x+w-108);
 for(bool down:{false,true}){
  const auto b=layout.ScrollButton(down);box(b,0xff26343b);
  label(b.x+8,b.y+2,down?"v":"^",0xffdce5ea,b.x+b.w-4);
 }
 // Large, dedicated corner grip; it never overlaps a setting or scroll button.
 const auto grip=layout.ResizeGrip();
 for(int n=0;n<3;++n)box({grip.x+7+float(n)*5,grip.y+18-float(n)*5,3,3},0xff8effbb);
 bool hasFont=font.Ensure(d,std::max(6,int(std::round(16*uiScale))));
 static bool fontFailed=false;if(!hasFont&&!fontFailed)renderer::menudiagnostics::Write("FONT FAILED: using bitmap fallback");fontFailed=!hasFont;
 if(!hasFont)for(const auto& l:labels){
  const float scale=1.6f*uiScale;
  const size_t count=size_t(std::max(0.f,(l.right-l.x)/(6*scale)));
  BitmapText(v,l.x,l.y,l.text.substr(0,count).c_str(),l.color,scale);
 }
 SetupOverlayState(d);check(d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,UINT(v.size()/3),v.data(),sizeof(V)),"DrawPrimitiveUP");
 if(hasFont)for(const auto& l:labels)font.Draw(d,l.x,l.y,l.text,l.color,l.right);
 check(state.Restore(),"RestoreState");
}
}
