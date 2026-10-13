#include "../VolumeEffects.h"
#include "../src/Effects/ImagePostProcess.h"
// Exercises the shipped atmosphere/local-light classes on the real D3D9 device.
// Synthetic depth is encoded with WoW's viewport range, not just 0..1.
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <wrl/client.h>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#include <vector>
#include "../src/D3D9/TrackedRenderState.h"
#include "../src/Effects/DirectionalVolumetricLighting.h"
#include "../src/Effects/LocalLightingRenderer.h"
#include "../src/Effects/VolumetricClouds.h"
#include "../src/Effects/GroundSurfaceCapture.h"
#include "../src/Lighting/LocalLightManager.h"
#include "../src/D3D9/DepthCapture.h"
#include "../src/D3D9/DepthCapturePolicy.h"
#include "../WeatherVisuals.h"
using Microsoft::WRL::ComPtr;
using namespace renderer;
void Check(HRESULT h) { if (FAILED(h)) { printf("HRESULT=%08lx\n", h); throw std::runtime_error("D3D call"); } }
void Require(bool value, const char* name) { if (!value) throw std::runtime_error(name); printf("PASS %s\n", name); }
HRESULT WINAPI SetDepth(IDirect3DDevice9* d, IDirect3DSurface9* s) { return d->SetDepthStencilSurface(s); }
HRESULT WINAPI GetDepth(IDirect3DDevice9* d, IDirect3DSurface9** s) { return d->GetDepthStencilSurface(s); }
#include "LocalFogFieldRegression.h"
int main() try {
 wchar_t path[MAX_PATH]{}; GetSystemDirectoryW(path, MAX_PATH); wcscat_s(path,L"\\d3d9.dll");
 auto dll=LoadLibraryW(path);
 auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(dll,"Direct3DCreate9"));
 ComPtr<IDirect3D9> api; api.Attach(create(D3D_SDK_VERSION));
 HWND window=CreateWindowW(L"STATIC",L"Atmosphere regression",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
 pp.hDeviceWindow=window; pp.BackBufferWidth=128; pp.BackBufferHeight=128; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
 ComPtr<IDirect3DDevice9> d; Check(api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,d.GetAddressOf()));
 DepthCapture::Instance().SetFunctions(SetDepth,GetDepth);
 // A full explicit clear is equivalent to Clear(0,nullptr), while a partial
 // clear cannot safely initialize an otherwise undefined replacement buffer.
 D3DVIEWPORT9 policyViewport{10,20,100,80,0,1};
 D3DRECT policyFull{10,20,110,100},policyPartial{10,20,109,100};
 Require(ClearsFullViewport(0,nullptr,policyViewport),"implicit full clear policy");
 Require(ClearsFullViewport(1,&policyFull,policyViewport),"explicit full-viewport clear policy");
 Require(!ClearsFullViewport(1,&policyPartial,policyViewport),"partial clear rejected by policy");
 for(auto format:{D3DFMT_D16,D3DFMT_D32,D3DFMT_D24X8,D3DFMT_D24S8})
  Require(CanReplaceDepthFormat(format),"standard depth format policy");
 Require(!CanReplaceDepthFormat(D3DFMT_D16_LOCKABLE),"incompatible depth format rejected");
 // Exercise the production capture lifecycle on the two common 3.3.5a
 // formats, including the explicit rectangle used by affected clients.
 for(auto format:{D3DFMT_D16,D3DFMT_D24S8}){
  ComPtr<IDirect3DSurface9> originalDepth,currentDepth;
  Check(d->CreateDepthStencilSurface(128,128,format,D3DMULTISAMPLE_NONE,0,TRUE,originalDepth.GetAddressOf(),nullptr));
  Check(d->SetDepthStencilSurface(originalDepth.Get()));
  D3DRECT partial{0,0,64,64},full{0,0,128,128};
  Require(!DepthCapture::Instance().BeforeClear(d.Get(),1,&partial,D3DCLEAR_ZBUFFER,1.f),"partial clear does not start depth capture");
  Require(DepthCapture::Instance().BeforeClear(d.Get(),1,&full,D3DCLEAR_ZBUFFER,1.f),"explicit clear starts depth capture");
  Require(DepthCapture::Instance().HasDepth(),"replacement INTZ depth is active");
  Check(d->GetDepthStencilSurface(currentDepth.GetAddressOf()));Require(currentDepth.Get()==DepthCapture::Instance().GetDepthSurface(),"replacement depth is bound");
  currentDepth.Reset();DepthCapture::Instance().OnFrameEnd(d.Get());
  Check(d->GetDepthStencilSurface(currentDepth.GetAddressOf()));Require(currentDepth.Get()==originalDepth.Get(),"frame end restores original depth");
 }
 Check(d->SetDepthStencilSurface(nullptr));
 TestLocalFogField(d.Get());
 ComPtr<IDirect3DSurface9> back,read; Check(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,back.GetAddressOf()));
 Check(d->CreateOffscreenPlainSurface(128,128,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,read.GetAddressOf(),nullptr));
 auto texture=[&](D3DFORMAT fmt){ComPtr<IDirect3DTexture9> t; Check(d->CreateTexture(128,128,1,0,fmt,D3DPOOL_MANAGED,t.GetAddressOf(),nullptr)); return t;};
 auto depth=texture(D3DFMT_R32F), waterDepth=texture(D3DFMT_R32F);
 auto scene=texture(D3DFMT_A8R8G8B8),noise=texture(D3DFMT_A8R8G8B8),mask=texture(D3DFMT_A8R8G8B8);
 auto fill=[&](IDirect3DTexture9* t,DWORD value){D3DLOCKED_RECT r{};Check(t->LockRect(0,&r,nullptr,0));for(int y=0;y<128;++y)for(int x=0;x<128;++x)reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=value;Check(t->UnlockRect(0));};
 auto fillFloat=[&](IDirect3DTexture9* t,float v){DWORD bits;memcpy(&bits,&v,4);fill(t,bits);};
 fill(scene.Get(),0xff202020);fill(noise.Get(),0xffb0b0b0);fill(mask.Get(),0);fillFloat(waterDepth.Get(),20.f);
 ComPtr<IDirect3DSurface9> sceneSurface;Check(scene->GetSurfaceLevel(0,sceneSurface.GetAddressOf()));
 FrameContext f; f.cameraValid=f.depthAvailable=true;f.sceneColor=scene.Get();f.sceneSurface=sceneSurface.Get();
 f.depthTexture=depth.Get();f.atmosphereNoise=noise.Get();f.waterMaskTexture=mask.Get();f.waterDepthTexture=waterDepth.Get();
 f.projUnpack[0]=1000.f/999.9f;f.projUnpack[1]=-100.f/999.9f;f.projUnpack[2]=f.projUnpack[3]=1;
 f.inverseView.SetZero();f.inverseView.m[0][0]=f.inverseView.m[1][2]=f.inverseView.m[2][1]=f.inverseView.m[3][3]=1;
 f.sunDirectionView={0,0,1};f.viewport={0,0,128,128,0,1};f.daylightFactor=1;
 auto& fog=DirectionalVolumetricLighting::Instance();auto& settings=fog.Settings();
 settings.temporalEnabled=false;settings.localFogEnabled=false;settings.useEnvironmentBaseline=false;
 auto encode=[&](float distance,float maxZ,float minZ=0){
  f.viewport.MinZ=minZ;f.viewport.MaxZ=maxZ;f.depthMaxZ=maxZ;
  D3DLOCKED_RECT r{};Check(depth->LockRect(0,&r,nullptr,0));
  for(int y=0;y<128;++y)for(int x=0;x<128;++x){
   // A real ground plane five units below the camera plus an end wall.
   float vertical=1-2*(y+.5f)/128;
   float z=vertical<0?std::min(distance,-5/vertical):distance;
   reinterpret_cast<float*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=minZ+(maxZ-minZ)*(f.projUnpack[0]+f.projUnpack[1]/z);
  }Check(depth->UnlockRect(0));
 };
 float sentinels[64][4];for(int c=0;c<64;++c)for(int j=0;j<4;++j)sentinels[c][j]=float(c*4+j)*.003f;
 auto pixel=[&](int px=64,int py=64){Check(d->GetRenderTargetData(back.Get(),read.Get()));D3DLOCKED_RECT r{};Check(read->LockRect(&r,nullptr,D3DLOCK_READONLY));DWORD p=reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+py*r.Pitch)[px];Check(read->UnlockRect());return p;};
 // The continuity layer is injected before a confirmed UI draw, so it cannot
 // disappear under F7 and cannot paint over WoW's interface.
 weathervisuals::Configure(L".\\");weathervisuals::enabled=weathervisuals::effectEnabled=weathervisuals::active=true;
 weathervisuals::mode=0;weathervisuals::nativeWeatherMode=1;weathervisuals::intensity=1;weathervisuals::lensDroplets=false;weathervisuals::composedThisFrame=false;
 g_trackedState.vsHash=0xd9e7756460af6296ull;g_trackedState.psHash=0;
 Check(d->SetRenderTarget(0,back.Get()));Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff202020,1,0));
 Check(d->BeginScene());weathervisuals::BeforeDraw(d.Get());Check(d->EndScene());
 Check(d->GetRenderTargetData(back.Get(),read.Get()));D3DLOCKED_RECT weatherRead{};Check(read->LockRect(&weatherRead,nullptr,D3DLOCK_READONLY));int weatherPixels=0;
 for(int y=0;y<128;++y)for(int x=0;x<128;++x)weatherPixels+=reinterpret_cast<DWORD*>(static_cast<BYTE*>(weatherRead.pBits)+y*weatherRead.Pitch)[x]!=0xff202020;
 Check(read->UnlockRect());Require(weathervisuals::composedThisFrame&&weatherPixels>100,"rain continuity renders before UI independently of native particle radius");
 weathervisuals::lastNativeWeatherTick=GetTickCount()-weathervisuals::nativeWeatherHoldMs-1;weathervisuals::Present(nullptr,true);
 Require(weathervisuals::nativeWeatherMode==1&&weathervisuals::weatherSuspended,"F7 preserves detected weather across missing native draws");
 weathervisuals::Reset(d.Get());weathervisuals::nativeWeatherMode=0;weathervisuals::weatherSuspended=false;g_trackedState={};
 auto render=[&](bool global){Check(d->SetRenderTarget(0,back.Get()));Check(d->SetViewport(&f.viewport));Check(d->SetPixelShaderConstantF(0,sentinels[0],64));Check(d->SetTexture(4,scene.Get()));Check(d->SetSamplerState(4,D3DSAMP_ADDRESSU,D3DTADDRESS_MIRROR));Check(d->SetSamplerState(4,D3DSAMP_SRGBTEXTURE,TRUE));Check(d->SetTexture(5,scene.Get()));Check(d->SetSamplerState(5,D3DSAMP_ADDRESSU,D3DTADDRESS_MIRROR));Check(d->SetSamplerState(5,D3DSAMP_SRGBTEXTURE,TRUE));Check(d->SetTexture(6,scene.Get()));Check(d->SetSamplerState(6,D3DSAMP_ADDRESSU,D3DTADDRESS_MIRROR));Check(d->SetSamplerState(6,D3DSAMP_SRGBTEXTURE,TRUE));Check(d->SetTexture(7,scene.Get()));Check(d->SetSamplerState(7,D3DSAMP_ADDRESSU,D3DTADDRESS_MIRROR));Check(d->SetSamplerState(7,D3DSAMP_SRGBTEXTURE,TRUE));Check(d->BeginScene());bool ok=fog.Render(d.Get(),f,back.Get(),global);Check(d->EndScene());Require(ok,"production atmosphere render");float restored[64][4];Check(d->GetPixelShaderConstantF(0,restored[0],64));Require(!memcmp(sentinels,restored,sizeof(restored)),"c0..c63 restored");ComPtr<IDirect3DBaseTexture9> tex4;DWORD address4=0,srgb4=0;Check(d->GetTexture(4,tex4.GetAddressOf()));Check(d->GetSamplerState(4,D3DSAMP_ADDRESSU,&address4));Check(d->GetSamplerState(4,D3DSAMP_SRGBTEXTURE,&srgb4));Require(tex4.Get()==scene.Get()&&address4==D3DTADDRESS_MIRROR&&srgb4==TRUE,"raw-depth composite sampler restored");ComPtr<IDirect3DBaseTexture9> tex5;DWORD address5=0,srgb5=0;Check(d->GetTexture(5,tex5.GetAddressOf()));Check(d->GetSamplerState(5,D3DSAMP_ADDRESSU,&address5));Check(d->GetSamplerState(5,D3DSAMP_SRGBTEXTURE,&srgb5));Require(tex5.Get()==scene.Get()&&address5==D3DTADDRESS_MIRROR&&srgb5==TRUE,"field sampler restored");ComPtr<IDirect3DBaseTexture9> tex6;DWORD address6=0,srgb6=0;Check(d->GetTexture(6,tex6.GetAddressOf()));Check(d->GetSamplerState(6,D3DSAMP_ADDRESSU,&address6));Check(d->GetSamplerState(6,D3DSAMP_SRGBTEXTURE,&srgb6));Require(tex6.Get()==scene.Get()&&address6==D3DTADDRESS_MIRROR&&srgb6==TRUE,"cloud light sampler restored");ComPtr<IDirect3DBaseTexture9> tex7;DWORD address7=0,srgb7=0;Check(d->GetTexture(7,tex7.GetAddressOf()));Check(d->GetSamplerState(7,D3DSAMP_ADDRESSU,&address7));Check(d->GetSamplerState(7,D3DSAMP_SRGBTEXTURE,&srgb7));Require(tex7.Get()==scene.Get()&&address7==D3DTADDRESS_MIRROR&&srgb7==TRUE,"raw-depth sampler restored");D3DVIEWPORT9 restoredViewport{};Check(d->GetViewport(&restoredViewport));Require(!memcmp(&restoredViewport,&f.viewport,sizeof(f.viewport)),"atmosphere viewport restored");return pixel();};
 auto diff=[](DWORD a,DWORD b){int m=0;for(int s=0;s<24;s+=8)m=std::max(m,abs(int((a>>s)&255)-int((b>>s)&255)));return m;};
 encode(120,1);DWORD standard=render(true);Require(diff(standard,0xff202020)>8,"fog visible on first resource creation");
 const float baseDensity=settings.densityScale;
 f.environment.values[FogDensity]=0;
 Require(render(true)==0xff202020&&settings.densityScale==baseDensity,"environment fog multiplier affects GPU without mutating base settings");
 f.environment={};
 encode(120,.94f);DWORD compressed=render(true);printf("fog z=120 standard=%08lx compressed=%08lx\n",standard,compressed);Require(diff(standard,compressed)<=2,"viewport MaxZ=.94 matches MaxZ=1");
 encode(120,.94f,.1f);Require(diff(standard,render(true))<=2,"nonzero viewport MinZ");
 Require(render(false)==0xff202020,"both fog layers off preserve scene");
 settings.densityScale=0;Require(render(true)==0xff202020,"zero density removes global fog");settings.densityScale=1;
 settings.localFogEnabled=true;settings.localFogHeightFalloff=.125f;Require(diff(render(false),0xff202020)>5,"local fog has volume above ground, independent of global switch");
 settings.localFogHeightFalloff=1.f;DWORD thin=render(false);
 settings.localFogHeightFalloff=.125f;DWORD thick=render(false);
 Require(diff(thick,0xff202020)>diff(thin,0xff202020)+5,"increasing local height thickens the volume");
 settings.localFogHeightFalloff=1.f;Require(diff(render(false),0xff202020)<=2,"view above compact bank remains clear within Gaussian tail");
 settings.localFogBaseOffset=0;settings.localFogHeightFalloff=1.f/3;render(false);DWORD offsetZero=pixel(64,100);
 settings.localFogBaseOffset=-10;render(false);Require(diff(pixel(64,100),0xff202020)<=1&&diff(offsetZero,0xff202020)>5,"negative BaseOffset lowers the actual local layer");
 settings.localFogBaseOffset=4;DWORD elevated=render(false);settings.localFogBaseOffset=0;DWORD grounded=render(false);
 Require(diff(elevated,grounded)>5,"positive BaseOffset raises the local layer");settings.localFogBaseOffset=-1;
 settings.localFogHeightFalloff=.125f;fill(noise.Get(),0xff000000);
 Require(render(false)==0xff202020,"gaps between banks have zero local haze");fill(noise.Get(),0xffb0b0b0);
 // Reproduce the user's low-wash night scene, with no selected lamps.
 settings.localFogHeightFalloff=1.f/3;settings.localFogDensity=.3f;
 float savedWash=settings.fogWash;settings.fogWash=.18f;f.daylightFactor=0;
 render(false);DWORD nightBank=pixel(64,100);
 Require(diff(nightBank,0xff202020)>5,"height-three ground bank visible at night without lights");
 Require((nightBank&255)>((nightBank>>16)&255),"unlit bank follows blue environment rather than fixed white");
 settings.fogWash=0;render(false);
 Require(diff(nightBank,pixel(64,100))<=1,"local bank independent of global fog wash");
 settings.localFogDensity=.005f;settings.debugMode=static_cast<VolumetricDebugMode>(1);
 fill(noise.Get(),0xffb0b0b0);render(false);DWORD denseOptical=pixel(64,100);
 fill(noise.Get(),0xff888888);render(false);DWORD sparseOptical=pixel(64,100);
 Require((denseOptical&255)>(sparseOptical&255),"noise changes ground-bank optical density independently of colour");
 settings.localFogDensity=.3f;settings.debugMode=static_cast<VolumetricDebugMode>(0);
 fill(noise.Get(),0);render(false);Require(pixel(64,100)==0xff202020,"clear gaps remain clear at maximum local density");
 fill(noise.Get(),0xffb0b0b0);settings.fogWash=savedWash;f.daylightFactor=1;
 settings.localFogEnabled=false;
 settings.resolutionScale=.5f;DWORD resized=render(true);DWORD resizedAgain=render(true);
 printf("resized first=%08lx second=%08lx\n",resized,resizedAgain);
 Require(diff(resized,0xff202020)>8 && diff(resized,resizedAgain)<=1,"quality resize preserves first-frame fog");
 settings.debugMode=VolumetricDebugMode::BoundaryDepth;encode(120,.94f);DWORD boundary=render(true);
 fill(mask.Get(),0xffffffff);DWORD water=render(true);Require((boundary&255)>(water&255)+30,"water stops atmosphere at surface, not seabed");
 fill(mask.Get(),0);settings.debugMode=VolumetricDebugMode::None;
 settings.edgeFogEnabled=true;settings.edgeFogDistance=30;settings.edgeFogPower=2;
 encode(120,.94f);DWORD edgeFog=render(false);Require(diff(edgeFog,0xff202020)>25,"world edge fog works with global and local fog disabled");
 encode(20,.94f);Require(render(false)==0xff202020,"world edge fog preserves near surfaces");// Sky well above the horizon must retain its original colour.
 fillFloat(depth.Get(),1.f);render(false);Require(diff(pixel(64,0),0xff202020)<=1,"world edge fog leaves high sky clear");
 settings.edgeFogEnabled=false;
 // Fog above nearby water must survive the final composite, not just integration.
 settings.localFogEnabled=true;settings.localFogDensity=.3f;settings.localFogHeightFalloff=1.f/3;
 encode(120,.94f);render(false);DWORD dryBank=pixel(64,100);
 D3DLOCKED_RECT wr{};Check(waterDepth->LockRect(0,&wr,nullptr,0));
 for(int y=0;y<128;++y)for(int x=0;x<128;++x){float v=1-2*(y+.5f)/128;reinterpret_cast<float*>(static_cast<BYTE*>(wr.pBits)+y*wr.Pitch)[x]=v<0?std::min(120.f,-5/v):120.f;}
 Check(waterDepth->UnlockRect(0));fill(mask.Get(),0xffffffff);render(false);
 Require(diff(dryBank,pixel(64,100))<=2&&diff(pixel(64,100),0xff202020)>20,"nearby water retains air fog above its surface");
 fill(mask.Get(),0);settings.localFogEnabled=false;
 // Read back only in this regression executable; production state stays on GPU.
 auto interaction=[&](){std::array<float,8> result{};ComPtr<IDirect3DSurface9> src,staging;
  Check(fog.GetInteractionState()->GetSurfaceLevel(0,src.GetAddressOf()));
  Check(d->CreateOffscreenPlainSurface(2,1,D3DFMT_A32B32G32R32F,D3DPOOL_SYSTEMMEM,staging.GetAddressOf(),nullptr));
  Check(d->GetRenderTargetData(src.Get(),staging.Get()));D3DLOCKED_RECT r{};Check(staging->LockRect(&r,nullptr,D3DLOCK_READONLY));memcpy(result.data(),r.pBits,32);Check(staging->UnlockRect());return result;};
 auto actorDepth=[&](){encode(120,.94f);D3DLOCKED_RECT r{};Check(depth->LockRect(0,&r,nullptr,0));
  for(int y=54;y<82;++y)for(int x=61;x<68;++x)reinterpret_cast<float*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=.94f*(f.projUnpack[0]+f.projUnpack[1]/12.f);
  Check(depth->UnlockRect(0));};
 fog.Reset(d.Get());actorDepth();render(false);auto actorStart=interaction();
 Require(fabsf(actorStart[1]-12)<.2f,"wake anchors to central character silhouette, not camera");
 for(int i=0;i<20;++i){Sleep(16);f.cameraPosition.x+=.08f;actorDepth();render(false);}
 auto actorMoved=interaction();
 Require(actorMoved[0]>.2f&&actorMoved[3]>.03f&&actorMoved[4]>.3f,"moving actor generates persistent directional wake");
 Require(fabsf(actorMoved[2]-actorStart[2])<.02f,"flat world ground stays anchored during movement");
 f.frameIndex+=100000;render(false);auto unchanged=interaction();
 Require(fabsf(unchanged[2]-actorMoved[2])<.02f,"frame counter wrap cannot displace ground state");
 f.cameraPosition={0,0,0};fog.Reset(d.Get());encode(120,.94f);
 auto& manager=LocalLightManager::Instance();
 auto lamp=[&](float y){manager.Reset();D3DLIGHT9 l{};l.Type=D3DLIGHT_POINT;l.Position={0,y,0};l.Range=12;l.Diffuse={1,.65f,.25f,1};for(int n=0;n<8;++n)manager.OnSetLight(0,&l);manager.OnLightEnable(0,TRUE);manager.SelectForFrame({0,0,0},{0,1,0});Require(!manager.Selected().empty(),"test world light selected");};
 encode(120,.94f);DWORD noLight=render(true);lamp(40);DWORD glow=render(true);Require(diff(noLight,glow)>2,"small lamp between world march samples produces halo");
 encode(20,.94f);DWORD blocked=render(true);manager.Reset();Require(diff(blocked,render(true))==0,"foreground wall occludes lamp volume");

 // User's low Light Rays=15 must still show a visible halo, even with global
 // Fog Wash zero. The zero endpoint must remain a real off switch.
 std::filesystem::create_directories("build/halo-fixture");auto savedFog=settings;
 settings.localFogEnabled=true;settings.localFogDensity=.132f;settings.localFogHeightFalloff=1.f/7;settings.fogWash=0;f.daylightFactor=0;
 auto haloAt=[&](int rays){std::ofstream cfg("build/halo-fixture/GraphicsEffects.ini");cfg<<"[DynamicLighting]\nEnabled=1\nIntensityPercent=55\nRayPercent="<<rays<<"\nQuality=2\n";cfg.close();
  manager.Configure(L"build/halo-fixture/");lamp(40);encode(120,.94f);return render(false);};
 auto haloZero=haloAt(0),haloLow=haloAt(15);
 printf("lamp halo rays0=%08lx rays15=%08lx\n",haloZero,haloLow);
 Require(diff(haloZero,haloLow)>8,"Light Rays 15 produces visible lamp scattering at zero global wash");
 manager.Reset();Require(diff(haloZero,render(false))<=1,"Light Rays zero removes lamp scattering completely");
 settings=savedFog;f.daylightFactor=1;manager.Configure(L".\\");
 auto& lighting=LocalLightingRenderer::Instance();lamp(52);
 auto surface=[&](float maxZ){lamp(52);encode(60,maxZ);f.sceneColor=scene.Get();f.sceneSurface=sceneSurface.Get();Check(d->BeginScene());Require(lighting.Render(d.Get(),f),"production surface light render");Check(d->EndScene());Check(d->StretchRect(lighting.GetLitSurface(),nullptr,back.Get(),nullptr,D3DTEXF_NONE));return pixel();};
 DWORD light1=surface(1),light94=surface(.94f);printf("surface standard=%08lx compressed=%08lx\n",light1,light94);
 Require(diff(light1,0xff202020)>2,"lamp illuminates world surface");Require(diff(light1,light94)<=2,"surface light viewport depth invariant");
 auto unrelatedOccluder=[&](bool blocked){
  encode(60,.94f);manager.Reset();D3DLIGHT9 l{};l.Type=D3DLIGHT_POINT;l.Position={8,52,0};l.Range=24;l.Diffuse={2,1.3f,.5f,1};
  for(int n=0;n<8;++n)manager.OnSetLight(0,&l);manager.OnLightEnable(0,TRUE);
  if(blocked){D3DLOCKED_RECT r{};Check(depth->LockRect(0,&r,nullptr,0));
   for(int y=62;y<=66;++y)for(int x=69;x<=72;++x)
    reinterpret_cast<float*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=.94f*(f.projUnpack[0]+f.projUnpack[1]/10.f);
   Check(depth->UnlockRect(0));}
  f.sceneColor=scene.Get();f.sceneSurface=sceneSurface.Get();Check(d->BeginScene());
  Require(lighting.Render(d.Get(),f),"surface occlusion fixture render");Check(d->EndScene());
  Check(d->StretchRect(lighting.GetLitSurface(),nullptr,back.Get(),nullptr,D3DTEXF_NONE));return pixel();
 };
 DWORD unobstructed=unrelatedOccluder(false),unrelated=unrelatedOccluder(true);
 Require(diff(unobstructed,unrelated)<=2,"foreground silhouette does not extrude a shadow onto distant receiver");
 // Compare reduced-resolution light against an exact-resolution reference
 // on a grazing ground plane. There must be no rejected dark scanlines.
 std::filesystem::create_directories("build/light-slope-fixture");
 auto slopeLighting=[&](int quality){
  {std::ofstream cfg("build/light-slope-fixture/GraphicsEffects.ini");cfg<<"[DynamicLighting]\nEnabled=1\nIntensityPercent=100\nQuality="<<quality<<"\n";}
  manager.Configure(L"build/light-slope-fixture/");manager.Reset();encode(120,.94f);
  D3DLIGHT9 l{};l.Type=D3DLIGHT_POINT;l.Position={0,18,-3};l.Range=40;l.Diffuse={8,5,2,1};
  for(int i=0;i<8;++i)manager.OnSetLight(0,&l);manager.OnLightEnable(0,TRUE);
  f.sceneColor=scene.Get();f.sceneSurface=sceneSurface.Get();Check(d->BeginScene());Require(lighting.Render(d.Get(),f),"grazing plane lighting render");Check(d->EndScene());
  Check(d->StretchRect(lighting.GetLitSurface(),nullptr,back.Get(),nullptr,D3DTEXF_NONE));
  std::array<DWORD,24> line{};for(int y=76;y<100;++y)line[y-76]=pixel(64,y);return line;};
 auto exactSlope=slopeLighting(2),reducedSlope=slopeLighting(1);
 int maxDarkGap=0;
 // A lower-resolution sample may interpolate anywhere between neighbouring
 // reference pixels, especially around a saturated lamp core. A black stripe
 // falls below both neighbours and is not legitimate interpolation.
 for(int i=0;i<24;++i)for(int shift=0;shift<24;shift+=8){
  int low=255;for(int j=std::max(0,i-1);j<=std::min(23,i+1);++j)low=std::min(low,int((exactSlope[j]>>shift)&255));
  maxDarkGap=std::max(maxDarkGap,low-int((reducedSlope[i]>>shift)&255));}
 printf("grazing plane maximum dark gap=%d\n",maxDarkGap);
 Require(maxDarkGap<=6,"sloping light footprint has no black upsampling stripes");
 // Feed a real shipped flame mip and verified rigid M2 draw layout through
 // the exact attachment observer, including animated bone translation.
 ComPtr<IDirect3DTexture9> flame;Check(d->CreateTexture(128,128,1,0,D3DFMT_DXT5,D3DPOOL_MANAGED,flame.GetAddressOf(),nullptr));
 std::ifstream payload("tools/fixtures/flamelicksmall-mip.dxt5",std::ios::binary);Require(bool(payload),"real torch texture fixture available");
 D3DLOCKED_RECT flameLock{};Check(flame->LockRect(0,&flameLock,nullptr,0));
 for(int row=0;row<32;++row)payload.read(static_cast<char*>(flameLock.pBits)+row*flameLock.Pitch,512);
 Check(flame->UnlockRect(0));Require(bool(payload),"torch mip complete");
 struct TorchVertex{float x,y,z;BYTE bone[4];float u,v;};
 TorchVertex tv[]={{-.1f,0,0,{0},0,0},{.1f,0,0,{0},1,0},{0,.2f,0,{0},.5f,1}};
 ComPtr<IDirect3DVertexBuffer9> vb;ComPtr<IDirect3DIndexBuffer9> ib;void* memory=nullptr;
 Check(d->CreateVertexBuffer(sizeof(tv),0,0,D3DPOOL_MANAGED,vb.GetAddressOf(),nullptr));Check(vb->Lock(0,sizeof(tv),&memory,0));memcpy(memory,tv,sizeof(tv));Check(vb->Unlock());
 WORD ti[]={0,1,2};Check(d->CreateIndexBuffer(sizeof(ti),0,D3DFMT_INDEX16,D3DPOOL_MANAGED,ib.GetAddressOf(),nullptr));Check(ib->Lock(0,sizeof(ti),&memory,0));memcpy(memory,ti,sizeof(ti));Check(ib->Unlock());
 D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_UBYTE4,0,D3DDECLUSAGE_BLENDINDICES,0},{0,16,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
 ComPtr<IDirect3DVertexDeclaration9> decl;Check(d->CreateVertexDeclaration(elements,decl.GetAddressOf()));
 Check(d->SetVertexDeclaration(decl.Get()));Check(d->SetStreamSource(0,vb.Get(),0,sizeof(TorchVertex)));Check(d->SetIndices(ib.Get()));Check(d->SetTexture(0,flame.Get()));
 float boneRows[3][4]={{1,0,0,0},{0,1,0,0},{0,0,1,40}};Check(d->SetVertexShaderConstantF(31,boneRows[0],3));
 FrameContext::Current()=f;g_trackedState.vsHash=0xff32338728131932ull;g_trackedState.zEnable=g_trackedState.alphaBlend=true;
 manager.Reset();manager.ObserveTorchDraw(d.Get(),D3DPT_TRIANGLELIST,0,0,1);manager.SelectForFrame({0,0,0},{0,1,0});
 Require(!manager.Selected().empty()&&(manager.Selected()[0].flags&LocalLightAttached),"held torch mesh creates a dynamic world light");
 auto heldTorchId=manager.Selected()[0].stableId;Sleep(350);manager.SelectForFrame({0,0,0},{0,1,0});
 bool torchHeldAcrossGap=false;for(const auto& selected:manager.Selected())if(selected.stableId==heldTorchId&&selected.intensity>1.7f)torchHeldAcrossGap=true;
 Require(torchHeldAcrossGap,"attached torch survives a skipped or occluded particle draw");
 Vec3 torchPosition=manager.Selected()[0].position;boneRows[0][3]=1;Check(d->SetVertexShaderConstantF(31,boneRows[0],3));
 manager.ObserveTorchDraw(d.Get(),D3DPT_TRIANGLELIST,0,0,1);manager.SelectForFrame({0,0,0},{0,1,0});
 Require(manager.Selected().size()==1&&fabsf(manager.Selected()[0].position.x-torchPosition.x-1)<.01f,"torch light follows the animated hand without duplicates");
 // Classic torch flames use the unskinned c31..c33 particle path too,
 // and may be submitted as UP draws with no readable GPU vertex buffer.
 g_trackedState.vsHash=0x5f2c6b3af8c6e543ull;manager.Reset();
 manager.ObserveTorchDraw(d.Get(),D3DPT_TRIANGLELIST,0,0,1,false,tv,sizeof(TorchVertex));manager.SelectForFrame({0,0,0},{0,1,0});
 Require(manager.Selected().size()==1,"unskinned flame particle UP draw creates light");
 manager.Reset();manager.ObserveTorchDraw(d.Get(),D3DPT_TRIANGLELIST,0,0,1,true,tv,sizeof(TorchVertex),ti,D3DFMT_INDEX16,3);manager.SelectForFrame({0,0,0},{0,1,0});
 Require(manager.Selected().size()==1,"indexed UP flame particle draw creates light");
 manager.Reset();manager.ObserveTorchDraw(d.Get(),D3DPT_TRIANGLELIST,0,0,1,false);manager.SelectForFrame({0,0,0},{0,1,0});
 Require(manager.Selected().size()==1,"unskinned nonindexed buffer draw creates light");
 TorchVertex separated[6];memcpy(separated,tv,sizeof(tv));memcpy(separated+3,tv,sizeof(tv));for(int i=3;i<6;++i)separated[i].x+=8;
 manager.Reset();manager.ObserveTorchDraw(d.Get(),D3DPT_TRIANGLELIST,0,0,2,false,separated,sizeof(TorchVertex));manager.SelectForFrame({0,0,0},{0,1,0});
 Require(manager.Selected().size()==2,"batched flames remain separate sources, without a midpoint light");
 Check(d->SetTexture(0,scene.Get()));manager.Reset();manager.ObserveTorchDraw(d.Get(),D3DPT_TRIANGLELIST,0,0,1);manager.SelectForFrame({0,0,0},{0,1,0});Require(manager.Selected().empty(),"ordinary textures do not create attached lights");

 // Replay real geometry into the production height atlas: slopes, water,
 // camera rotation, and draw-state restoration are checked independently.
 auto& ground=GroundSurfaceCapture::Instance();ground.Reset();manager.Reset();
 f.cameraPosition={0,0,0};f.inverseView.SetZero();f.inverseView.m[0][0]=f.inverseView.m[1][2]=f.inverseView.m[2][1]=f.inverseView.m[3][3]=1;
 struct GroundVertex{float x,y,z;};
 GroundVertex hill[]={{-110,-110,-24.8f},{110,-110,-2.8f},{-110,110,-7.2f},{110,110,14.8f}};
 float groundView[4][4]={{1,0,0,0},{0,0,1,0},{0,1,0,0},{0,0,0,1}};
 auto captureGround=[&](GroundVertex* q){Check(d->SetFVF(D3DFVF_XYZ));Check(d->SetVertexShaderConstantF(0,groundView[0],4));
  Check(d->SetRenderTarget(0,back.Get()));Check(d->SetViewport(&f.viewport));Check(d->SetPixelShaderConstantF(0,sentinels[0],64));
  float before[9][4]{};Check(d->GetVertexShaderConstantF(0,before[0],9));Check(d->BeginScene());
  // Native shadowed terrain uses a PS absent from the old four-hash list.
  g_trackedState.vsHash=0x938d1ef758aa6085ull;g_trackedState.psHash=0x634793193e26059dull;g_trackedState.zEnable=true;
  ground.Observe(d.Get(),f,[&]{Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(GroundVertex)));});Check(d->EndScene());
  float after[9][4]{};Check(d->GetVertexShaderConstantF(0,after[0],9));Require(!memcmp(before,after,sizeof(before)),"height capture restores vertex constants");
  float ps[64][4]{};Check(d->GetPixelShaderConstantF(0,ps[0],64));Require(!memcmp(ps,sentinels,sizeof(ps)),"height capture restores pixel constants");
  D3DVIEWPORT9 vp{};Check(d->GetViewport(&vp));Require(!memcmp(&vp,&f.viewport,sizeof(vp)),"height capture restores viewport including depth range");};
 auto groundPixel=[&](float x,float y){
  Require(ground.Texture()!=nullptr,"height atlas is available");ComPtr<IDirect3DSurface9> a,b;
  Check(ground.Texture()->GetSurfaceLevel(0,a.GetAddressOf()));Check(d->CreateOffscreenPlainSurface(1024,1024,D3DFMT_A16B16G16R16F,D3DPOOL_SYSTEMMEM,b.GetAddressOf(),nullptr));
  Check(d->GetRenderTargetData(a.Get(),b.Get()));D3DLOCKED_RECT r{};Check(b->LockRect(&r,nullptr,D3DLOCK_READONLY));
  int ix=int((x-ground.Origin()[0]+256)*2),iy=int((y-ground.Origin()[1]+256)*2);
  auto h=reinterpret_cast<unsigned short*>(static_cast<BYTE*>(r.pBits)+iy*r.Pitch)+ix*4;
  auto half=[](unsigned short v){return std::ldexp(float((v&1023)+((v&0x7c00)?1024:0)),((v>>10)&31)?int((v>>10)&31)-25:-24)*((v&0x8000)?-1.f:1.f);};
  std::array<float,2> value{half(h[0])+ground.Origin()[2],half(h[1])};Check(b->UnlockRect());return value;};
 // Reproduce water arriving before supported terrain: it must not turn an
 // empty land atlas into authoritative coverage and erase all ground fog.
 Check(d->SetFVF(D3DFVF_XYZ));Check(d->SetVertexShaderConstantF(5,groundView[0],4));Check(d->BeginScene());
 ground.Capture(d.Get(),f,5,[&]{Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,hill,sizeof(GroundVertex)));});Check(d->EndScene());
 Require(!ground.Texture(),"water-only atlas cannot disable land fog fallback");
 for(auto h:{0x2437f34e978bb37full,0x26c1716cad7cb0abull,0x79a7faa602ba6b68ull,0x91f8b826abc842a1ull,0x938d1ef758aa6085ull,0xde5fb134242c2b4eull,0xf6d02568ca25d7caull})Require(GroundSurfaceCapture::IsTerrainVertexShader(h),"native shadow terrain vertex variant recognized");
 Require(!GroundSurfaceCapture::IsTerrainVertexShader(0x28d133339d7839aeull),"WMO vertices cannot become terrain height");
 captureGround(hill);auto gp=groundPixel(32,32);printf("atlas hill h=%f coverage=%f\n",gp[0],gp[1]);
 Require(fabsf(gp[0]-(-5+.1f*32.25f+.08f*32.25f))<.025f&&gp[1]>.99f,"captured height follows sloping terrain at world XY");

 f.sceneColor=scene.Get();f.sceneSurface=sceneSurface.Get();
 settings.localFogEnabled=true;settings.localFogDensity=.1f;settings.localFogHeightFalloff=1.f/3;
 settings.localFogWakeStrength=0;settings.edgeFogEnabled=false;settings.resolutionScale=1;settings.sampleCount=24;
 fill(noise.Get(),0xffb0b0b0);fill(mask.Get(),0);f.daylightFactor=1;
 auto slopingDepth=[&](){D3DLOCKED_RECT r{};Check(depth->LockRect(0,&r,nullptr,0));
  for(int y=0;y<128;++y)for(int x=0;x<128;++x){float vx=2*(x+.5f)/128-1,vy=1-2*(y+.5f)/128;
   float denominator=.1f*vx+.08f-vy;float z=denominator>0?std::min(120.f,5/denominator):120.f;
   reinterpret_cast<float*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=.94f*(f.projUnpack[0]+f.projUnpack[1]/z);}
  Check(depth->UnlockRect(0));};
 auto smooth=[](float a,float b,float v){float q=std::clamp((v-a)/(b-a),0.f,1.f);return q*q*(3-2*q);};
 auto referenceBank=[&](int x,int y){
  double vx=2*(x+.5)/128-1,vy=1-2*(y+.5)/128,scale=sqrt(vx*vx+vy*vy+1);
  double denominator=.1*vx+.08-vy,z=denominator>0?std::min(120.,5/denominator):120.;
  double length=std::min(z*scale,97.5),tau=0,step=length/8192;
  double n=176./255,shape=smooth(.28f,.72f,float(n));
  auto fieldSlice=[&](double h){double warp=(n-.5)*.3;
   double low=shape*exp(-2*(h-warp)*(h-warp)/(.65*.65));
   double mid=shape*exp(-2*(h-.48-warp)*(h-.48-warp)/(.62*.62));
   double high=shape*exp(-2*(h-.95-warp)*(h-.95-warp)/(.58*.58));
   return 1-exp(-(low*.85+mid*.65+high*.4)*1.25);};
  for(int i=0;i<8192;++i){double t=(i+.5)*step,px=vx/scale*t,py=t/scale,pz=vy/scale*t;
   double relative=pz-(-5+.1*px+.08*py)-settings.localFogBaseOffset;
   double h=std::max(0.,relative)/3,density=0;
   for(int layer=0;layer<4;++layer){double slice=layer*.5;
    density+=fieldSlice(slice)*std::max(0.,1-abs(h-slice)*2);}
   density*=.1*2.2*smooth(-.4f,.05f,float(relative));
   density*=1-smooth(65*.65f,65,float(sqrt(px*px+py*py)));density*=smooth(1,6,float(t));tau+=density*step;}
  double alpha=1-exp(-tau),transmitted=pow(32./255,2.2)*(1-alpha),headroom=1-transmitted;
  double scatter=pow(.36,2.2)*alpha;return int(pow(transmitted+headroom*(1-exp(-scatter/headroom)),1/2.2)*255+.5);};
 slopingDepth();render(false);int slopeError=0;
 for(int y=70;y<=110;y+=5){int actual=(pixel(64,y)>>16)&255;printf("slope y=%d actual=%d ref=%d\n",y,actual,referenceBank(64,y));slopeError=std::max(slopeError,abs(actual-referenceBank(64,y)));}
 printf("sloped fog error versus 8192-step reference=%d\n",slopeError);
 Require(slopeError<=5,"ground fog integration follows slope instead of camera-height slab");
 render(false);auto stationary=pixel(64,90);render(false);
 Require(diff(stationary,pixel(64,90))<=1,"stationary fog has no frame-random sampling flicker");
 // Change camera basis while keeping the same world geometry.
 f.inverseView.SetZero();f.inverseView.m[0][1]=-1;f.inverseView.m[1][2]=f.inverseView.m[2][0]=f.inverseView.m[3][3]=1;
 float rotated[4][4]={{0,0,1,0},{-1,0,0,0},{0,1,0,0},{0,0,0,1}};memcpy(groundView,rotated,sizeof(rotated));
 captureGround(hill);auto turned=groundPixel(32,32);Require(fabsf(turned[0]-gp[0])<.025f,"terrain height invariant under camera rotation");
 GroundVertex pond[]={{-16,-16,3},{16,-16,3},{-16,16,3},{16,16,3}};
 captureGround(pond);captureGround(hill);auto wet=groundPixel(0,0);
 Require(fabsf(wet[0]-3)<.01f,"water surface wins over seabed independent of draw order");
 // A hole in captured coverage must stay unknown, never invent a floating floor.
 auto missing=groundPixel(150,150);Require(missing[1]==0,"uncaptured ground has explicit missing coverage");
 // Fill the real atlas with a premultiplied height at varying confidence.
 // The tracked fallback is the same -5 plane, so coverage cannot change fog.
 f.inverseView.SetZero();f.inverseView.m[0][0]=f.inverseView.m[1][2]=f.inverseView.m[2][1]=f.inverseView.m[3][3]=1;
 fog.Reset(d.Get());encode(120,.94f);settings.localFogBaseOffset=-1;
 auto atlasConfidence=[&](float confidence){ScopedRenderState saved(d.Get());ComPtr<ID3DBlob> code;ComPtr<IDirect3DPixelShader9> ps;ComPtr<IDirect3DSurface9> surface;
  const char* source="float4 value:register(c0);float4 main():COLOR0{return value;}";
  Check(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","ps_3_0",0,0,code.GetAddressOf(),nullptr));Check(d->CreatePixelShader((DWORD*)code->GetBufferPointer(),ps.GetAddressOf()));
  Check(ground.Texture()->GetSurfaceLevel(0,surface.GetAddressOf()));Check(d->SetDepthStencilSurface(nullptr));Check(d->SetRenderTarget(0,surface.Get()));
  D3DVIEWPORT9 vp{0,0,1024,1024,0,1};Check(d->SetViewport(&vp));Check(d->SetVertexShader(nullptr));Check(d->SetPixelShader(ps.Get()));Check(d->SetFVF(D3DFVF_XYZRHW));
  for(auto rs:{D3DRS_ZENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_STENCILENABLE,D3DRS_FOGENABLE,D3DRS_SRGBWRITEENABLE})Check(d->SetRenderState(rs,FALSE));Check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));Check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));
  float value[4]={(-5-ground.Origin()[2])*confidence,confidence,0,1};Check(d->SetPixelShaderConstantF(0,value,1));
  struct V{float x,y,z,w;};V q[]={{-.5f,-.5f,0,1},{1023.5f,-.5f,0,1},{-.5f,1023.5f,0,1},{1023.5f,1023.5f,0,1}};
  Check(d->BeginScene());Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(V)));Check(d->EndScene());};
 atlasConfidence(1);render(false);DWORD fullCoverage=pixel(64,100);
 Require(diff(fullCoverage,0xff202020)>10,"coverage fixture contains visible local fog");
 for(float confidence:{.5f,.1f,0.f}){atlasConfidence(confidence);render(false);Require(diff(fullCoverage,pixel(64,100))<=1,"partial or missing atlas coverage does not erase fog");}
 ground.Reset();Require(!ground.Texture(),"height atlas releases default-pool resources on reset");
 render(false);Require(diff(fullCoverage,pixel(64,100))<=1,"absent atlas matches missing-coverage fallback");
 settings.localFogEnabled=false;settings.aerialDensity=0;settings.heightDensity=.02f;settings.mistDensity=0;settings.fogBaseOffset=0;
 render(true);DWORD heightOffsetZero=pixel(64,70);settings.fogBaseOffset=-50;render(true);Require(diff(heightOffsetZero,pixel(64,70))>5,"FogBaseOffset affects height atmosphere");
 settings.heightDensity=0;settings.mistDensity=.04f;settings.mistBaseOffset=0;render(true);DWORD mistOffsetZero=pixel(64,100);settings.mistBaseOffset=-20;render(true);Require(diff(mistOffsetZero,pixel(64,100))>5,"GroundMistOffset affects mist atmosphere");


 // End-to-end stationary animation with real temporal history enabled.
 settings.localFogEnabled=true;settings.localFogDensity=.1f;settings.localFogBaseOffset=0;settings.localFogHeightFalloff=1.f/3;
 settings.temporalEnabled=true;settings.temporalBlend=.88f;settings.resolutionScale=.5f;
 f.viewRaw=f.inverseView;f.viewRawValid=f.previousViewValid=true;
 D3DLOCKED_RECT nr{};Check(noise->LockRect(0,&nr,nullptr,0));
 for(int y=0;y<128;++y)for(int x=0;x<128;++x){float a=6.2831853f*x/128,b=6.2831853f*y/128;
  DWORD r=DWORD(128+55*sinf(a)*cosf(b)+35*sinf(3*a+2*b)),g=DWORD(128+55*cosf(2*a-b)+35*sinf(a+3*b));
  reinterpret_cast<DWORD*>(static_cast<BYTE*>(nr.pBits)+y*nr.Pitch)[x]=0xff000000|(r<<16)|(g<<8);}
 Check(noise->UnlockRect(0));
 auto bankLine=[&](){std::array<DWORD,96> line{};for(int x=16;x<112;++x)line[x-16]=pixel(x,90);return line;};
 for(int i=0;i<12;++i){Sleep(16);render(false);}auto stillStart=bankLine();
 for(int i=0;i<80;++i){Sleep(16);render(false);}auto stillEnd=bankLine();int evolution=0;
 for(int i=0;i<96;++i)evolution+=diff(stillStart[i],stillEnd[i]);
 printf("stationary temporal bank evolution=%d\n",evolution);Require(evolution>30,"stationary camera and player retain autonomous flow through temporal history");
 settings.enabled=false;Require(!fog.Render(d.Get(),f,back.Get(),false),"master atmosphere switch skips field and render");settings.enabled=true;
 fog.Reset(d.Get());render(false);Require(fog.GetRaymarchTexture()!=nullptr,"field and integration recreate after reset with local fog enabled");
 settings.temporalEnabled=false;

 // Compile the complete shipped water shader and exercise its exact local
 // reflection block on this device, with no copied approximation of the math.
 std::ifstream waterFile("WaterEffect.h");std::string waterText((std::istreambuf_iterator<char>(waterFile)),{});
 auto startSource=waterText.find("const char* source=R\"HLSL(")+strlen("const char* source=R\"HLSL(");
 auto endSource=waterText.find(")HLSL\";",startSource);std::string waterShader=waterText.substr(startSource,endSource-startSource);
 ComPtr<ID3DBlob> waterCode,error;ComPtr<IDirect3DPixelShader9> waterPS;
 Check(D3DCompile(waterShader.data(),waterShader.size(),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,waterCode.GetAddressOf(),error.GetAddressOf()));
 Check(d->CreatePixelShader((DWORD*)waterCode->GetBufferPointer(),waterPS.GetAddressOf()));Require(bool(waterPS),"complete production water shader accepted by D3D9");
 auto beginLocal=waterShader.find("    float3 localReflection=0;"),endLocal=waterShader.find("    // 4. Celestial",beginLocal);
 std::string reflectionShader="float4 localPosition[8]:register(c180);float4 localColor[8]:register(c188);float4 localControl:register(c196);float3 safeNormalize(float3 v){return normalize(v);}float4 main():COLOR0{float3 rgb=0,viewPos=float3(0,0,5),wave=float3(0,0,-1),v=wave;"+waterShader.substr(beginLocal,endLocal-beginLocal)+"return float4(rgb,1);}";
 {std::ofstream out("build/water-reflection-test.hlsl");out<<reflectionShader;}
 waterCode.Reset();waterPS.Reset();Check(D3DCompile(reflectionShader.data(),reflectionShader.size(),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,waterCode.GetAddressOf(),nullptr));Check(d->CreatePixelShader((DWORD*)waterCode->GetBufferPointer(),waterPS.GetAddressOf()));
 auto reflectedLight=[&](float radius,float enabled){float lights[17][4]{};lights[0][3]=radius;lights[8][0]=10;lights[8][1]=5;lights[8][2]=1;lights[8][3]=1;lights[16][0]=1;lights[16][1]=enabled;
  struct Q{float x,y,z,w;};Q q[]={{-.5f,-.5f,0,1},{127.5f,-.5f,0,1},{-.5f,127.5f,0,1},{127.5f,127.5f,0,1}};
  Check(d->SetRenderTarget(0,back.Get()));Check(d->SetViewport(&f.viewport));Check(d->SetVertexShader(nullptr));Check(d->SetFVF(D3DFVF_XYZRHW));Check(d->SetPixelShader(waterPS.Get()));Check(d->SetPixelShaderConstantF(180,lights[0],17));
  for(auto rs:{D3DRS_ZENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_STENCILENABLE,D3DRS_FOGENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_CLIPPLANEENABLE})Check(d->SetRenderState(rs,FALSE));Check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));Check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));Check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
  Check(d->BeginScene());Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(Q)));Check(d->EndScene());return pixel();};
 auto reflectionOn=reflectedLight(10,1);printf("water reflection=%08lx\n",reflectionOn);Require(((reflectionOn>>16)&255)>10,"local emitter reflects a warm specular highlight in water");
 Require(reflectedLight(10,0)==0xff000000,"reflection toggle suppresses local water highlight");Require(reflectedLight(2,1)==0xff000000,"water reflection cannot exceed emitter radius");
 manager.Configure(L".\\");manager.SelectForFrame({-9492.77f,71.4f,60.45f},{0,1,0});
 bool outdoorLamp=false;
 for(const auto& l:manager.Selected())
  if(Length(l.position-Vec3{-9499.8376f,60.7080f,59.3262f})<.1f)outdoorLamp=true;
 Require(outdoorLamp,"actual screenshot-area street lamp is selected from shipped manifest");
 // Exercise the exact production post pass with inherited additive/alpha state.
 ComPtr<ID3DBlob> postCode;ComPtr<IDirect3DPixelShader9> postShader;
 Check(D3DCompile(postProcessPixelSource,strlen(postProcessPixelSource),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,postCode.GetAddressOf(),nullptr));
 Check(d->CreatePixelShader((DWORD*)postCode->GetBufferPointer(),postShader.GetAddressOf()));
 ComPtr<IDirect3DTexture9> postScratch;ComPtr<IDirect3DSurface9> postSurface;
 Check(d->CreateTexture(128,128,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,postScratch.GetAddressOf(),nullptr));Check(postScratch->GetSurfaceLevel(0,postSurface.GetAddressOf()));
 auto grade=[&](float brightness,float contrast,float gamma){
  Check(d->SetRenderTarget(0,back.Get()));Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff404040,1,0));
  d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE);d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ONE);d->SetRenderState(D3DRS_COLORWRITEENABLE,1);
  float params[4]={brightness,contrast,gamma,0};Check(d->BeginScene());Check(ApplyImagePostProcess(d.Get(),back.Get(),postScratch.Get(),postSurface.Get(),postShader.Get(),params));Check(d->EndScene());
  DWORD mask=0;d->GetRenderState(D3DRS_COLORWRITEENABLE,&mask);Require(mask==1,"post process restores inherited colour mask");
  Check(d->GetRenderTargetData(back.Get(),read.Get()));D3DLOCKED_RECT r{};Check(read->LockRect(&r,nullptr,D3DLOCK_READONLY));DWORD result=((DWORD*)((BYTE*)r.pBits+64*r.Pitch))[64];read->UnlockRect();return result&255;};
 Require(abs(int(grade(0,1,1))-64)<=1,"neutral post process preserves scene");
 Require(grade(.35f,1,1)>140,"post brightness visibly changes output");
 Require(grade(0,1,1.68f)>100,"post gamma visibly changes output");
 Require(grade(0,1.74f,1)<30,"post contrast visibly changes output");
 ComPtr<ID3DBlob> rayLinkCode;ComPtr<IDirect3DPixelShader9> rayLinkShader;
 Check(D3DCompile(rayCompositePixelSource,strlen(rayCompositePixelSource),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,rayLinkCode.GetAddressOf(),nullptr));
 Check(d->CreatePixelShader((DWORD*)rayLinkCode->GetBufferPointer(),rayLinkShader.GetAddressOf()));
 auto linkedRay=[&](int opacity,bool available,bool debug,float rawDepth,float maxZ){
  fill(scene.Get(),0xff808080);fill(mask.Get(),DWORD(opacity)<<24);fillFloat(depth.Get(),rawDepth);
  Check(d->SetRenderTarget(0,back.Get()));Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0));
  Check(d->BeginScene());
  d->SetVertexShader(nullptr);d->SetPixelShader(rayLinkShader.Get());d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);d->SetTexture(0,scene.Get());d->SetTexture(1,mask.Get());d->SetTexture(2,depth.Get());
  d->SetSamplerState(2,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(2,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  d->SetSamplerState(2,D3DSAMP_MIPFILTER,D3DTEXF_NONE);d->SetSamplerState(2,D3DSAMP_SRGBTEXTURE,FALSE);
  d->SetSamplerState(2,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(2,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
  d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetRenderState(D3DRS_COLORWRITEENABLE,7);
  float link[4]={available?1.f:0.f,debug?1.f:0.f,0,0},depthInfo[4]={maxZ,0,0,0};
  d->SetPixelShaderConstantF(0,link,1);d->SetPixelShaderConstantF(1,depthInfo,1);
  struct V{float x,y,z,w,u,v;};V q[]={{-.5f,-.5f,0,1,0,0},{127.5f,-.5f,0,1,1,0},{-.5f,127.5f,0,1,0,1},{127.5f,127.5f,0,1,1,1}};
  Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(V)));Check(d->EndScene());return pixel()&255;};
 Require(linkedRay(0,true,false,1.f,1.f)==0,"clear medium suppresses linked rays");
 Require(linkedRay(128,true,false,1.f,1.f)>120,"moderate fog reveals rays");
 Require(linkedRay(250,true,false,1.f,1.f)<15,"dense banks extinguish linked rays");
 Require(linkedRay(0,false,false,1.f,1.f)==128,"missing current fog preserves unlinked fallback");
 Require(linkedRay(0,true,true,1.f,1.f)==128,"shaft diagnostic mask bypasses fog coupling");
 Require(linkedRay(128,true,false,.5f,1.f)==0,
         "depth-aware radial composite blocks moonlight on a foreground wall");
 Require(linkedRay(128,false,false,.5f,1.f)==0,
         "depth gate also blocks the unlinked fallback from bleeding through walls");
 // With viewport MaxZ=.99, a foreground surface can have raw depth .98;
 // it must not be confused with the clear value of exactly 1.
 Require(linkedRay(128,true,false,.98f,.99f)==0,
         "viewport-mapped foreground is not mistaken for sky");
 Require(linkedRay(128,false,false,.98f,.99f)==0,
         "raw-depth gate also blocks fallback rays at reduced viewport MaxZ");
 Require(linkedRay(128,true,false,.9997f,.9998f)==0,
         "geometry remains occluding when viewport MaxZ is nearly one");
 Require(linkedRay(128,true,false,.9997f,1.f)==0,
         "distant geometry cannot bleed radial shafts at the full viewport range");
 ComPtr<ID3DBlob> sourceMaskCode;ComPtr<IDirect3DPixelShader9> sourceMaskShader;
 Check(D3DCompile(volumePixelSource,strlen(volumePixelSource),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,sourceMaskCode.GetAddressOf(),nullptr));
 Check(d->CreatePixelShader((DWORD*)sourceMaskCode->GetBufferPointer(),sourceMaskShader.GetAddressOf()));
 auto sourceMaskPixel=[&](float rawDepth,float maxZ){
  fillFloat(depth.Get(),rawDepth);Check(d->SetRenderTarget(0,back.Get()));
  Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0));Check(d->BeginScene());
  d->SetVertexShader(nullptr);d->SetPixelShader(sourceMaskShader.Get());
  d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);d->SetTexture(1,depth.Get());
  d->SetTexture(3,scene.Get());d->SetSamplerState(1,D3DSAMP_MINFILTER,D3DTEXF_POINT);
  d->SetSamplerState(1,D3DSAMP_MAGFILTER,D3DTEXF_POINT);d->SetSamplerState(1,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
  d->SetSamplerState(1,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);d->SetSamplerState(1,D3DSAMP_SRGBTEXTURE,FALSE);
  d->SetSamplerState(3,D3DSAMP_SRGBTEXTURE,FALSE);d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
  d->SetRenderState(D3DRS_COLORWRITEENABLE,7);
  float projection[4]={f.projUnpack[0],f.projUnpack[1],f.projUnpack[2],f.projUnpack[3]};
  float medium[4]={0,0,0,350},sun[4]={.5f,.5f,1.f,maxZ},mode[4]={7,1,0,0};
  float rayTuning[4]={1.f/128,1.f/128,0,0},cloudMask[4]={0,0,0,0};
  d->SetPixelShaderConstantF(0,projection,1);d->SetPixelShaderConstantF(1,medium,1);
  d->SetPixelShaderConstantF(2,sun,1);d->SetPixelShaderConstantF(8,mode,1);
  d->SetPixelShaderConstantF(12,rayTuning,1);d->SetPixelShaderConstantF(14,cloudMask,1);
  struct V{float x,y,z,w,u,v;};V q[]={{-.5f,-.5f,0,1,0,0},{127.5f,-.5f,0,1,1,0},{-.5f,127.5f,0,1,0,1},{127.5f,127.5f,0,1,1,1}};
  Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(V)));Check(d->EndScene());
  d->SetTexture(1,nullptr);d->SetTexture(3,nullptr);return pixel()&255;};
 Require(sourceMaskPixel(.98f,.99f)<2,"viewport-mapped foreground is excluded from the celestial source mask");
 Require(sourceMaskPixel(.9997f,.9998f)<2,"near-one viewport geometry is excluded from the celestial source mask");
 Require(sourceMaskPixel(.9997f,1.f)<2,"far geometry is excluded from the source mask at MaxZ=1");
 Require(sourceMaskPixel(1.f,.99f)>250,"clear depth remains open sky in the celestial source mask");
 ComPtr<ID3DBlob> localSourceCode;ComPtr<IDirect3DPixelShader9> localSourceShader;
 Check(D3DCompile(localLightPixelSource,strlen(localLightPixelSource),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,localSourceCode.GetAddressOf(),nullptr));
 Check(d->CreatePixelShader((DWORD*)localSourceCode->GetBufferPointer(),localSourceShader.GetAddressOf()));
 auto localSourcePixel=[&](float rawDepth,float maxZ){
  fill(scene.Get(),0xffe08020);fillFloat(depth.Get(),rawDepth);
  Check(d->SetRenderTarget(0,back.Get()));Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0));
  Check(d->BeginScene());d->SetVertexShader(nullptr);d->SetPixelShader(localSourceShader.Get());
  d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);d->SetTexture(0,scene.Get());d->SetTexture(1,depth.Get());
  d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
  d->SetSamplerState(1,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(1,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  d->SetSamplerState(1,D3DSAMP_SRGBTEXTURE,FALSE);
  d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetRenderState(D3DRS_COLORWRITEENABLE,7);
  float tuning[4]={1.f/128,1.f/128,.01f,.25f},mode[4]={0,0,0,0},depthInfo[4]={maxZ,0,0,0};
  d->SetPixelShaderConstantF(0,tuning,1);d->SetPixelShaderConstantF(1,mode,1);
  d->SetPixelShaderConstantF(2,depthInfo,1);
  struct V{float x,y,z,w,u,v;};V q[]={{-.5f,-.5f,0,1,0,0},{127.5f,-.5f,0,1,1,0},{-.5f,127.5f,0,1,0,1},{127.5f,127.5f,0,1,1,1}};
  Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(V)));Check(d->EndScene());
  d->SetTexture(0,nullptr);d->SetTexture(1,nullptr);return (pixel()>>16)&255;};
 Require(localSourcePixel(.98f,.99f)>30,"reduced-MaxZ geometry remains eligible for local-light extraction");
 Require(localSourcePixel(.9997f,.9998f)>30,"near-one viewport geometry remains eligible for local-light extraction");
 Require(localSourcePixel(.9997f,1.f)>30,"distant geometry remains eligible for local-light extraction at MaxZ=1");
 Require(localSourcePixel(1.f,.99f)<2,"clear sky is excluded from local-light extraction");
 ComPtr<ID3DBlob> temporalCode;ComPtr<IDirect3DPixelShader9> temporalShader;
 Check(D3DCompile(volumeTemporalPixelSource,strlen(volumeTemporalPixelSource),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,temporalCode.GetAddressOf(),nullptr));
 Check(d->CreatePixelShader((DWORD*)temporalCode->GetBufferPointer(),temporalShader.GetAddressOf()));
 auto temporalCurrent=texture(D3DFMT_A8R8G8B8),temporalHistory=texture(D3DFMT_A8R8G8B8);
 fill(temporalCurrent.Get(),0xff808080);fill(temporalHistory.Get(),0xff202020);
 auto setPixel=[&](IDirect3DTexture9* t,int x,int y,DWORD value){D3DLOCKED_RECT r{};Check(t->LockRect(0,&r,nullptr,0));reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=value;Check(t->UnlockRect(0));};
 setPixel(temporalCurrent.Get(),63,64,0xff000000);setPixel(temporalCurrent.Get(),65,64,0xffffffff);
 for(int y=62;y<=66;++y)for(int x=94;x<=98;++x)setPixel(temporalHistory.Get(),x,y,0xffe0e0e0);
 auto temporalPixel=[&](float rawDepth,float maxZ){
  f.viewport.MinZ=0;f.viewport.MaxZ=maxZ;f.depthMaxZ=maxZ;
  fillFloat(depth.Get(),rawDepth);Check(d->SetRenderTarget(0,back.Get()));Check(d->SetViewport(&f.viewport));
  Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0));Check(d->BeginScene());
  Check(d->SetVertexShader(nullptr));Check(d->SetPixelShader(temporalShader.Get()));Check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
  Check(d->SetTexture(0,temporalCurrent.Get()));Check(d->SetTexture(1,temporalHistory.Get()));Check(d->SetTexture(2,depth.Get()));
  for(DWORD s=0;s<3;++s){Check(d->SetSamplerState(s,D3DSAMP_MINFILTER,s<2?D3DTEXF_LINEAR:D3DTEXF_POINT));Check(d->SetSamplerState(s,D3DSAMP_MAGFILTER,s<2?D3DTEXF_LINEAR:D3DTEXF_POINT));Check(d->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));Check(d->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));Check(d->SetSamplerState(s,D3DSAMP_SRGBTEXTURE,FALSE));}
  Check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));Check(d->SetRenderState(D3DRS_ZENABLE,FALSE));Check(d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE));Check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));
  float temporal[4]={1.f/128,1.f/128,1,1},inverse[3][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0}},camera[4]={0,0,0,0};
  float previousView[4][4]={{1,0,0,1},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
  float normalized=std::clamp(rawDepth/maxZ,0.f,1.f);
  float z=f.projUnpack[1]/(normalized-f.projUnpack[0]);previousView[3][0]=.5f*z/f.projUnpack[2];
  float depthRange[4]={0,maxZ,maxZ,0};
  Check(d->SetPixelShaderConstantF(0,temporal,1));Check(d->SetPixelShaderConstantF(1,f.projUnpack,1));
  Check(d->SetPixelShaderConstantF(2,inverse[0],3));Check(d->SetPixelShaderConstantF(5,camera,1));
  Check(d->SetPixelShaderConstantF(6,previousView[0],4));Check(d->SetPixelShaderConstantF(10,depthRange,1));
  struct V{float x,y,z,w,u,v;};V q[]={{-.5f,-.5f,0,1,0,0},{127.5f,-.5f,0,1,1,0},{-.5f,127.5f,0,1,0,1},{127.5f,127.5f,0,1,1,1}};
  Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(V)));Check(d->EndScene());
  for(DWORD s=0;s<3;++s)Check(d->SetTexture(s,nullptr));return (pixel(64,64)>>16)&255;
 };
 const int clearHistory=temporalPixel(1.f,.99f),farGeometryHistory=temporalPixel(.99f,.99f);
 printf("temporal reduced-MaxZ clear=%d far geometry=%d\n",clearHistory,farGeometryHistory);
 Require(clearHistory<80&&farGeometryHistory>160,
         "temporal sky classification uses raw depth before MaxZ normalization");
 // --- Volumetric clouds -------------------------------------------------
 // Same device, a fresh synthetic depth buffer: genuine clear sky in the
 // top half, solid geometry thirty units away in the bottom half, with a
 // reduced viewport MaxZ to exercise raw-depth sky classification. The
 // noise fields are deterministic, so coverage is a value assertion.
 auto& clouds=VolumetricClouds::Instance();auto& cloud=clouds.Settings();
 clouds.ApplyQualityPreset(1);
 cloud.enabled=true;cloud.coverage=1.f;cloud.density=1.f;cloud.weatherCoverageBoost=0;
 cloud.debugMode=CloudDebugMode::None;cloud.cloudType=0;
 cloud.planetRadius=42000;cloud.baseAltitude=900;cloud.thickness=1400;
 cloud.featureSize=5200;cloud.detailSize=380;cloud.weatherSize=34000;
 cloud.windSpeed=0;cloud.maxDistance=26000;cloud.horizonFade=.018f;
 cloud.groundShadows=false;cloud.groundShadowStrength=.55f;cloud.groundShadowExtent=14000;
 f.cameraPosition={0,0,0};f.sunDirectionWorld={0,0,1};f.directionalLightColor={0,0,0};
 f.celestialIntensity=1;f.celestialIsMoon=false;f.daylightFactor=1;f.moonlightFactor=0;
 f.viewport.MinZ=0;f.viewport.MaxZ=.94f;f.depthMaxZ=.94f;
 {
  D3DLOCKED_RECT r{};Check(depth->LockRect(0,&r,nullptr,0));
  for(int y=0;y<128;++y)for(int x=0;x<128;++x)
   reinterpret_cast<float*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=y<64?1.f:.94f*(f.projUnpack[0]+f.projUnpack[1]/30.f);
  Check(depth->UnlockRect(0));
 }
 fill(scene.Get(),0xff202020);
 // Resolution is independent of march quality and reallocates live.
 const auto marchSamples=cloud.sampleCount;
 cloud.resolutionScale=.5f;
 Check(d->BeginScene());Require(clouds.Render(d.Get(),f,back.Get()),"half-resolution cloud pass renders");Check(d->EndScene());
 D3DSURFACE_DESC cloudDesc{};Check(clouds.GetCloudTexture()->GetLevelDesc(0,&cloudDesc));
 Require(cloudDesc.Width==64&&cloudDesc.Height==64&&cloud.sampleCount==marchSamples,
         "cloud resolution changes without changing march quality");
 cloud.resolutionScale=.25f;
 // Clearing to a colour the scene never uses proves the composite really
 // re-published the scene instead of the target happening to survive.
 auto renderClouds=[&]{Check(d->SetRenderTarget(0,back.Get()));Check(d->SetViewport(&f.viewport));
  Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff123456,1,0));Check(d->SetPixelShaderConstantF(0,sentinels[0],64));
  Check(d->BeginScene());bool ok=clouds.Render(d.Get(),f,back.Get());Check(d->EndScene());return ok;};
 // One pixel cannot describe a procedural sky: these average the blue
 // channel over the part of the frame that is genuinely above the horizon.
 auto skyMean=[&]{Check(d->GetRenderTargetData(back.Get(),read.Get()));D3DLOCKED_RECT r{};
  Check(read->LockRect(&r,nullptr,D3DLOCK_READONLY));double sum=0;int n=0;
  for(int y=6;y<58;++y)for(int x=6;x<122;++x){sum+=double(reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]&255);++n;}
  Check(read->UnlockRect());return sum/double(n?n:1);};
 Require(renderClouds(),"overcast cloud deck renders on the production device");
 Require(clouds.FieldsReady(),"the shape, detail and weather fields reach the GPU");
 Check(clouds.GetCloudTexture()->GetLevelDesc(0,&cloudDesc));
 Require(cloudDesc.Width==32&&cloudDesc.Height==32,"cloud resolution can be lowered live again");
 {
  float restored[64][4];Check(d->GetPixelShaderConstantF(0,restored[0],64));
  Require(!memcmp(sentinels,restored,sizeof(restored)),"cloud pass restores c0..c63");
  D3DVIEWPORT9 restoredViewport{};Check(d->GetViewport(&restoredViewport));
  Require(!memcmp(&restoredViewport,&f.viewport,sizeof(f.viewport)),"cloud pass restores the viewport");
 }
 DWORD overcastSky=pixel(64,20);
 printf("clouds overcast sky=%08lx ground=%08lx mean=%.1f\n",overcastSky,pixel(64,110),skyMean());
 Require(diff(overcastSky,0xff202020)>12,"overcast deck is visible against the sky");
 Require(pixel(64,110)==0xff202020,"cloud composite leaves solid geometry byte-identical");
 Require((overcastSky&255)>((overcastSky>>16)&255),"deck takes the environment ambient tint instead of a fixed white");
 // Opacity, not brightness, is the honest measurement: the diagnostic
 // returns the integrated (1 - transmittance) directly.
 cloud.debugMode=CloudDebugMode::Coverage;
 cloud.coverage=.15f;renderClouds();const double thinDeck=skyMean();
 cloud.coverage=.45f;renderClouds();const double medium=skyMean();
 cloud.coverage=.85f;renderClouds();const double solid=skyMean();
 printf("clouds coverage thin=%.1f medium=%.1f solid=%.1f\n",thinDeck,medium,solid);
 Require(thinDeck<medium-10&&medium<solid-30,"the coverage slider walks the deck from broken to overcast");
 Require(solid>120,"maximum coverage really closes the sky");
 cloud.coverage=1.f;
 cloud.density=.05f;renderClouds();const double sparseDeck=skyMean();
 cloud.density=1.f;renderClouds();const double denseDeck=skyMean();
 printf("clouds opacity sparse=%.1f dense=%.1f\n",sparseDeck,denseDeck);
 Require(denseDeck>sparseDeck+40,"density drives the integrated opacity of the deck");
 cloud.debugMode=CloudDebugMode::None;
 cloud.coverage=.3f;cloud.weatherCoverageBoost=.5f;
 const DWORD boosted=(renderClouds(),pixel(64,20));
 cloud.weatherCoverageBoost=0;
 Require(diff(boosted,0xff202020)>12,"native rain pushes coverage up through the weather boost");
 Require(clouds.Settings().coverage==.3f,"the weather boost never mutates the configured coverage");
 // The deck stays at its configured world height, so all three camera
 // regions have to behave: under it, inside it, above it.
 cloud.coverage=1.f;
 f.cameraPosition={0,0,1500};
 Require(renderClouds()&&diff(pixel(64,20),0xff202020)>12,"a camera inside the deck still integrates the cloud around it");
 // The march is clipped by the depth buffer, not skipped on it: cloud
 // between the eye and a surface is as real as cloud against the sky, and
 // a sky-only pass deletes the whole deck the moment you fly above it.
 {
  D3DLOCKED_RECT r{};Check(depth->LockRect(0,&r,nullptr,0));
  for(int y=64;y<128;++y)for(int x=0;x<128;++x)
   reinterpret_cast<float*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=.94f*(f.projUnpack[0]+f.projUnpack[1]/800.f);
  Check(depth->UnlockRect(0));
 }
 Require(renderClouds()&&diff(pixel(64,110),0xff202020)>12,"cloud in front of distant geometry is composited over it");
 {
  D3DLOCKED_RECT r{};Check(depth->LockRect(0,&r,nullptr,0));
  for(int y=64;y<128;++y)for(int x=0;x<128;++x)
   reinterpret_cast<float*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=.94f*(f.projUnpack[0]+f.projUnpack[1]/30.f);
  Check(depth->UnlockRect(0));
 }
 Require(renderClouds()&&pixel(64,110)==0xff202020,"a surface thirty units away is still in front of the deck");
 f.cameraPosition={0,0,9000};
 Require(renderClouds()&&pixel(64,20)==0xff202020,"above the deck the sky overhead is genuinely empty");
 f.cameraPosition={0,0,0};
 // Cloud shadows: the sun is straight up and the sky is solid, so the
 // ground thirty units away has to lose light - and only when asked.
 Require(renderClouds()&&pixel(64,110)==0xff202020,"ground stays untouched while cloud shadows are off");
 cloud.groundShadows=true;
 Require(renderClouds(),"cloud shadow pass renders");
 const DWORD shadowed=pixel(64,110);
 printf("clouds shadowed ground=%08lx\n",shadowed);
 Require((shadowed&255)<0x20&&((shadowed>>8)&255)<0x20,"an overcast deck casts its shadow on the world below");
 Require(clouds.GetShadowTexture()!=nullptr,"the shadow map is available to the rest of the renderer");
 // A shadow field must not change when only the camera height changes.
 ComPtr<IDirect3DSurface9> shadowRead;
 Check(d->CreateOffscreenPlainSurface(256,256,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,shadowRead.GetAddressOf(),nullptr));
 auto shadowBytes=[&]{
  ComPtr<IDirect3DSurface9> surface;Check(clouds.GetShadowTexture()->GetSurfaceLevel(0,surface.GetAddressOf()));
  Check(d->GetRenderTargetData(surface.Get(),shadowRead.Get()));D3DLOCKED_RECT r{};
  Check(shadowRead->LockRect(&r,nullptr,D3DLOCK_READONLY));std::vector<DWORD> bytes(256*256);
  for(int y=0;y<256;++y)memcpy(bytes.data()+y*256,static_cast<BYTE*>(r.pBits)+y*r.Pitch,256*sizeof(DWORD));
  Check(shadowRead->UnlockRect());return bytes;
 };
 const auto fixedShadows=shadowBytes();
 f.cameraPosition.z=100;Require(renderClouds(),"raised camera renders cloud shadows");
 Require(shadowBytes()==fixedShadows,"camera height cannot change the cloud shadow field");
 f.cameraPosition={.1f,.1f,0};Require(renderClouds(),"sub-texel camera movement renders");
 Require(shadowBytes()==fixedShadows,"sub-texel camera movement cannot shimmer the shadow field");
 f.cameraPosition={0,0,0};
 cloud.debugMode=CloudDebugMode::ShadowMap;
 renderClouds();
 printf("clouds shadow map mean=%.1f\n",skyMean());
 Require(skyMean()<200,"the shadow map itself is not a blank sheet");
 cloud.debugMode=CloudDebugMode::None;
 // A disc on the horizon would cast a shadow a hundred kilometres long:
 // the pass has to bow out instead of smearing one texel over the world.
 f.sunDirectionWorld={1,0,.01f};
 Require(renderClouds()&&pixel(64,110)==0xff202020,"a disc at the horizon casts no ground shadow");
 // The stabilized direct light, not the disc-draw signal, drives both
 // the deck's lighting and its ground shadow: with the stabilized light
 // out (night / no body up) neither may appear, and with it on, both
 // must - regardless of what celestialIntensity happens to say.
 // DirectLighting debug returns the raw phase energy and ignores the
 // intensity by design, so measure the real composite here.
 f.sunDirectionWorld={0,0,1};
 f.directLightValid=true;f.directLightIsMoon=false;f.directLightIntensity=0;f.directLightDirectionWorld={0,0,1};
 renderClouds();const double unlitDirect=skyMean();
 Require(renderClouds()&&pixel(64,110)==0xff202020,"no stabilized light casts no cloud shadow");
 f.directLightIntensity=1;
 renderClouds();const double litDirect=skyMean();
 printf("clouds direct lighting unlit=%.1f lit=%.1f\n",unlitDirect,litDirect);
 Require(litDirect>unlitDirect+20,"stabilized light lights the deck");
 Require(renderClouds()&&((pixel(64,110)>>8)&255)<0x20,"stabilized light restores the cloud shadow");
 f.directLightValid=false;
 f.sunDirectionWorld={0,0,1};cloud.groundShadows=false;
 cloud.enabled=false;
 Check(d->SetRenderTarget(0,back.Get()));Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff123456,1,0));
 Check(d->BeginScene());bool cloudsOff=clouds.Render(d.Get(),f,back.Get());Check(d->EndScene());
 Require(!cloudsOff&&pixel(64,20)==0xff123456,"disabled clouds draw nothing at all");
 Require(clouds.GetShadowTexture()==nullptr,"disabled cloud pass cannot publish a stale shadow map");
 cloud.enabled=true;cloud.coverage=0;
 Check(d->BeginScene());bool clearSky=clouds.Render(d.Get(),f,back.Get());Check(d->EndScene());
 Require(!clearSky&&pixel(64,20)==0xff123456,"zero coverage skips the pass entirely");
 cloud.coverage=.5f;cloud.density=1.f;clouds.Reset(d.Get());
 Require(clouds.GetCloudTexture()==nullptr,"cloud reset releases the device resources");
 Require(renderClouds()&&clouds.GetCloudTexture()!=nullptr,"the deck rebuilds itself after a device reset");
 Require(clouds.FieldsReady(),"a device reset does not throw the generated fields away");
 clouds.Reset(d.Get());
 // Synthetic transmittance isolates cloud-filtered shafts from cloud colour.
 auto transmittance=texture(D3DFMT_A8R8G8B8);
 f.cloudShadowTexture=transmittance.Get();f.cloudShadowArea={-7000,-7000,1.f/14000,1};
 f.cloudLightProjection={0,0,900,1400};f.cloudRayStrength=.8f;
 f.directLightValid=true;f.directLightIntensity=1;f.directLightIsMoon=false;
 f.directLightDirectionView={0,0,1};f.celestialIntensity=0;f.sunScreenX=f.sunScreenY=-1;
 settings.enabled=true;settings.temporalEnabled=false;settings.localFogEnabled=false;
 settings.debugMode=VolumetricDebugMode::CelestialInscatter;settings.densityScale=1;
 settings.aerialDensity=.002f;settings.fogWash=.55f;settings.sunGlowStrength=0;
 encode(120,1);fill(transmittance.Get(),0xff000000);const DWORD blockedCloudRays=render(true);
 fill(transmittance.Get(),0xffffffff);const DWORD openCloudRays=render(true);
 Require(diff(openCloudRays,blockedCloudRays)>5,"cloud gaps transmit sunlight even with the disc off screen");
 f.cloudRayStrength=0;const DWORD raysOff=render(true);
 Require(diff(openCloudRays,raysOff)>5,"cloud ray strength zero removes the new shaft contribution");

 // The circular screen-space term belongs to open sky, not to foreground
 // surfaces that happen to be inside the fog raymarch. A tracked moon toggle
 // must therefore have no effect on an opaque wall, while the same halo is
 // still visible around the source in clear-sky pixels.
 f.cloudShadowTexture=nullptr;f.cloudRayStrength=0;
 settings.debugMode=VolumetricDebugMode::None;settings.temporalEnabled=false;
 settings.localFogEnabled=false;settings.edgeFogEnabled=false;settings.aerialDensity=.002f;
 settings.heightDensity=settings.mistDensity=0;settings.sunGlowStrength=3.f;settings.moonStrength=1.f;
 f.directLightValid=true;f.directLightIsMoon=true;f.directLightIntensity=1.f;
 f.directLightDirectionView={0,0,-1};f.sunScreenX=.5f;f.sunScreenY=.5f;
 f.cameraPosition={0,0,0};f.celestialIntensity=0;
 encode(40,1.f);render(true);const DWORD wallWithoutMoon=pixel(74,64);
 f.celestialIntensity=1.f;render(true);const DWORD wallWithMoon=pixel(74,64);
 Require(diff(wallWithMoon,wallWithoutMoon)<=1,
         "moon halo does not paint through a foreground wall");
 // Near the far plane, normalized/reconstructed depth can be almost one
 // even though the raw depth buffer still contains opaque geometry. The
 // screen-space halo must use the original clear-depth signal, not the
 // normalized depth used to reconstruct the raymarch distance.
 encode(700,1.f);f.celestialIntensity=0;render(true);const DWORD farWallWithoutMoon=pixel(74,63);
 f.celestialIntensity=1.f;render(true);const DWORD farWallWithMoon=pixel(74,63);
 Require(diff(farWallWithMoon,farWallWithoutMoon)<=1,
         "moon halo does not leak through far-plane geometry");
 fillFloat(depth.Get(),1.f);f.celestialIntensity=0;
 render(true);const DWORD skyWithoutMoon=pixel(74,64);
 f.celestialIntensity=1.f;
 render(true);const DWORD skyWithMoon=pixel(74,64);
 Require(diff(skyWithMoon,skyWithoutMoon)>1,"moon halo remains visible through open-sky fog");

 f.directLightValid=false;settings.debugMode=VolumetricDebugMode::None;
 lighting.Reset(d.Get());fog.Reset(d.Get());DestroyWindow(window);puts("PASS atmosphere regression suite");return 0;
} catch(const std::exception& e) {printf("FAIL %s\n",e.what());return 1;}
