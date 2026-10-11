#include "src/Environment/LocationTuning.h"
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <map>
#include "src/Environment/OverlayFont.h"
#include "src/Environment/EnvironmentProfileManager.h"
#include "src/Environment/MenuDiagnostics.h"

namespace tuningoverlay {
enum ItemKind { KIND_HEADER, KIND_TOGGLE, KIND_SLIDER };
struct Item { ItemKind kind; const wchar_t* section; const wchar_t* key; const char* label; int lo,hi,step,value; };
inline std::wstring ini;
inline bool visible=false,f7Down=false,mouseDown=false;
inline int hover=-1,dragItem=-1;
inline std::string saveError;
inline std::vector<Item> items={
 // === ATMOSPHERE & GOD RAYS ===
 {KIND_HEADER,nullptr,nullptr,"--- ATMOSPHERE & GOD RAYS ---",0,0,0,0},
 {KIND_TOGGLE,L"Atmosphere",L"ShaftsEnabled","SUN RAYS",0,1,1,1},
 {KIND_SLIDER,L"Atmosphere",L"ShaftPercent","RAY STRENGTH",0,400,5,160},
 {KIND_SLIDER,L"Atmosphere",L"SunGlowPercent","SUN GLOW",0,200,5,80},
 {KIND_TOGGLE,L"Atmosphere",L"FogEnabled","FOG",0,1,1,1},
 // Derives fog distance/colour from WoW's own authored values for the
 // current zone/shader (read-only capture) instead of one fixed profile
 // everywhere - FOG DISTANCE still scales it. Off = old fixed profile.
 {KIND_TOGGLE,L"Atmosphere",L"UseEnvironmentFog","WOW FOG BASELINE",0,1,1,0},
 {KIND_SLIDER,L"Atmosphere",L"AtmosphereQuality","ATMOSPHERE QUALITY",0,2,1,1},
 {KIND_SLIDER,L"Atmosphere",L"DensityPermille","FOG DENSITY",0,20,1,5},
 {KIND_SLIDER,L"Atmosphere",L"AtmosphereMaxDistance","FOG DISTANCE",200,1200,20,520},
 {KIND_SLIDER,L"Atmosphere",L"AtmosphereWashPercent","FOG WASH",0,100,2,55},
 // Separate near-ground world-space volume integrated by the atmosphere
 // raymarch. Movement opens a soft wake which gradually fills back in.
 {KIND_HEADER,nullptr,nullptr,"--- LOCAL VOLUMETRIC FOG ---",0,0,0,0},
 {KIND_TOGGLE,L"LocalFog",L"Enabled","LOCAL FOG",0,1,1,1},
 {KIND_SLIDER,L"LocalFog",L"DensityPermille","LOCAL DENSITY",0,100,2,12},
 {KIND_SLIDER,L"LocalFog",L"HeightUnits","LOCAL HEIGHT",1,8,1,3},
 {KIND_SLIDER,L"LocalFog",L"WakeStrengthPercent","PLAYER WAKE",0,150,5,85},
 {KIND_SLIDER,L"LocalFog",L"WakeRadius","WAKE RADIUS",2,14,1,5},
 {KIND_SLIDER,L"LocalFog",L"TrailLength","WAKE TRAIL",4,40,2,18},
 // Rescales the GAME's OWN native distance fog (its real per-shader fog
 // constants, the same ones vanilla WoW uses to hide its own render/
 // streaming distance edge) - independent of the atmosphere controls
 // above. This is what actually guarantees the world edge stays covered
 // regardless of any other setting here.
 {KIND_TOGGLE,L"DistanceFog",L"Enabled","WORLD EDGE FOG",0,1,1,1},
 {KIND_SLIDER,L"DistanceFog",L"DistancePercent","EDGE FOG DISTANCE",10,300,5,130},
 {KIND_SLIDER,L"DistanceFog",L"PowerPercent","EDGE FOG POWER",10,300,5,30},

 // === WEATHER VISUALS ===
 {KIND_HEADER,nullptr,nullptr,"--- WEATHER VISUALS ---",0,0,0,0},
 {KIND_TOGGLE,L"WeatherVisuals",L"Enabled","WEATHER SYSTEM",0,1,1,1},
 {KIND_SLIDER,L"WeatherVisuals",L"Mode","WEATHER HINT (0=AUTO 1=RAIN 2=SNOW)",0,2,1,0},
 {KIND_SLIDER,L"WeatherVisuals",L"IntensityPercent","WEATHER STRENGTH",25,200,5,100},

 // Raymarched volumetric cloud deck. It shares this column with the
 // weather controls because a covered sky and active precipitation are
 // the same mood; the shape, scattering and multiple-scattering knobs
 // stay ini-only.
 {KIND_HEADER,nullptr,nullptr,"--- VOLUMETRIC CLOUDS ---",0,0,0,0},
 {KIND_TOGGLE,L"VolumetricClouds",L"Enabled","CLOUDS",0,1,1,1},
 {KIND_SLIDER,L"VolumetricClouds",L"CoveragePercent","CLOUD COVERAGE",0,100,2,50},
 {KIND_SLIDER,L"VolumetricClouds",L"DensityPercent","CLOUD DENSITY",10,400,5,100},
 {KIND_SLIDER,L"VolumetricClouds",L"CloudTypePercent","CLOUD TYPE (-100 FLAT 100 TOWERING)",-100,100,5,0},
 {KIND_SLIDER,L"VolumetricClouds",L"BaseAltitude","CLOUD HEIGHT",100,20000,50,850},
 {KIND_SLIDER,L"VolumetricClouds",L"Thickness","CLOUD THICKNESS",100,12000,50,2150},
 {KIND_SLIDER,L"VolumetricClouds",L"FeatureSize","CLOUD SIZE",600,40000,200,3000},
 {KIND_SLIDER,L"VolumetricClouds",L"WindSpeed","CLOUD WIND",0,400,5,26},
 {KIND_SLIDER,L"VolumetricClouds",L"SunIntensityPercent","CLOUD SUNLIGHT",0,400,5,130},
 {KIND_SLIDER,L"VolumetricClouds",L"AerialPerspectivePercent","CLOUD HAZE",0,100,5,60},
 {KIND_SLIDER,L"VolumetricClouds",L"GroundShadowPercent","CLOUD SHADOWS",0,100,5,55},
 {KIND_SLIDER,L"VolumetricClouds",L"Quality","CLOUD QUALITY (0 LOW 3 ULTRA)",0,3,1,2},

 // === WATER & REFLECTIONS ===
 {KIND_HEADER,nullptr,nullptr,"--- WATER & REFLECTIONS ---",0,0,0,0},
 {KIND_TOGGLE,L"Water",L"Enabled","WATER",0,1,1,1},
 {KIND_TOGGLE,L"Water",L"WavesEnabled","WAVES",0,1,1,1},
 {KIND_TOGGLE,L"Water",L"ReflectionsEnabled","REFLECTIONS",0,1,1,1},
 {KIND_SLIDER,L"Water",L"ReflectionPercent","SSR STRENGTH",0,150,2,80},
 {KIND_SLIDER,L"Water",L"EnvironmentPercent","SKY REFLECTION",0,150,2,45},
 {KIND_TOGGLE,L"Water",L"SunGlintEnabled","SUN MOON GLINT",0,1,1,1},
 {KIND_SLIDER,L"Water",L"SunGlintPercent","GLINT STRENGTH",0,500,5,160},
 {KIND_TOGGLE,L"Water",L"RefractionEnabled","REFRACTION",0,1,1,1},
 {KIND_SLIDER,L"Water",L"RefractionPercent","REFRACT AMOUNT",0,50,1,14},
 {KIND_SLIDER,L"Water",L"DepthAbsorptionPercent","WATER DEPTH",0,100,1,38},
 {KIND_TOGGLE,L"Water",L"FoamEnabled","FOAM",0,1,1,1},
 {KIND_SLIDER,L"Water",L"ShoreFoamPercent","FOAM AMOUNT",0,60,1,28},
 {KIND_SLIDER,L"Water",L"SpecularPercent","WATER SHINE",0,400,5,115},

 // === LOCAL LIGHTING ===
 {KIND_HEADER,nullptr,nullptr,"--- LOCAL LIGHTING ---",0,0,0,0},
 {KIND_TOGGLE,L"DynamicLighting",L"Enabled","DYNAMIC LIGHTS",0,1,1,1},
 {KIND_SLIDER,L"DynamicLighting",L"IntensityPercent","LIGHT INTENSITY",0,250,5,100},
 {KIND_SLIDER,L"DynamicLighting",L"RayPercent","LIGHT RAYS",0,200,5,75},
 {KIND_SLIDER,L"DynamicLighting",L"Quality","LIGHT QUALITY (0 LOW 1 BAL 2 HIGH)",0,2,1,1},
 {KIND_SLIDER,L"DynamicLighting",L"DebugMode","LIGHT DEBUG",0,4,1,0},

 // === DYNAMIC SHADOWS ===
 {KIND_HEADER,nullptr,nullptr,"--- DYNAMIC SHADOWS ---",0,0,0,0},
 {KIND_TOGGLE,L"NativeShadows",L"Enabled","NATIVE SHADOWS",0,1,1,1},
 {KIND_SLIDER,L"NativeShadows",L"SoftnessPercent","SHADOW SOFTNESS",50,220,5,145},
 // SHADOW STRENGTH: c12 (the darkening constant) is a `def`-declared shader
 // literal, so writing the constant register at runtime is a no-op - this
 // slider instead swaps in a byte-patched COPY of the shader (only the two
 // c12 floats changed, everything else byte-identical) for the 4 confirmed
 // receiver hashes, restoring the original shader after the draw.
 {KIND_SLIDER,L"NativeShadows",L"StrengthPercent","SHADOW STRENGTH",30,250,10,100},

 // === AI MATERIALS & RELIEF ===
 {KIND_HEADER,nullptr,nullptr,"--- AI MATERIALS & RELIEF ---",0,0,0,0},
 {KIND_TOGGLE,L"AIMaterials",L"Enabled","AI MATERIALS",0,1,1,1},
 {KIND_SLIDER,L"AIMaterials",L"NormalStrengthPercent","NORMAL BUMP",0,200,5,90},
 {KIND_SLIDER,L"AIMaterials",L"ParallaxDepthPercent","PARALLAX DEPTH",0,100,2,35},
 {KIND_SLIDER,L"AIMaterials",L"SelfShadowStrength","SELF SHADOWS",0,150,5,90},

 // === IMAGE & COLOR ===
 {KIND_HEADER,nullptr,nullptr,"--- IMAGE & COLOR (GLOBAL) ---",0,0,0,0},
 {KIND_TOGGLE,L"PostProcess",L"Enabled","POST PROCESS",0,1,1,0},
 {KIND_SLIDER,L"PostProcess",L"BrightnessPercent","BRIGHTNESS",-50,50,1,0},
 {KIND_SLIDER,L"PostProcess",L"ContrastPercent","CONTRAST",50,180,2,100},
 {KIND_SLIDER,L"PostProcess",L"GammaPercent","GAMMA",50,180,2,100},
 {KIND_SLIDER,L"PostProcess",L"SharpnessPercent","SHARPNESS",0,100,2,35}
};
inline const std::vector<int> defaults=[](){std::vector<int> result;for(const auto& i:items)result.push_back(i.value);return result;}();
inline void Configure(const std::wstring& base){ini=base+L"GraphicsEffects.ini";for(auto& i:items){if(i.kind==KIND_HEADER)continue;i.value=std::clamp(static_cast<int>(renderer::locationtuning::ReadEditorInt(i.section,i.key,defaults[&i-items.data()],ini.c_str())),i.lo,i.hi);}}
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
inline const std::array<unsigned char,7>& Glyph(char c){
 if(c>='a'&&c<='z')c=char(c-'a'+'A');
 static const std::array<unsigned char,7> blank{};
 static const std::array<unsigned char,7> dash={0,0,0,31,0,0,0};
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
 if(c=='-')return dash;if(c=='/')return slash;if(c=='&')return amp;return blank;
}
struct V {float x,y,z,w;DWORD color;};
inline void Rect(std::vector<V>& out,float x,float y,float w,float h,DWORD color){V a{x,y,0,1,color},b{x+w,y,0,1,color},c{x,y+h,0,1,color},d{x+w,y+h,0,1,color};out.insert(out.end(),{a,b,c,c,b,d});}
inline void BitmapText(std::vector<V>& out,float x,float y,const char* s,DWORD color,float scale=2.f){for(;*s;++s,x+=6*scale){if(*s==' '){continue;}auto& g=Glyph(*s);for(int yy=0;yy<7;++yy)for(int xx=0;xx<5;++xx)if(g[yy]&(16>>xx))Rect(out,x+xx*scale,y+yy*scale,scale,scale,color);}}

inline OverlayFont font;
inline float uiScale=1;
inline std::vector<std::string> environmentLines;
inline constexpr int columns=3;
inline constexpr float columnWidth=350,controlRow=24;
inline const std::array<int,4>& ColumnStarts(){
 static const std::array<int,4> starts=[](){
  std::array<int,4> result{0,0,0,int(items.size())};
  for(int i=0;i<int(items.size());++i)if(items[i].kind==KIND_HEADER){
   if(std::string(items[i].label)=="--- WEATHER VISUALS ---")result[1]=i;
   if(std::string(items[i].label)=="--- LOCAL LIGHTING ---")result[2]=i;
  }
  return result;
 }();return starts;
}
inline int ItemColumn(int index){const auto& starts=ColumnStarts();return index>=starts[2]?2:index>=starts[1]?1:0;}
inline int columnRows=1;
inline float panelHeight=0;
inline int renderWidth=0,renderHeight=0;
inline float PanelX(){return 24*uiScale;}
inline float PanelY(){return 24*uiScale;}
inline float ControlsY(){return PanelY()+196*uiScale;}
inline void Layout(int width,int height,HWND window){
 using GetDpi=UINT(WINAPI*)(HWND);
 auto getDpi=reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
 float dpi=getDpi&&window?getDpi(window)/96.f:1.f;
 const auto& starts=ColumnStarts();columnRows=std::max({starts[1]-starts[0],starts[2]-starts[1],starts[3]-starts[2]});
 const float logicalHeight=196+columnRows*controlRow+18;
 uiScale=std::clamp(std::max(height/1440.f,std::min(dpi,1.15f)),.85f,1.5f);
 uiScale=std::min(uiScale,std::min((width-48.f)/(columns*columnWidth),(height-48.f)/logicalHeight));
 uiScale=std::max(.5f,uiScale);
 panelHeight=logicalHeight*uiScale;
}
inline int HitItem(float px,float py){
 const float x=PanelX(),start=ControlsY();
 if(px<x||px>=x+columns*columnWidth*uiScale||py<start)return -1;
 int column=int((px-x)/(columnWidth*uiScale));
 int row=int((py-start)/(controlRow*uiScale));
 int index=ColumnStarts()[column]+row;
 return row>=0&&row<columnRows&&index<ColumnStarts()[column+1]&&items[index].kind!=KIND_HEADER?index:-1;
}
inline std::string CompactLabel(const Item& item){
 std::string result=item.label;
 if(item.kind==KIND_HEADER){if(result.starts_with("--- "))result.erase(0,4);if(result.ends_with(" ---"))result.resize(result.size()-4);}
 const auto hint=result.find(" (");if(hint!=result.npos)result.resize(hint);
 return result;
}
inline int HitScope(float px,float py){
 if(py<PanelY()+132*uiScale||py>=PanelY()+157*uiScale)return -1;
 const float local=(px-PanelX())/uiScale;
 if(local>=155&&local<585)return 0;
 if(local>=595&&local<1038)return 1;
 return -1;
}
inline bool SelectScope(int scope){
 if(!renderer::locationtuning::editable||scope<0||scope>1||(scope==1&&!renderer::locationtuning::active.areaId))return false;
 renderer::locationtuning::editZone=scope==0;dragItem=-1;
 for(auto& i:items)if(i.kind!=KIND_HEADER)i.value=std::clamp(static_cast<int>(renderer::locationtuning::ReadEditorInt(i.section,i.key,defaults[&i-items.data()],ini.c_str())),i.lo,i.hi);
 return true;
}
inline bool Update(){
 DWORD pid=0;HWND window=GetForegroundWindow();GetWindowThreadProcessId(window,&pid);bool focused=pid==GetCurrentProcessId();
 bool f7=(GetAsyncKeyState(VK_F7)&0x8000)!=0;if(focused&&f7&&!f7Down){visible=!visible;if(visible)renderer::EnvironmentProfileManager::Instance().WriteMenuReport("F7 opened");}f7Down=f7;
 if(!visible||!focused){hover=-1;dragItem=-1;mouseDown=false;return false;}
 RECT client{};GetClientRect(window,&client);
 if(!renderWidth||!renderHeight)Layout(client.right,client.bottom,window);
 POINT p{};GetCursorPos(&p);ScreenToClient(window,&p);
 const float px=renderWidth&&client.right>0?float(p.x)*renderWidth/client.right:float(p.x);
 const float py=renderHeight&&client.bottom>0?float(p.y)*renderHeight/client.bottom:float(p.y);
 bool down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;bool changed=false;
 if(!down)dragItem=-1;
 if(down&&!mouseDown&&HitScope(px,py)>=0){SelectScope(HitScope(px,py));mouseDown=down;return false;}
 const int curHover=HitItem(px,py);
 hover=dragItem>=0?dragItem:curHover;
 const int editItem=dragItem>=0?dragItem:curHover;
 if(down&&!mouseDown&&editItem>=0&&!renderer::locationtuning::editable&&!renderer::locationtuning::IsGlobal(items[editItem].section))renderer::EnvironmentProfileManager::Instance().WriteMenuReport("blocked control click");
 if(down&&(renderer::locationtuning::editable||(editItem>=0&&renderer::locationtuning::IsGlobal(items[editItem].section)))){
  if(!mouseDown&&curHover>=0){auto& i=items[curHover];if(i.kind==KIND_TOGGLE){i.value=!i.value;changed=Save(i);}else dragItem=curHover;}
  if(dragItem>=0){
   auto& i=items[dragItem];const float left=PanelX()+ItemColumn(dragItem)*columnWidth*uiScale;
   float t=std::clamp((px-left-252*uiScale)/(82*uiScale),0.f,1.f);
   int value=std::clamp(i.lo+int((i.hi-i.lo)*t/i.step+.5f)*i.step,i.lo,i.hi);
   if(value!=i.value){i.value=value;changed=Save(i)||changed;if(!changed)dragItem=-1;}
  }
 }
 mouseDown=down;return changed;
}
inline void Draw(IDirect3DDevice9* d){
 if(!visible||!d)return;
 static std::map<std::string,HRESULT> lastErrors;
 auto check=[&](HRESULT result,const char* stage){auto& previous=lastErrors[stage];if(FAILED(result)&&result!=previous)renderer::menudiagnostics::Write(std::string("DRAW FAILED ")+stage+" HRESULT="+std::to_string(result));previous=result;return SUCCEEDED(result);};
 IDirect3DStateBlock9* raw=nullptr;if(!check(d->CreateStateBlock(D3DSBT_ALL,&raw),"CreateStateBlock"))return;if(!check(raw->Capture(),"Capture")){raw->Release();return;}
 D3DVIEWPORT9 viewport{};d->GetViewport(&viewport);D3DDEVICE_CREATION_PARAMETERS creation{};d->GetCreationParameters(&creation);
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
 float x=PanelX(),y=PanelY(),w=columns*columnWidth*uiScale,row=controlRow*uiScale;
 auto label=[&](float xx,float yy,const std::string& text,DWORD color,float right){labels.push_back({xx,yy,right,text,color});};
 Rect(v,x,y,w,panelHeight,0xf5121820);Rect(v,x,y,w,30*uiScale,0xff1b3340);
 label(x+12*uiScale,y+5*uiScale,"Modern WoW Renderer",0xff8effbb,x+w);
 label(x+w-220*uiScale,y+5*uiScale,renderer::locationtuning::editable?"Local preset / F7 close":"Local locked / F7 close",0xffb5cbd6,x+w-12*uiScale);
 for(size_t i=0;i<environmentLines.size();++i)label(x+12*uiScale,y+(36+23*i)*uiScale,environmentLines[i],i?0xffdce5ea:0xffffdc82,x+w-12*uiScale);
 const auto& manager=renderer::EnvironmentProfileManager::Instance();
 const bool zone=renderer::locationtuning::editZone||!renderer::locationtuning::active.areaId;
 label(x+12*uiScale,y+135*uiScale,"Settings for:",0xffdce5ea,x+150*uiScale);
 Rect(v,x+155*uiScale,y+132*uiScale,430*uiScale,25*uiScale,zone?0xff246b47:0xff26343b);
 Rect(v,x+595*uiScale,y+132*uiScale,443*uiScale,25*uiScale,!zone?0xff246b47:0xff26343b);
 label(x+162*uiScale,y+135*uiScale,"Entire zone - "+manager.ZoneName(),0xffdce5ea,x+578*uiScale);
 label(x+602*uiScale,y+135*uiScale,renderer::locationtuning::active.areaId?"This subarea - "+manager.AreaName():"Subarea unavailable (Area 0)",0xffdce5ea,x+w-12*uiScale);
 label(x+12*uiScale,y+165*uiScale,!saveError.empty()?saveError:renderer::locationtuning::editable?"Subarea settings override zone settings.":"Local locked: "+manager.TuningBlockedReason()+" | Send MenuDiagnostics.log",0xffb5cbd6,x+w-12*uiScale);
 for(int column=0;column<columns;++column){
  float xx=x+column*columnWidth*uiScale;
  if(column)Rect(v,xx-2*uiScale,ControlsY(),uiScale,panelHeight-196*uiScale,0xff2a3d48);
  for(int n=0;n<columnRows;++n){
   const int index=ColumnStarts()[column]+n;if(index>=ColumnStarts()[column+1])break;
   auto& i=items[index];float yy=ControlsY()+n*row;
   if(i.kind==KIND_HEADER){Rect(v,xx,yy,columnWidth*uiScale,row-1,0xff162836);label(xx+12*uiScale,yy+3*uiScale,CompactLabel(i),0xffffdc82,xx+(columnWidth-8)*uiScale);}
   else{
    label(xx+12*uiScale,yy+3*uiScale,CompactLabel(i),index==hover?0xffffffff:0xffdce5ea,xx+210*uiScale);
    if(i.kind==KIND_TOGGLE){Rect(v,xx+276*uiScale,yy+3*uiScale,58*uiScale,18*uiScale,i.value?0xff246b47:0xff4a3232);label(xx+289*uiScale,yy+3*uiScale,i.value?"ON":"OFF",i.value?0xff9dffc0:0xffffaaaa,xx+340*uiScale);}
    else{Rect(v,xx+252*uiScale,yy+8*uiScale,82*uiScale,9*uiScale,0xff26343b);float t=float(i.value-i.lo)/float(std::max(1,i.hi-i.lo));Rect(v,xx+252*uiScale,yy+8*uiScale,82*t*uiScale,9*uiScale,0xff45c985);label(xx+212*uiScale,yy+3*uiScale,std::to_string(i.value),0xffffdc82,xx+248*uiScale);}
   }
  }
 }
 bool hasFont=font.Ensure(d,std::max(12,int(std::round(16*uiScale))));
 static bool fontFailed=false;if(!hasFont&&!fontFailed)renderer::menudiagnostics::Write("FONT FAILED: using bitmap fallback");fontFailed=!hasFont;
 if(!hasFont)for(const auto& l:labels)BitmapText(v,l.x,l.y,l.text.c_str(),l.color,1.6f*uiScale);
 d->SetVertexShader(nullptr);d->SetPixelShader(nullptr);d->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);d->SetTexture(0,nullptr);
 d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
 d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
 d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);d->SetRenderState(D3DRS_ZENABLE,FALSE);d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
 d->SetRenderState(D3DRS_LIGHTING,FALSE);d->SetRenderState(D3DRS_FOGENABLE,FALSE);d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
 d->SetRenderState(D3DRS_STENCILENABLE,FALSE);d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
 d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
 d->SetRenderState(D3DRS_COLORWRITEENABLE,15);check(d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,UINT(v.size()/3),v.data(),sizeof(V)),"DrawPrimitiveUP");
 if(hasFont)for(const auto& l:labels)font.Draw(d,l.x,l.y,l.text,l.color,l.right);
 check(raw->Apply(),"RestoreState");raw->Release();
}
}
