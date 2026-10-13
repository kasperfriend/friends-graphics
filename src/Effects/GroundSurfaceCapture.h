#pragma once
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include "../Core/DeviceHealth.h"
#include "../Core/FrameContext.h"
#include "../D3D9/ScopedRenderState.h"
#include "../D3D9/TrackedRenderState.h"

namespace renderer {
// A persistent, world-aligned height field of actual terrain/liquid geometry.
// No vertex-buffer locks, guessed map IDs, asset copies, or GPU readbacks.
class GroundSurfaceCapture {
    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DSurface9> surface, depth;
    ComPtr<IDirect3DVertexShader9> vs;
    ComPtr<IDirect3DPixelShader9> ps;
    IDirect3DDevice9* owner=nullptr;
    RetryGate retry;
    bool valid=false,terrainReady=false;
    float origin[4]{};
    ULONGLONG lastTick=0;
    static constexpr UINT Size=1024;
    // Creation is attempted once per cooldown while it fails. Without this,
    // every terrain draw would recompile both shaders while the device is
    // unavailable.
    bool Ensure(IDirect3DDevice9* d) {
        if(owner==d && texture && depth && vs && ps)return true;
        const ULONGLONG now=GetTickCount64();
        if(!retry.Open(now))return false;
        if(Create(d)){retry.Succeed();return true;}
        retry.Fail(now,kOptionalPassRetryMs);
        return false;
    }
    bool Create(IDirect3DDevice9* d) {
        Release();owner=d;
        const char* vertex=R"(
float4 mv[4]:register(c0); float4 iv[3]:register(c4);
float4 camera:register(c7); float4 atlas:register(c8);
struct O {float4 p:POSITION;float h:TEXCOORD0;};
O main(float4 p:POSITION){O o;float3 v=(mv[0]*p.x+mv[1]*p.y+mv[2]*p.z+mv[3]).xyz;
float3 w=camera.xyz+v.x*iv[0].xyz+v.y*iv[1].xyz+v.z*iv[2].xyz;
o.h=w.z-atlas.z;o.p=float4((w.x-atlas.x)/256-1.0/1024,-(w.y-atlas.y)/256+1.0/1024,.5-o.h/4096,1);return o;})";
        const char* pixel="float4 main(float h:TEXCOORD0):COLOR0{return float4(h,1,0,1);}";
        ComPtr<ID3DBlob> b;
        if(FAILED(D3DCompile(vertex,strlen(vertex),nullptr,nullptr,nullptr,"main","vs_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,b.GetAddressOf(),nullptr)))return false;
        if(FAILED(d->CreateVertexShader((DWORD*)b->GetBufferPointer(),vs.GetAddressOf())))return false;
        b.Reset();
        if(FAILED(D3DCompile(pixel,strlen(pixel),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,b.GetAddressOf(),nullptr)))return false;
        if(FAILED(d->CreatePixelShader((DWORD*)b->GetBufferPointer(),ps.GetAddressOf())))return false;
        return SUCCEEDED(d->CreateTexture(Size,Size,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,texture.GetAddressOf(),nullptr)) &&
            SUCCEEDED(texture->GetSurfaceLevel(0,surface.GetAddressOf())) &&
            SUCCEEDED(d->CreateDepthStencilSurface(Size,Size,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,depth.GetAddressOf(),nullptr));
    }
public:
    static GroundSurfaceCapture& Instance(){// D3D resources must not be released from CRT teardown under loader lock.
        // Reset releases them on device reset/replacement; process exit owns the final lifetime.
        static auto* instance=new GroundSurfaceCapture;return *instance;}
    void Release(){texture.Reset();surface.Reset();depth.Reset();vs.Reset();ps.Reset();owner=nullptr;valid=false;terrainReady=false;lastTick=0;}
    void Reset(){Release();retry=RetryGate{};}
    IDirect3DTexture9* Texture()const{return valid&&terrainReady?texture.Get():nullptr;}
    const float* Origin()const{return origin;}
    static bool IsTerrainVertexShader(uint64_t hash) {
        // Verified from the client's native terrain VS dumps: c0..c3 model-view,
        // c4..c7 projection, terrain UVs derived from position and c18..c23.
        // PS hashes change with native shadows and therefore are not geometry IDs.
        switch(hash){
        case 0x2437f34e978bb37full: case 0x26c1716cad7cb0abull:
        case 0x79a7faa602ba6b68ull: case 0x91f8b826abc842a1ull:
        case 0x938d1ef758aa6085ull: case 0xde5fb134242c2b4eull:
        case 0xf6d02568ca25d7caull:return true;
        default:return false;}
    }
    // Called before replacement material scopes, only for genuine world draws.
    template<class Draw> void Observe(IDirect3DDevice9* d,const FrameContext& f,Draw draw) {
        if(!f.cameraValid || !g_trackedState.zEnable)return;
        const auto hash=g_trackedState.psHash;
        bool terrain=IsTerrainVertexShader(g_trackedState.vsHash)||hash==0x1259eadbfaafb43eull||hash==0x8f5f50576e4cf36aull||hash==0x36df746731dcdefdull||hash==0x250ee869fc4be850ull;
        bool water=false;
        if(!terrain){
            water=true;
            for(UINT s=0;s<2;++s){ComPtr<IDirect3DBaseTexture9> t;D3DSURFACE_DESC desc{};
                if(FAILED(d->GetTexture(s,t.GetAddressOf()))||!t||t->GetType()!=D3DRTYPE_TEXTURE||
                    FAILED(static_cast<IDirect3DTexture9*>(t.Get())->GetLevelDesc(0,&desc))||
                    desc.Width!=(s?512u:8u)||desc.Height!=(s?512u:64u)){water=false;break;}}
        }
        if(!terrain&&!water)return;
        Capture(d,f,water?5:0,draw);
    }
    template<class Draw> void Capture(IDirect3DDevice9* d,const FrameContext& f,UINT matrixRegister,Draw draw) {
        if(!Ensure(d))return;
        float saved[9][4]{},mv[4][4]{};
        if(FAILED(d->GetVertexShaderConstantF(0,saved[0],9))||FAILED(d->GetVertexShaderConstantF(matrixRegister,mv[0],4)))return;
        ScopedRenderState state(d);
        ComPtr<IDirect3DSurface9> extra[3];
        for(UINT i=1;i<4;++i){d->GetRenderTarget(i,extra[i-1].GetAddressOf());d->SetRenderTarget(i,nullptr);}
        for(UINT i=0;i<4;++i)d->SetTexture(i,nullptr);
        DepthCapture::Instance().RawSetDepth(d,nullptr);
        d->SetRenderTarget(0,surface.Get());DepthCapture::Instance().RawSetDepth(d,depth.Get());
        D3DVIEWPORT9 vp{0,0,Size,Size,0,1};d->SetViewport(&vp);
        auto tick=GetTickCount64();
        if(!valid||fabsf(f.cameraPosition.x-origin[0])>128||fabsf(f.cameraPosition.y-origin[1])>128||fabsf(f.cameraPosition.z-origin[2])>512||tick-lastTick>2000){
            origin[0]=std::floor(f.cameraPosition.x/128)*128;
            origin[1]=std::floor(f.cameraPosition.y/128)*128;origin[2]=f.cameraPosition.z;origin[3]=1.f/512;
            d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0,1,0);valid=true;terrainReady=false;
        }
        lastTick=tick;
        for(auto s:{D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_SRGBWRITEENABLE,D3DRS_CLIPPLANEENABLE})d->SetRenderState(s,FALSE);
        d->SetRenderState(D3DRS_ZENABLE,TRUE);d->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);d->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
        d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_COLORWRITEENABLE,15);d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID);
        d->SetRenderState(D3DRS_DEPTHBIAS,0);d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS,0);
        d->SetVertexShader(vs.Get());d->SetPixelShader(ps.Get());
        d->SetVertexShaderConstantF(0,mv[0],4);
        float inv[3][4]{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)inv[i][j]=f.inverseView.m[i][j];
        float camera[4]={f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,0};
        d->SetVertexShaderConstantF(4,inv[0],3);d->SetVertexShaderConstantF(7,camera,1);d->SetVertexShaderConstantF(8,origin,1);
        draw();if(matrixRegister==0)terrainReady=true;d->SetVertexShaderConstantF(0,saved[0],9);
        state.Restore();for(UINT i=1;i<4;++i)d->SetRenderTarget(i,extra[i-1].Get());
    }
};
}
