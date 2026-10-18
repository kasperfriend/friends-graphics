#pragma once
#include "src/D3D9/TrackedRenderState.h"
#include "src/Water/WaterMaterial.h"
namespace waterreflection {
using Microsoft::WRL::ComPtr;
struct Resources{
    ComPtr<IDirect3DTexture9> scene;
    ComPtr<IDirect3DSurface9> surface;
    UINT width=0, height=0;
    D3DFORMAT format=D3DFMT_UNKNOWN;
    IDirect3DDevice9* owner=nullptr;
};
inline Resources& resources=*new Resources;
inline bool ready=false,attempted=false;
inline unsigned copiedFrames=0,failedFrames=0,presentedFrames=0;
inline std::wstring logPath;

inline void Configure(const std::wstring& base){logPath=base+L"WaterReflection.log";}
inline void ClearInput(){watereffect::reflectionScene=nullptr;watereffect::reflectionDepth=nullptr;memset(watereffect::reflectionData,0,sizeof(watereffect::reflectionData));}

inline void Expose(){
 if(!ready||!resources.scene)return;
 watereffect::reflectionScene=resources.scene.Get();
 watereffect::reflectionDepth=volume::depth.Get();
 memcpy(watereffect::reflectionData,volume::constants[0],sizeof(float)*4);
 watereffect::reflectionData[5]=1.f/float(std::max<UINT>(resources.width,1));
 watereffect::reflectionData[6]=1.f/float(std::max<UINT>(resources.height,1));
 watereffect::reflectionData[7]=volume::constants[2][3];
}

inline void Prepare(IDirect3DDevice9* d,const renderer::water::Observation* observed=nullptr){
 // A previous world capture must not leak into an offscreen water draw.
 // The owned copy stays ready and is re-exposed on the next valid world tile.
 ClearInput();
 if(!watereffect::NeedsScene()||volume::internal)return;
 const auto material=observed?*observed:renderer::water::Observe(d);
 if(!material.liquid)return;
 if(volume::owner!=d||volume::composed||!volume::target||!volume::depth)return;
 // Never capture a shadow/offscreen target or expose another device's scene.
 ComPtr<IDirect3DSurface9> current;
 if(FAILED(d->GetRenderTarget(0,current.GetAddressOf()))||current.Get()!=volume::target.Get())return;
 if(ready&&resources.owner==d){if(volume::ready)Expose();return;}
 // The FIRST world water draw is the capture deadline, even when camera
 // capture is not ready yet. A later retry would include water already drawn
 // into the color buffer (self-reflection/refraction and inconsistent tiles).
 // Offscreen draws above do not consume this world's capture opportunity.
 if(attempted)return;
 attempted=true;
 if(!volume::ready)return;
 D3DSURFACE_DESC desc{};
 // Colour only, and multisampling is ALLOWED here - which it was not, and the guard was costing the
 // whole reflection/refraction colour source whenever the client ran above 1x. Copying a
 // multisampled render target into the plain D3DUSAGE_RENDERTARGET texture created below is not a
 // special case in D3D9, it is the resolve: StretchRect averages the samples on the way across,
 // which is exactly what a sampled colour source needs. If a driver refuses it, the HRESULT is
 // already handled one line down and this fails the way it used to, per frame, without taking
 // anything else with it.
 //
 // The DEPTH side of this renderer has no equivalent and cannot be given one: a multisampled depth
 // buffer cannot be sampled by a pixel shader in D3D9 at all, so above 1x the effects that read
 // depth lose it whatever this file does. That is why anti-aliasing lives in the image pass
 // (see GraphicsEffects.ini's [PostProcess] AntiAliasing) and why 1x multisampling is the setting
 // this renderer is meant to run at.
 if(FAILED(volume::target->GetDesc(&desc))){++failedFrames;return;}
 if(resources.owner!=d||resources.width!=desc.Width||resources.height!=desc.Height||resources.format!=desc.Format||!resources.scene||!resources.surface){
     resources.scene.Reset();
     resources.surface.Reset();
     resources.owner=d;
     resources.width=desc.Width;
     resources.height=desc.Height;
     resources.format=desc.Format;
     if(FAILED(d->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,desc.Format,D3DPOOL_DEFAULT,resources.scene.GetAddressOf(),nullptr))||
        FAILED(resources.scene->GetSurfaceLevel(0,resources.surface.GetAddressOf()))){
         ++failedFrames;return;
     }
 }
 if(FAILED(d->StretchRect(volume::target.Get(),nullptr,resources.surface.Get(),nullptr,D3DTEXF_NONE))){
     ++failedFrames;return;
 }
 ready=true;++copiedFrames;Expose();
 if(copiedFrames==1)std::ofstream(std::filesystem::path(logPath),std::ios::app)<<"SSR source captured before first confirmed water draw "<<desc.Width<<'x'<<desc.Height<<'\n';
}

inline void Finish(){ClearInput();ready=attempted=false;}
inline void Reset(IDirect3DDevice9* /*d*/){Finish();resources.surface.Reset();resources.scene.Reset();resources.owner=nullptr;resources.width=resources.height=0;resources.format=D3DFMT_UNKNOWN;}
inline void Present(){if(++presentedFrames%600==120)std::ofstream(std::filesystem::path(logPath),std::ios::app)<<"copied="<<copiedFrames<<" failed="<<failedFrames<<'\n';Finish();}
}
