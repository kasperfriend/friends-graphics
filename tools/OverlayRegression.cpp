#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "../TuningOverlay.h"
using Microsoft::WRL::ComPtr;
void Check(HRESULT h) {if(FAILED(h))throw std::runtime_error("D3D9 error "+std::to_string(h));}
int main() try {
 wchar_t path[MAX_PATH]{};GetSystemDirectoryW(path,MAX_PATH);wcscat_s(path,L"\\d3d9.dll");auto dll=LoadLibraryW(path);
 auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(dll,"Direct3DCreate9"));ComPtr<IDirect3D9> api;api.Attach(create(D3D_SDK_VERSION));
 HWND window=CreateWindowW(L"STATIC",L"Overlay regression",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 std::filesystem::create_directories("build/overlay-preview");
 renderer::menudiagnostics::Configure(L"build\\overlay-preview\\");
 renderer::locationtuning::Sync(L"build\\overlay-preview\\missing\\",{571,65,4164,true},true);
 auto& failedItem=tuningoverlay::items[1];const int originalValue=failedItem.value;failedItem.value=!originalValue;
 if(tuningoverlay::Save(failedItem))throw std::runtime_error("Save succeeded into missing directory");
 std::ifstream diagnostic("build/overlay-preview/MenuDiagnostics.log");std::string diagnosticText((std::istreambuf_iterator<char>(diagnostic)),{});
 if(diagnosticText.find("SAVE FAILED")==std::string::npos||diagnosticText.find("win32=")==std::string::npos)throw std::runtime_error("Save failure missing from support log");
 // Unverified client (a stock 3.3.5 build): the menu must still work, as
 // global GraphicsEffects.ini edits - not as rows that highlight under
 // the cursor and snap back. The toggle has to round-trip through the
 // global file and must not invent a per-location file.
 {
  std::filesystem::create_directories("build/overlay-preview/global-fallback");
  renderer::locationtuning::Sync(L"build\\overlay-preview\\global-fallback\\",{571,65,4164,true},false);
  if(renderer::locationtuning::editable)throw std::runtime_error("Unverified sync unexpectedly editable");
  auto& item=tuningoverlay::items[1];
  item.value=0;
  if(!tuningoverlay::Save(item))throw std::runtime_error("Global fallback toggle save failed");
  const auto globalFile=L"build\\overlay-preview\\global-fallback\\GraphicsEffects.ini";
  if(GetPrivateProfileIntW(L"Atmosphere",L"ShaftsEnabled",-1,globalFile)!=0)throw std::runtime_error("Global fallback did not write the global section");
  item.value=1;
  if(!tuningoverlay::Save(item))throw std::runtime_error("Global fallback toggle re-save failed");
  if(GetPrivateProfileIntW(L"Atmosphere",L"ShaftsEnabled",-1,globalFile)!=1)throw std::runtime_error("Global fallback toggle did not change");
  if(renderer::locationtuning::ReadEditorInt(L"Atmosphere",L"ShaftsEnabled",0,globalFile)!=1)throw std::runtime_error("Global fallback value does not read back");
  if(std::filesystem::exists("build/overlay-preview/global-fallback/LocationGraphics.ini"))throw std::runtime_error("Global fallback invented a location file");
  bool resolutionFound=false;
  for(auto& control:tuningoverlay::items)if(control.key&&wcscmp(control.key,L"ResolutionPercent")==0&&
      control.section&&wcscmp(control.section,L"VolumetricClouds")==0){
   control.value=50;
   if(!tuningoverlay::Save(control)||renderer::locationtuning::ReadEditorInt(L"VolumetricClouds",L"ResolutionPercent",0,globalFile)!=50)
    throw std::runtime_error("Cloud resolution slider did not round-trip");
   resolutionFound=true;
  }
  if(!resolutionFound)throw std::runtime_error("Cloud resolution slider missing");
  tuningoverlay::Configure(L"build\\overlay-preview\\global-fallback\\");
  int waterTab=-1;
  for(int tab=0;tab<int(tuningoverlay::Tabs().size());++tab)
   if(tuningoverlay::CompactLabel(tuningoverlay::items[tuningoverlay::Tabs()[tab].header])=="WATER & REFLECTIONS")waterTab=tab;
  if(waterTab<0)throw std::runtime_error("Water tab missing");
  const auto waterRows=tuningoverlay::Tabs()[waterTab];
  auto& unrelated=tuningoverlay::items[1];unrelated.value=0;
  if(!tuningoverlay::Save(unrelated))throw std::runtime_error("Could not prepare unrelated tab setting");
  for(int index=waterRows.begin;index<waterRows.end;++index){
   auto& control=tuningoverlay::items[index];
   control.value=tuningoverlay::defaults[index]==control.lo?control.hi:control.lo;
   if(!tuningoverlay::Save(control))throw std::runtime_error("Could not prepare nondefault water tab value");
  }
  tuningoverlay::layout.SelectTab(waterTab);
  if(!tuningoverlay::ResetActiveTab())throw std::runtime_error("Reset current tab reported no changed values");
  const auto resetFile=L"build\\overlay-preview\\global-fallback\\GraphicsEffects.ini";
  for(int index=waterRows.begin;index<waterRows.end;++index){
   const auto& control=tuningoverlay::items[index];
   if(control.value!=tuningoverlay::defaults[index]||
      renderer::locationtuning::ReadEditorInt(control.section,control.key,-1,resetFile)!=tuningoverlay::defaults[index])
    throw std::runtime_error("Reset current tab did not restore and persist every default");
  }
  if(unrelated.value!=0||renderer::locationtuning::ReadEditorInt(unrelated.section,unrelated.key,1,resetFile)!=0)
   throw std::runtime_error("Per-tab reset changed a setting outside the selected tab");
  std::cout<<"PASS overlay global fallback, cloud resolution and per-tab reset round-trip\n";
 }
 // While the menu is visible the game window must not receive the clicks
 // aimed at it (they would click the world / orbit the camera mid-drag).
 {
  tuningoverlay::visible=true;tuningoverlay::InstallInputHook(window);
  if(!tuningoverlay::inputWindow)throw std::runtime_error("Input hook was not installed");
  if(SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,0)!=0)throw std::runtime_error("Menu click leaked to the game window");
  if(SendMessageW(window,WM_MOUSEMOVE,0,0)!=0)throw std::runtime_error("Menu mouse move leaked to the game window");
  tuningoverlay::ReleaseInputHook();
  if(tuningoverlay::inputWindow)throw std::runtime_error("Input hook was not released");
  tuningoverlay::visible=false;
  std::cout<<"PASS overlay input hook swallows mouse while visible\n";
 }
 for(auto height:{1080,1440,2160}) {
  int width=height*16/9;D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;pp.BackBufferWidth=width;pp.BackBufferHeight=height;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
  ComPtr<IDirect3DDevice9> device;Check(api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,device.GetAddressOf()));
  tuningoverlay::visible=true;
  renderer::locationtuning::Sync(L"",{571,65,4164,true},true);
  if(!tuningoverlay::SelectScope(0)||!renderer::locationtuning::editZone)throw std::runtime_error("Cannot select zone editor");
  if(!tuningoverlay::SelectScope(1)||renderer::locationtuning::editZone)throw std::runtime_error("Cannot select subarea editor");
  renderer::locationtuning::Sync(L"",{571,65,0,true},true);
  if(tuningoverlay::SelectScope(1))throw std::runtime_error("Area zero allowed subarea edit");
  for(int page=0;page<1;++page) {
   Check(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff354052,1,0));device->SetRenderState(D3DRS_CULLMODE,D3DCULL_CW);Check(device->BeginScene());tuningoverlay::Draw(device.Get());
   if(tuningoverlay::PanelY()+tuningoverlay::panelHeight>height)throw std::runtime_error("Panel clipped vertically");
   if(tuningoverlay::HitScope(tuningoverlay::PanelX()+200*tuningoverlay::uiScale,tuningoverlay::PanelY()+140*tuningoverlay::uiScale)!=0||tuningoverlay::HitScope(tuningoverlay::PanelX()+700*tuningoverlay::uiScale,tuningoverlay::PanelY()+140*tuningoverlay::uiScale)!=1)throw std::runtime_error("Scope buttons cannot be reached");
   for(int index=0;index<int(tuningoverlay::items.size());++index)if(tuningoverlay::items[index].kind!=tuningoverlay::KIND_HEADER){
    int column=tuningoverlay::ItemColumn(index),row=index-tuningoverlay::ColumnStarts()[column];
    float px=tuningoverlay::PanelX()+(column*tuningoverlay::columnWidth+40)*tuningoverlay::uiScale;
    float py=tuningoverlay::ControlsY()+(row+.5f)*tuningoverlay::controlRow*tuningoverlay::uiScale;
    if(tuningoverlay::HitItem(px,py)!=index)throw std::runtime_error("Control cannot be reached by mouse");
   }
   DWORD restored=0;device->GetRenderState(D3DRS_CULLMODE,&restored);if(restored!=D3DCULL_CW)throw std::runtime_error("Overlay leaks state");
   device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);device->SetRenderState(D3DRS_LIGHTING,FALSE);device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
   tuningoverlay::font.Draw(device.Get(),1120*tuningoverlay::uiScale,80*tuningoverlay::uiScale,"1234567890  0 O  1 I l  5 S  8 B",0xffffffff,float(width));
   tuningoverlay::font.Draw(device.Get(),1120*tuningoverlay::uiScale,110*tuningoverlay::uiScale,"MapID ZoneID AreaID Fog Density",0xffffffff,float(width));
   tuningoverlay::font.Draw(device.Get(),1120*tuningoverlay::uiScale,140*tuningoverlay::uiScale,"\xd0\x9b\xd0\xbe\xd0\xba\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f / Environment",0xffffffff,float(width));
   Check(device->EndScene());DWORD cull;device->GetRenderState(D3DRS_CULLMODE,&cull);if(cull!=D3DCULL_NONE)throw std::runtime_error("Unexpected sample state");
   ComPtr<IDirect3DSurface9> back,read;Check(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,back.GetAddressOf()));Check(device->CreateOffscreenPlainSurface(width,height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,read.GetAddressOf(),nullptr));Check(device->GetRenderTargetData(back.Get(),read.Get()));
   D3DLOCKED_RECT locked{};Check(read->LockRect(&locked,nullptr,D3DLOCK_READONLY));std::ofstream output("build/overlay-preview/"+std::to_string(height)+"-"+std::to_string(page)+".ppm",std::ios::binary);output<<"P6\n"<<width<<' '<<height<<"\n255\n";
   for(int y=0;y<height;++y)for(int x=0;x<width;++x){DWORD pixel=reinterpret_cast<DWORD*>(static_cast<char*>(locked.pBits)+y*locked.Pitch)[x];char rgb[]={char(pixel>>16),char(pixel>>8),char(pixel)};output.write(rgb,3);}read->UnlockRect();
  }
  tuningoverlay::font.Reset();Check(device->Reset(&pp));if(!tuningoverlay::font.Ensure(device.Get(),16))throw std::runtime_error("Font atlas reset failed");tuningoverlay::font.Reset();
  std::cout<<"PASS overlay "<<width<<'x'<<height<<" all controls, state restoration, font reset\n";
 }
 DestroyWindow(window);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
