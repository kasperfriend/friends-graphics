#pragma once
#include "src/D3D9/TrackedRenderState.h"
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

inline void Prepare(IDirect3DDevice9* d){
 // A previous world capture must not leak into an offscreen water draw.
 // The owned copy stays ready and is re-exposed on the next valid world tile.
 ClearInput();
 if(!watereffect::NeedsScene()||volume::internal||!renderer::water::IsMaterial(d))return;
 if(volume::owner!=d||!volume::ready||volume::composed||!volume::target||!volume::depth)return;
 // Never capture a shadow/offscreen target or expose another device's scene.
 ComPtr<IDirect3DSurface9> current;
 if(FAILED(d->GetRenderTarget(0,current.GetAddressOf()))||current.Get()!=volume::target.Get())return;
 if(ready&&resources.owner==d){Expose();return;}
 // If the first copy fails, retry next frame, not after native/modern water
 // has already entered the color buffer. Every tile needs the same input.
 if(attempted)return;
 attempted=true;
 D3DSURFACE_DESC desc{};
 if(FAILED(volume::target->GetDesc(&desc))||desc.MultiSampleType!=D3DMULTISAMPLE_NONE){++failedFrames;return;}
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
