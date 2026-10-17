// Production weather rendering with no atmosphere/cloud renderer instantiated.
// Temporal image comparisons catch the sign of motion, not merely 'pixels changed'.
#include "../WeatherVisuals.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
constexpr UINT size=512;
void Check(HRESULT hr) {
    if(FAILED(hr)){std::printf("HRESULT=%08lx\n",hr);throw std::runtime_error("D3D call");}
}
void Require(bool value,const char* name) {
    if(!value)throw std::runtime_error(name);
    std::printf("PASS %s\n",name);
}
HRESULT WINAPI SetDepth(IDirect3DDevice9* d,IDirect3DSurface9* s){return d->SetDepthStencilSurface(s);}
HRESULT WINAPI GetDepth(IDirect3DDevice9* d,IDirect3DSurface9** s){return d->GetDepthStencilSurface(s);}
int main() try {
    Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
    wchar_t path[MAX_PATH]{};GetSystemDirectoryW(path,MAX_PATH);wcscat_s(path,L"\\d3d9.dll");
    auto runtime=LoadLibraryW(path);Require(runtime!=nullptr,"system runtime loaded, not the proxy under test");
    auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(runtime,"Direct3DCreate9"));
    Require(create!=nullptr,"Direct3DCreate9 found");
    ComPtr<IDirect3D9> api;api.Attach(create(D3D_SDK_VERSION));
    HWND window=CreateWindowW(L"STATIC",L"Weather regression",WS_OVERLAPPEDWINDOW,0,0,size,size,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=size;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    ComPtr<IDirect3DDevice9> d;
    HRESULT hr=api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,d.GetAddressOf());
    if(FAILED(hr)){std::printf("SKIP GPU tests: no D3D9 HAL (hr=%08lx)\n",hr);return 77;}
    renderer::DepthCapture::Instance().SetFunctions(SetDepth,GetDepth);
    weathervisuals::Configure(L".\\");weathervisuals::EnsureShaders(d.Get());
    Require(bool(weathervisuals::precipitationPS)&&bool(weathervisuals::lensDropletsPS),"production precipitation and lens shaders compile and install");
    auto texture=[&](UINT width,UINT height,DWORD value,bool band=false) {
        ComPtr<IDirect3DTexture9> t;Check(d->CreateTexture(width,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,t.GetAddressOf(),nullptr));
        D3DLOCKED_RECT r{};Check(t->LockRect(0,&r,nullptr,0));
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
            reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=(band&&(y<height/4||y>height*3/4))?0:value;
        Check(t->UnlockRect(0));return t;
    };
    auto black=texture(1,1,0xff000000),clear=texture(1,1,0),drop=texture(16,128,0xffffffff,true);
    ComPtr<IDirect3DTexture9> depth;
    Check(d->CreateTexture(1,1,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,depth.GetAddressOf(),nullptr));
    ComPtr<IDirect3DSurface9> back,readback;Check(d->GetRenderTarget(0,back.GetAddressOf()));
    Check(d->CreateOffscreenPlainSurface(size,size,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,readback.GetAddressOf(),nullptr));
    auto pixels=[&]() {
        Check(d->GetRenderTargetData(back.Get(),readback.Get()));D3DLOCKED_RECT r{};Check(readback->LockRect(&r,nullptr,D3DLOCK_READONLY));
        std::vector<DWORD> result(size*size);
        for(UINT y=0;y<size;++y)memcpy(result.data()+y*size,static_cast<BYTE*>(r.pBits)+y*r.Pitch,size*4);
        Check(readback->UnlockRect());return result;
    };
    auto nonblack=[](const std::vector<DWORD>& image){unsigned n=0;for(DWORD p:image)n+=(p&0xffffff)!=0;return n;};
    auto difference=[](const std::vector<DWORD>& a,const std::vector<DWORD>& b,int dx,int dy) {
        double sum=0;unsigned n=0;
        for(int y=20;y<int(size)-20;++y)for(int x=20;x<int(size)-20;++x) {
            sum+=std::abs(int(a[y*size+x]&255)-int(b[(y+dy)*size+x+dx]&255));++n;
        }
        return sum/n;
    };
    struct Vertex{float x,y,z,w,u,v;};
    Vertex quad[]={{-.5f,-.5f,0,1,0,0},{size-.5f,-.5f,0,1,1,0},{-.5f,size-.5f,0,1,0,1},{size-.5f,size-.5f,0,1,1,1}};
    // Five units exposes only the nearest precipitation layer; all farther
    // layers are occluded. Mist is disabled (haze 0) so movement has one exact
    // answer; the three trailing arguments exist for the mist-gate assertions
    // below, which need the term measurable and the depth-valid flag in c1.w
    // under their own control.
    auto render=[&](int mode,float time,float sceneZ,float minZ=0.f,float maxZ=1.f,float windX=0.f,
                    float haze=0.f,float depthValid=1.f,IDirect3DTexture9* mistTex=nullptr) {
        D3DLOCKED_RECT r{};Check(depth->LockRect(0,&r,nullptr,0));
        *static_cast<float*>(r.pBits)=minZ+(maxZ-minZ)*(1.001f-.1f/sceneZ);Check(depth->UnlockRect(0));
        Check(d->SetRenderTarget(0,back.Get()));D3DVIEWPORT9 vp{0,0,size,size,0,1};Check(d->SetViewport(&vp));
        for(auto state:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,
            D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_CLIPPLANEENABLE})Check(d->SetRenderState(state,FALSE));
        Check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));Check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));Check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));
        Check(d->SetVertexShader(nullptr));Check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));Check(d->SetPixelShader(weathervisuals::precipitationPS.Get()));
        Check(d->SetTexture(0,black.Get()));Check(d->SetTexture(1,depth.Get()));Check(d->SetTexture(2,drop.Get()));Check(d->SetTexture(3,mistTex?mistTex:clear.Get()));
        for(DWORD slot=0;slot<4;++slot){Check(d->SetSamplerState(slot,D3DSAMP_MINFILTER,slot==1?D3DTEXF_POINT:D3DTEXF_LINEAR));Check(d->SetSamplerState(slot,D3DSAMP_MAGFILTER,slot==1?D3DTEXF_POINT:D3DTEXF_LINEAR));Check(d->SetSamplerState(slot,D3DSAMP_MIPFILTER,D3DTEXF_NONE));Check(d->SetSamplerState(slot,D3DSAMP_ADDRESSU,slot<2?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP));Check(d->SetSamplerState(slot,D3DSAMP_ADDRESSV,slot<2?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP));Check(d->SetSamplerState(slot,D3DSAMP_SRGBTEXTURE,FALSE));}
        // c0..c5 carry the raw values; c6..c8 carry the same phases
        // wrapped exactly as BeforeDraw does (the motion asserts below
        // measure the 8px/4px steps those formulas produce).
        // c1.w is the depth-valid flag the mist gate reads; production fills it
        // from hasCapturedDepth, and c2.z is the haze the mist is scaled by.
        float constants[6][4]={{1.001f,-.1f,1,1},{time,1,float(mode),depthValid},{windX,0,haze,0},{float(size),float(size),1.f/size,1.f/size},{1,1,1,1},{minZ,1/(maxZ-minZ),0,0}};
        auto wrapped = renderer::ComputeWrappedPhases(double(time),double(windX),0.0,mode);
        Check(d->SetPixelShaderConstantF(0,constants[0],6));
        Check(d->SetPixelShaderConstantF(6,wrapped.c6,1));
        Check(d->SetPixelShaderConstantF(7,wrapped.c7,1));
        Check(d->SetPixelShaderConstantF(8,wrapped.c8,1));
        Check(d->SetPixelShaderConstantF(9,wrapped.c9,1));
        Check(d->SetPixelShaderConstantF(10,wrapped.c10,1));Check(d->BeginScene());Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex)));Check(d->EndScene());return pixels();
    };
    auto rain0=render(1,0,5),rain1=render(1,1.f/60,5);
    Require(nonblack(rain0)>1000,"rain remains visible with no cloud or atmosphere renderer");
    const double down=difference(rain0,rain1,0,8),up=difference(rain0,rain1,0,-8);
    std::printf("rain translation error down=%.5f up=%.5f\n",down,up);
    Require(down<.3&&up>down+2,"actual rain pixels move downward between frames");
    // c2.xy is now the INTEGRATED displacement; changing current wind at a
    // late session time must not multiply that entire age into the offset.
    auto wind0=render(1,0,5,0,1,0),wind1=render(1,1.f/60,5,0,1,1.2f/60);
    Require(difference(wind0,wind1,8,8)<.3,"rain drifts with positive projected wind, not against it");
    unsigned longest=0;
    for(UINT x=0;x<size;++x){unsigned run=0;for(UINT y=0;y<size;++y){run=(rain0[y*size+x]&255)>2?run+1:0;longest=std::max(longest,run);}}
    std::printf("longest rain streak=%u pixels of %u\n",longest,size);
    Require(longest<=22,"rain streaks are compact rather than screen-height beams");
    auto snow0=render(2,0,5),snow1=render(2,1.f/60,5);
    Require(difference(snow0,snow1,0,4)<.3&&difference(snow0,snow1,0,-4)>2,"shared snow UV motion also falls downward");
    Require(nonblack(render(1,0,1))==0,"near foreground fully occludes precipitation");
    auto compressed=render(1,0,5,0,.94f),offset=render(1,0,5,.1f,.94f);
    Require(difference(rain0,compressed,0,0)<.1&&difference(rain0,offset,0,0)<.1,"rain occlusion normalizes viewport MinZ and MaxZ");

    // THE MIST GATE - the second half of "rain overflows the full screen", reproduced exactly. With
    // no INTZ capture the neutral depth texture reads 1.0 and sceneZ comes back 100000, which is past
    // BOTH of the mist's distance gates, so the mist used to run at full strength over every pixel of
    // the screen whenever depth was missing - which above 1x multisampling is always. c1.w now says
    // whether depth exists and the mist is gated on it. Slot 3 has to be a white texture for any of
    // this to be measurable: production binds a soft noise, this harness binds a 1x1 clear, and with
    // a zero alpha the mist contributes nothing whatever the gate says, which is why no assertion
    // here ever noticed the term running full-screen. Haze is at its authored maximum (3.0) so the
    // term is ~0.24 of a white mist, i.e. 0.8*0.08*3.0 = ~49 of 255 levels in red, against the 20
    // the first assertion asks for.
    const auto mist=texture(1,1,0xffffffff);
    auto withDepthHaze=render(1,0,40,0,1,0,3.0f,1.f,mist.Get());
    auto withDepthNoHaze=render(1,0,40,0,1,0,0.f,1.f,mist.Get());
    auto noDepthHaze=render(1,0,100000,0,1,0,3.0f,0.f,mist.Get());
    auto noDepthNoHaze=render(1,0,100000,0,1,0,0.f,0.f,mist.Get());
    // haze is read nowhere else in the stage, so the two no-depth renders are not "close", they are
    // the same image: the gate removes the term rather than scaling it down. Printed in 8-bit levels,
    // which is the unit difference() works in, and the expected first number is 0.8*0.08*3.0*255 = 49.
    std::printf("mist gate: with depth %.3f levels, without depth %.3f levels\n",
                difference(withDepthHaze,withDepthNoHaze,0,0),difference(noDepthHaze,noDepthNoHaze,0,0));
    Require(difference(withDepthHaze,withDepthNoHaze,0,0)>20.0,
            "with depth captured, haze still makes mist at mid distance");
    Require(difference(noDepthHaze,noDepthNoHaze,0,0)<.5,
            "with no depth captured, haze makes no mist however far sceneZ reads");

    // Run the actual integration path with native snow, without INTZ/clouds, and
    // with hostile inherited UI state. Snow uses its own texture, not rain's.
    weathervisuals::enabled=weathervisuals::effectEnabled=weathervisuals::active=true;
    weathervisuals::nativeWeatherMode=2;weathervisuals::lensDroplets=false;
    weathervisuals::intensity=1;weathervisuals::atmosphereHaze=0;
    renderer::FrameContext::Current().wind.enabled=false;
    weathervisuals::rainDropTex=clear;weathervisuals::snowFlakeTex=drop;weathervisuals::snowMistTex=clear;
    renderer::FrameContext::Current().cameraValid=false;
    renderer::g_trackedState.vsHash=0xd9e7756460af6296ull;
    const auto tracked=renderer::g_trackedState;
    Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0));
    Check(d->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE));RECT rect{0,0,1,1};Check(d->SetScissorRect(&rect));
    Check(d->SetRenderState(D3DRS_FOGENABLE,TRUE));Check(d->SetRenderState(D3DRS_STENCILENABLE,TRUE));
    Check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_WIREFRAME));Check(d->SetRenderState(D3DRS_SRGBWRITEENABLE,TRUE));
    Check(d->SetSamplerState(3,D3DSAMP_ADDRESSU,D3DTADDRESS_MIRROR));Check(d->SetSamplerState(3,D3DSAMP_SRGBTEXTURE,TRUE));
    float sentinel[4]={17,18,19,20};Check(d->SetPixelShaderConstantF(5,sentinel,1));
    weathervisuals::composedThisFrame=false;Check(d->BeginScene());weathervisuals::BeforeDraw(d.Get());Check(d->EndScene());
    Require(nonblack(pixels())>1000,"detected snow uses snow texture and renders without cloud/depth passes");
    DWORD state=0;Check(d->GetRenderState(D3DRS_SCISSORTESTENABLE,&state));Require(state==TRUE,"scissor state restored after weather");
    Check(d->GetRenderState(D3DRS_FILLMODE,&state));Require(state==D3DFILL_WIREFRAME,"fill mode restored after weather");
    Check(d->GetRenderState(D3DRS_FOGENABLE,&state));Require(state==TRUE,"native fog restored after weather");
    Check(d->GetRenderState(D3DRS_STENCILENABLE,&state));Require(state==TRUE,"stencil restored after weather");
    Check(d->GetRenderState(D3DRS_SRGBWRITEENABLE,&state));Require(state==TRUE,"sRGB write state restored after weather");
    Check(d->GetSamplerState(3,D3DSAMP_ADDRESSU,&state));Require(state==D3DTADDRESS_MIRROR,"mist sampler addressing restored");
    Check(d->GetSamplerState(3,D3DSAMP_SRGBTEXTURE,&state));Require(state==TRUE,"mist sampler color space restored");
    float c[4]{};Check(d->GetPixelShaderConstantF(5,c,1));Require(memcmp(c,sentinel,sizeof(c))==0,"depth-range constants restored");
    Require(renderer::g_trackedState.vsHash==tracked.vsHash&&renderer::g_trackedState.currentPS==tracked.currentPS,"tracked game pipeline preserved");
    weathervisuals::nativeWeatherMode=0;weathervisuals::composedThisFrame=false;
    Check(d->BeginScene());weathervisuals::BeforeDraw(d.Get());Check(d->EndScene());
    Require(!weathervisuals::composedThisFrame,"no native weather means no added weather pass");
    weathervisuals::rainDropTex.Reset();weathervisuals::CreateProceduralFallback(d.Get());
    D3DLOCKED_RECT r{};Check(weathervisuals::rainDropTex->LockRect(0,&r,nullptr,D3DLOCK_READONLY));unsigned maxAlpha=0;
    for(unsigned y=0;y<128;++y)for(unsigned x=0;x<16;++x)maxAlpha=std::max(maxAlpha,unsigned(reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]>>24));
    Check(weathervisuals::rainDropTex->UnlockRect(0));Require(maxAlpha>0&&maxAlpha<=38,"fallback rain matches the low-opacity authored mask");
    weathervisuals::Reset(d.Get());std::puts("Weather GPU regressions passed");return 0;
} catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
