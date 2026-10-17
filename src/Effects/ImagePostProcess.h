#pragma once
#include <d3d9.h>
#include <initializer_list>
#include "../D3D9/ScopedPassState.h"
namespace renderer {
// `aaParameters` is c2 and is required rather than defaulted: the stage reads its anti-aliasing
// switch from c2.x, and a constant register this pass does not write keeps whatever the previous
// pass left in it, so an optional argument here would be a way for a call site to silently leave
// the switch on. c0 is full (brightness/contrast/gamma/sharpness), which is why AA has its own
// register instead of a spare lane.
inline HRESULT ApplyImagePostProcess(IDirect3DDevice9* d,IDirect3DSurface9* target,IDirect3DTexture9* scene,IDirect3DSurface9* scratch,IDirect3DPixelShader9* shader,const float* parameters,const float* aaParameters) {
 if(!d||!target||!scene||!scratch||!shader||!parameters||!aaParameters)return E_INVALIDARG;
 ScopedPassState saved(d);HRESULT hr=S_OK;
 auto set=[&](HRESULT result){if(SUCCEEDED(hr)&&FAILED(result))hr=result;};
 D3DSURFACE_DESC desc{};set(target->GetDesc(&desc));
 for(DWORD slot=0;slot<6;++slot)set(d->SetTexture(slot,nullptr));
 set(d->StretchRect(target,nullptr,scratch,nullptr,D3DTEXF_NONE));
 if(FAILED(hr))return hr;
 set(d->SetRenderTarget(0,target));set(d->SetDepthStencilSurface(nullptr));
 D3DVIEWPORT9 viewport{0,0,desc.Width,desc.Height,0,1};set(d->SetViewport(&viewport));
 set(d->SetVertexShader(nullptr));set(d->SetPixelShader(shader));set(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
 for(auto state:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING})set(d->SetRenderState(state,FALSE));
 set(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));set(d->SetRenderState(D3DRS_COLORWRITEENABLE,7));set(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));set(d->SetRenderState(D3DRS_CLIPPLANEENABLE,0));
 set(d->SetTexture(0,scene));set(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE));
 set(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));set(d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR));set(d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
 set(d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));set(d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
 float texel[4]={1.f/desc.Width,1.f/desc.Height,0,0};set(d->SetPixelShaderConstantF(0,parameters,1));set(d->SetPixelShaderConstantF(1,texel,1));set(d->SetPixelShaderConstantF(2,aaParameters,1));
 struct Vertex{float x,y,z,w,u,v;};float w=desc.Width-.5f,h=desc.Height-.5f;
 Vertex quad[]={{-.5f,-.5f,0,1,0,0},{w,-.5f,0,1,1,0},{-.5f,h,0,1,0,1},{w,h,0,1,1,1}};
 if(SUCCEEDED(hr))set(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex)));
 return hr;
}
}
