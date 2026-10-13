// Tests the production water Scope, not a second implementation of it.
#include "../WaterEffect.h"
#include "../src/Scene/DrawCallClassifier.h"
// The capture provider is controlled here; Prepare itself is production code.
namespace volume {
inline bool internal=false,ready=false,composed=false;
inline IDirect3DDevice9* owner=nullptr;
inline Microsoft::WRL::ComPtr<IDirect3DSurface9> target;
inline Microsoft::WRL::ComPtr<IDirect3DTexture9> depth;
inline float constants[3][4]{};
}
#include "../WaterReflection.h"
#include <stdexcept>
#include <cstdio>

using Microsoft::WRL::ComPtr;
void Check(HRESULT hr) {
    if(FAILED(hr)) { std::printf("HRESULT=%08lx\n",hr);throw std::runtime_error("D3D call"); }
}
void Require(bool value,const char* name) {
    if(!value)throw std::runtime_error(name);
    std::printf("PASS %s\n",name);
}
int main() try {
    watereffect::Configure(L".\\");
    Require(watereffect::enabled&&!watereffect::code.empty()&&!watereffect::vertexCode.empty(),"production water compiles");
    // The test sits beside the built proxy DLL. Load the system runtime
    // explicitly so the proxy cannot hook and double-apply our test scopes.
    wchar_t systemPath[MAX_PATH]{};GetSystemDirectoryW(systemPath,MAX_PATH);
    wcscat_s(systemPath,L"\\d3d9.dll");
    HMODULE runtime=LoadLibraryW(systemPath);Require(runtime!=nullptr,"system D3D9 runtime loaded");
    auto createD3D=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(runtime,"Direct3DCreate9"));
    Require(createD3D!=nullptr,"system Direct3DCreate9 found");
    ComPtr<IDirect3D9> api;api.Attach(createD3D(D3D_SDK_VERSION));
    Require(bool(api),"Direct3D9 available");
    HWND window=CreateWindowW(L"STATIC",L"Water regression",WS_OVERLAPPEDWINDOW,0,0,128,128,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=128;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    ComPtr<IDirect3DDevice9> d;
    const HRESULT create=api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,d.GetAddressOf());
    if(FAILED(create)) { std::printf("SKIP GPU tests: no D3D9 HAL (hr=%08lx)\n",create);return 77; }
    ComPtr<IDirect3DPixelShader9> originalPS;
    ComPtr<IDirect3DVertexShader9> originalVS;
    ComPtr<ID3DBlob> nativeCode;
    const char* native="float4 main():COLOR0{return float4(.1,.2,.3,1);}";
    Check(D3DCompile(native,strlen(native),nullptr,nullptr,nullptr,"main","ps_3_0",0,0,nativeCode.GetAddressOf(),nullptr));
    Check(d->CreatePixelShader((DWORD*)nativeCode->GetBufferPointer(),originalPS.GetAddressOf()));
    Check(d->CreateVertexShader(watereffect::vertexCode.data(),originalVS.GetAddressOf()));
    Check(d->SetPixelShader(originalPS.Get()));Check(d->SetVertexShader(originalVS.Get()));
    // Deliberately do not seed g_trackedState: Scope must restore actual device state.
    auto texture=[&](UINT w,UINT h,DWORD color) {
        ComPtr<IDirect3DTexture9> t;Check(d->CreateTexture(w,h,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,t.GetAddressOf(),nullptr));
        D3DLOCKED_RECT r{};Check(t->LockRect(0,&r,nullptr,0));
        for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)
            reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=color;
        Check(t->UnlockRect(0));return t;
    };
    auto base=texture(8,64,0xff406080),surface=texture(512,512,0xff101820),sentinel=texture(4,4,0xff808080);
    Check(d->SetTexture(0,base.Get()));Check(d->SetTexture(1,surface.Get()));
    for(DWORD slot=2;slot<4;++slot) {
        Check(d->SetTexture(slot,sentinel.Get()));
        Check(d->SetSamplerState(slot,D3DSAMP_MINFILTER,D3DTEXF_POINT));
        Check(d->SetSamplerState(slot,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
        Check(d->SetSamplerState(slot,D3DSAMP_MIPFILTER,D3DTEXF_POINT));
        Check(d->SetSamplerState(slot,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP));
        Check(d->SetSamplerState(slot,D3DSAMP_ADDRESSV,D3DTADDRESS_MIRROR));
        Check(d->SetSamplerState(slot,D3DSAMP_SRGBTEXTURE,TRUE));
    }
    const D3DSAMPLERSTATETYPE samplerStates[]={D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,
        D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_SRGBTEXTURE};
    DWORD samplers[2][6]{};
    for(unsigned slot=0;slot<2;++slot)for(unsigned i=0;i<6;++i)
        Check(d->GetSamplerState(2+slot,samplerStates[i],&samplers[slot][i]));
    float savedPS[36],savedVS[4]={71,72,73,74},savedLights[68];
    for(unsigned i=0;i<36;++i)savedPS[i]=float(i+1);
    for(unsigned i=0;i<68;++i)savedLights[i]=float(i+80);
    Check(d->SetPixelShaderConstantF(200,savedPS,9));Check(d->SetVertexShaderConstantF(200,savedVS,1));
    Check(d->SetPixelShaderConstantF(180,savedLights,17));
    float nativeConstants[36][4]{};
    for(int i=0;i<4;++i){nativeConstants[i][i]=1;nativeConstants[5+i][i]=1;}
    nativeConstants[4][1]=nativeConstants[4][2]=1;
    nativeConstants[9][0]=nativeConstants[10][1]=nativeConstants[13][0]=nativeConstants[14][1]=1;
    nativeConstants[33][2]=1;
    for(int i=0;i<3;++i){nativeConstants[34][i]=1;nativeConstants[35][i]=.2f;}
    Check(d->SetVertexShaderConstantF(0,nativeConstants[0],36));
    float fog[4]={.1f,.2f,.3f,1};Check(d->SetPixelShaderConstantF(4,fog,1));
    D3DVIEWPORT9 viewport{0,0,128,128,0,.94f};Check(d->SetViewport(&viewport));
    Check(d->SetRenderState(D3DRS_COLORWRITEENABLE1,0));Check(d->SetRenderState(D3DRS_COLORWRITEENABLE2,0));
    auto restored=[&]() {
        ComPtr<IDirect3DPixelShader9> ps;ComPtr<IDirect3DVertexShader9> vs;
        Check(d->GetPixelShader(ps.GetAddressOf()));Check(d->GetVertexShader(vs.GetAddressOf()));
        Require(ps.Get()==originalPS.Get()&&vs.Get()==originalVS.Get(),"actual native shaders restored");
        float values[68];Check(d->GetPixelShaderConstantF(200,values,9));
        Require(memcmp(values,savedPS,sizeof(savedPS))==0,"pixel controls restored");
        Check(d->GetVertexShaderConstantF(200,values,1));Require(memcmp(values,savedVS,sizeof(savedVS))==0,"vertex controls restored");
        Check(d->GetPixelShaderConstantF(180,values,17));Require(memcmp(values,savedLights,sizeof(savedLights))==0,"local light constants restored");
        for(unsigned slot=0;slot<2;++slot) {
            ComPtr<IDirect3DBaseTexture9> t;Check(d->GetTexture(2+slot,t.GetAddressOf()));
            Require(t.Get()==sentinel.Get(),"extra textures restored even without scene capture");
            for(unsigned i=0;i<6;++i){DWORD value=0;Check(d->GetSamplerState(2+slot,samplerStates[i],&value));
                Require(value==samplers[slot][i],"sampler state restored");}
        }
        D3DVIEWPORT9 v{};Check(d->GetViewport(&v));Require(memcmp(&v,&viewport,sizeof(v))==0,"viewport including MaxZ restored");
        for(auto state:{D3DRS_COLORWRITEENABLE1,D3DRS_COLORWRITEENABLE2}){DWORD value=1;Check(d->GetRenderState(state,&value));Require(value==0,"MRT write masks restored");}
    };
    watereffect::reflectionScene=watereffect::reflectionDepth=nullptr;
    {
        watereffect::Scope scope(d.Get());
        Require(scope.device==d.Get(),"replacement installs without scene/depth capture");
        float c[36];Check(d->GetPixelShaderConstantF(200,c,9));
        Require(c[23]==0&&c[13]<0&&c[19]>0,"missing capture has valid depth unpack constants");
        scope.Restore();scope.Restore(); // idempotent; destructor also restores
    }
    restored();
    // Depth/refraction/foam switches do not depend on reflections.
    watereffect::reflectionsEnabled=false;
    watereffect::reflectionScene=sentinel.Get();watereffect::reflectionDepth=sentinel.Get();
    watereffect::reflectionData[0]=1.001f;watereffect::reflectionData[1]=-.1f;
    watereffect::reflectionData[2]=watereffect::reflectionData[3]=1;
    watereffect::reflectionData[7]=.94f;
    {
        watereffect::Scope scope(d.Get());float c[36];Check(d->GetPixelShaderConstantF(200,c,9));
        Require(c[16]==0&&c[20]==0&&c[23]==1&&c[28]>0&&c[29]>0&&c[30]>0,"reflections off preserves refraction depth and foam controls");
    }
    restored();
    // Same shader/declaration pipeline, different material textures in both orders.
    renderer::DrawCallContext ctx;ctx.vertexShader=originalVS.Get();ctx.pixelShader=originalPS.Get();
    auto& classifier=renderer::DrawCallClassifier::Instance();float translation[3]{};
    for(int iteration=0;iteration<3;++iteration) {
        Check(d->SetTexture(1,sentinel.Get()));
        Require(!classifier.Classify(d.Get(),ctx,translation,false,0).isWater,"non-water material not poisoned by water cache");
        Check(d->SetTexture(1,surface.Get()));
        Require(classifier.Classify(d.Get(),ctx,translation,false,0).isWater,"water material not poisoned by non-water cache");
    }
    // Exercise the real pre-water copy with reflections off, inside a scene.
    // Refraction/depth/foam must still get a fresh image and finish must drop it.
    Check(d->GetRenderTarget(0,volume::target.GetAddressOf()));
    volume::owner=d.Get();volume::ready=true;volume::depth=sentinel;
    volume::constants[0][0]=1.001f;volume::constants[0][1]=-.1f;
    volume::constants[0][2]=volume::constants[0][3]=1;volume::constants[2][3]=.94f;
    waterreflection::Configure(L".\\");
    Check(d->BeginScene());waterreflection::Prepare(d.Get());Check(d->EndScene());
    Require(waterreflection::ready&&watereffect::reflectionScene&&watereffect::reflectionDepth,
        "pre-water scene copy works with reflections disabled");
    waterreflection::Finish();
    Require(!waterreflection::ready&&!watereffect::reflectionScene&&!watereffect::reflectionDepth,
        "frame end clears borrowed capture inputs");
    // An offscreen draw with water textures must not copy the wrong scene.
    ComPtr<IDirect3DTexture9> offscreen;ComPtr<IDirect3DSurface9> offscreenRT;
    Check(d->CreateTexture(128,128,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,offscreen.GetAddressOf(),nullptr));
    Check(offscreen->GetSurfaceLevel(0,offscreenRT.GetAddressOf()));Check(d->SetRenderTarget(0,offscreenRT.Get()));
    waterreflection::Prepare(d.Get());Require(!waterreflection::ready,"offscreen water cannot capture the main scene");
    Check(d->SetRenderTarget(0,volume::target.Get()));Check(d->SetViewport(&viewport));
    waterreflection::Prepare(d.Get());Require(waterreflection::ready,"world capture available again");
    Check(d->SetRenderTarget(0,offscreenRT.Get()));waterreflection::Prepare(d.Get());
    Require(!watereffect::reflectionScene&&!watereffect::reflectionDepth,"ready world capture not exposed to offscreen water");
    Check(d->SetRenderTarget(0,volume::target.Get()));Check(d->SetViewport(&viewport));waterreflection::Prepare(d.Get());
    Require(watereffect::reflectionScene!=nullptr,"owned scene re-exposed on returning to world target");
    waterreflection::Reset(d.Get());
    Require(!waterreflection::resources.scene&&!waterreflection::resources.surface,"capture reset releases default-pool resources");
    // Render the complete shader, not just a diagnostic extraction, without capture.
    struct Vertex {float x,y,z,nx,ny,nz;DWORD color;float u,v,s,t;};
    Vertex vertices[]={{-1,1,1,0,0,-1,0xffffffff,0,0,0,0},{1,1,1,0,0,-1,0xffffffff,1,0,1,0},
        {-1,-1,1,0,0,-1,0xffffffff,0,1,0,1},{1,-1,1,0,0,-1,0xffffffff,1,1,1,1}};
    Check(d->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX2));
    for(auto state:{D3DRS_ZENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_FOGENABLE})Check(d->SetRenderState(state,FALSE));
    Check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
    Check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));
    ComPtr<IDirect3DSurface9> back,readback;Check(d->GetRenderTarget(0,back.GetAddressOf()));
    Check(d->CreateOffscreenPlainSurface(128,128,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,readback.GetAddressOf(),nullptr));
    bool wroteMRT=false;
    auto pixel=[&](float debug) {
        watereffect::waterDebugMode=debug;
        Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0));Check(d->BeginScene());
        {watereffect::Scope scope(d.Get());Require(scope.device==d.Get(),"production draw uses modern shader");wroteMRT=scope.maskBound&&scope.depthBound;Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex)));}
        Check(d->EndScene());Check(d->GetRenderTargetData(back.Get(),readback.Get()));
        D3DLOCKED_RECT r{};Check(readback->LockRect(&r,nullptr,D3DLOCK_READONLY));
        DWORD value=reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+64*r.Pitch)[64];Check(readback->UnlockRect());return value;
    };
    watereffect::reflectionScene=watereffect::reflectionDepth=nullptr;
    Require((pixel(1)&0xffffff)==0xff00ff,"debug mode proves replacement reaches the framebuffer");
    Require((pixel(0)&0xffffff)!=0,"modern water without capture is finite and nonblack");
    restored();
    // The optional atmosphere MRTs must never corrupt the main water draw.
    ComPtr<IDirect3DTexture9> mask,waterDepth;ComPtr<IDirect3DSurface9> maskRT,depthRT;
    Check(d->CreateTexture(128,128,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,mask.GetAddressOf(),nullptr));
    Check(mask->GetSurfaceLevel(0,maskRT.GetAddressOf()));
    HRESULT depthHR=d->CreateTexture(128,128,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,waterDepth.GetAddressOf(),nullptr);
    if(SUCCEEDED(depthHR)) {
        Check(waterDepth->GetSurfaceLevel(0,depthRT.GetAddressOf()));
        Check(d->ColorFill(maskRT.Get(),nullptr,0));Check(d->ColorFill(depthRT.Get(),nullptr,0));
        watereffect::waterTargetSurface=back.Get();watereffect::waterMaskSurface=maskRT.Get();watereffect::waterDepthSurface=depthRT.Get();
        // Capability checks may decline MRTs on older hardware; color still renders.
        Require((pixel(1)&0xffffff)==0xff00ff,"optional MRTs do not break the water draw");restored();
        ComPtr<IDirect3DSurface9> checkRT;
        Require(d->GetRenderTarget(1,checkRT.GetAddressOf())==D3DERR_NOTFOUND,"mask slot restored to unbound");
        Require(d->GetRenderTarget(2,checkRT.GetAddressOf())==D3DERR_NOTFOUND,"depth slot restored to unbound");
        if(wroteMRT) {
            auto translucent=texture(8,64,0x80406080);Check(d->SetTexture(0,translucent.Get()));
            Check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE));
            Check(d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA));Check(d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA));
            Check(d->ColorFill(maskRT.Get(),nullptr,0));Check(d->ColorFill(depthRT.Get(),nullptr,0));
            pixel(0);
            Check(d->GetRenderTargetData(maskRT.Get(),readback.Get()));D3DLOCKED_RECT r{};
            Check(readback->LockRect(&r,nullptr,D3DLOCK_READONLY));
            DWORD coverage=reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+64*r.Pitch)[64]&255;
            Check(readback->UnlockRect());Require(coverage>=126&&coverage<=130,"water mask preserves fractional shoreline coverage");
            ComPtr<IDirect3DSurface9> floatReadback;
            Check(d->CreateOffscreenPlainSurface(128,128,D3DFMT_R32F,D3DPOOL_SYSTEMMEM,floatReadback.GetAddressOf(),nullptr));
            Check(d->GetRenderTargetData(depthRT.Get(),floatReadback.Get()));Check(floatReadback->LockRect(&r,nullptr,D3DLOCK_READONLY));
            float waterZ=reinterpret_cast<float*>(static_cast<BYTE*>(r.pBits)+64*r.Pitch)[64];Check(floatReadback->UnlockRect());
            Require(std::abs(waterZ-1.f)<.001f,"water surface depth is not attenuated by color alpha");
            Check(d->SetTexture(0,base.Get()));Check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));
        } else std::puts("SKIP optional water MRT readbacks: capability gate declined the pair");
        // Reproduce the old resize mismatch: reject the pair before mutating RTs.
        ComPtr<IDirect3DTexture9> smallTexture;ComPtr<IDirect3DSurface9> smallRT;
        Check(d->CreateTexture(64,64,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,smallTexture.GetAddressOf(),nullptr));
        Check(smallTexture->GetSurfaceLevel(0,smallRT.GetAddressOf()));watereffect::waterDepthSurface=smallRT.Get();
        {watereffect::Scope scope(d.Get());Require(!scope.maskBound&&!scope.depthBound,"mismatched resize pair not bound");}
        restored();watereffect::waterTargetSurface=watereffect::waterMaskSurface=watereffect::waterDepthSurface=nullptr;
    } else std::puts("SKIP optional R32F MRT checks: unsupported format");
    watereffect::effectEnabled=false;
    {watereffect::Scope scope(d.Get());Require(scope.device==nullptr,"feature disabled leaves native draw untouched");}
    watereffect::effectEnabled=true;
    watereffect::Reset(d.Get());Require(!watereffect::cachedReplacementPS&&!watereffect::cachedReplacementVS,"reset releases cached shaders");
    {watereffect::Scope scope(d.Get());Require(scope.device==d.Get(),"replacement recreated after reset");}
    restored();watereffect::Reset(d.Get());
    std::puts("Water GPU regressions passed");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
