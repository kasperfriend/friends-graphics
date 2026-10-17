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
    Require(bool(weathervisuals::rainPS)&&bool(weathervisuals::precipitationPS)&&bool(weathervisuals::lensDropletsPS),
        "production rain, snow/sand and lens shaders compile and install");
    // Native weather batch size varies with particle emission, not with the
    // server weather type. A small valid draw must refresh the rain signal.
    Require(weathervisuals::ClassifyNativeWeather(1,16,128,0xde9331cb560b9814ull,
        0x8832bcc5ed4ff2a6ull,0)==1 &&
        weathervisuals::ClassifyNativeWeather(1,64,64,0x50aa504baa78b237ull,
        0xec9ee827b8bf5afaull,0)==2 &&
        weathervisuals::ClassifyNativeWeather(1,64,64,0x50aa504baa78b237ull,
        0x8832bcc5ed4ff2a6ull,0)==3 &&
        weathervisuals::ClassifyNativeWeather(0,16,128,0xde9331cb560b9814ull,
        0x8832bcc5ed4ff2a6ull,0)==0 &&
        weathervisuals::ClassifyNativeWeather(1,16,128,0xde9331cb560b9814ull,
        0x8832bcc5ed4ff2a6ull,42)==0,
        "weather identity accepts small native batches but rejects zero/foreign draws");
    weathervisuals::nativeWeatherSeenThisFrame=1;
    weathervisuals::UpdateWeatherObservation(1000,true,false);
    weathervisuals::nativeWeatherSeenThisFrame=0;
    weathervisuals::UpdateWeatherObservation(7000,true,false);
    Require(weathervisuals::nativeWeatherMode==1,
        "a missed native rain batch cannot turn rain off after five seconds");
    // Steady rain may emit only occasional small batches; each one refreshes
    // the observation while clear weather eventually releases the latch.
    for(DWORD tick=12000;tick<120000;tick+=12000) {
        weathervisuals::nativeWeatherSeenThisFrame=1;
        weathervisuals::UpdateWeatherObservation(tick,true,false);
        weathervisuals::nativeWeatherSeenThisFrame=0;
        weathervisuals::UpdateWeatherObservation(tick+9000,true,false);
        Require(weathervisuals::nativeWeatherMode==1,"steady intermittent rain stays active");
    }
    weathervisuals::UpdateWeatherObservation(120000+weathervisuals::nativeWeatherHoldMs,true,false);
    Require(weathervisuals::nativeWeatherMode==0,"rain eventually clears without any further native evidence");
    weathervisuals::nativeWeatherSeenThisFrame=1;
    weathervisuals::UpdateWeatherObservation(200000,true,false);
    weathervisuals::nativeWeatherSeenThisFrame=0;
    weathervisuals::UpdateWeatherObservation(200000+weathervisuals::nativeWeatherHoldMs+1,false,false);
    Require(weathervisuals::nativeWeatherMode==1&&weathervisuals::weatherSuspended,
        "focus loss holds a detected storm");
    weathervisuals::UpdateWeatherObservation(240000,true,false);
    Require(weathervisuals::nativeWeatherMode==1&&!weathervisuals::weatherSuspended,
        "returning to the game restarts the evidence window");
    weathervisuals::nativeWeatherMode=0;
    weathervisuals::nativeWeatherSeenThisFrame=0;
    weathervisuals::weatherSuspended=false;
    weathervisuals::lastNativeWeatherTick=0;
    auto texture=[&](UINT width,UINT height,DWORD value,bool band=false) {
        ComPtr<IDirect3DTexture9> t;Check(d->CreateTexture(width,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,t.GetAddressOf(),nullptr));
        D3DLOCKED_RECT r{};Check(t->LockRect(0,&r,nullptr,0));
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
            reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]=(band&&(y<height/4||y>height*3/4))?0:value;
        Check(t->UnlockRect(0));return t;
    };
    auto black=texture(1,1,0xff000000),bright=texture(1,1,0xffffffff),
         clear=texture(1,1,0),drop=texture(16,128,0xffffffff,true);
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
    // Five units exposes only the nearest particle layer; farther layers
    // are occluded. Compare real shader pixels, not a substitute model.
    auto render=[&](int mode,float time,float sceneZ,float minZ=0.f,float maxZ=1.f,
                    float windX=0.f,bool debugMask=true,bool brightScene=false) {
        D3DLOCKED_RECT r{};Check(depth->LockRect(0,&r,nullptr,0));
        *static_cast<float*>(r.pBits)=minZ+(maxZ-minZ)*(1.001f-.1f/sceneZ);Check(depth->UnlockRect(0));
        Check(d->SetRenderTarget(0,back.Get()));D3DVIEWPORT9 vp{0,0,size,size,0,1};Check(d->SetViewport(&vp));
        for(auto state:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,
            D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_CLIPPLANEENABLE})Check(d->SetRenderState(state,FALSE));
        Check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));Check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));Check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));
        Check(d->SetVertexShader(nullptr));Check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));Check(d->SetPixelShader(weathervisuals::precipitationPS.Get()));
        Check(d->SetTexture(0,brightScene?bright.Get():black.Get()));Check(d->SetTexture(1,depth.Get()));Check(d->SetTexture(2,drop.Get()));Check(d->SetTexture(3,clear.Get()));
        for(DWORD slot=0;slot<4;++slot){Check(d->SetSamplerState(slot,D3DSAMP_MINFILTER,slot==1?D3DTEXF_POINT:D3DTEXF_LINEAR));Check(d->SetSamplerState(slot,D3DSAMP_MAGFILTER,slot==1?D3DTEXF_POINT:D3DTEXF_LINEAR));Check(d->SetSamplerState(slot,D3DSAMP_MIPFILTER,D3DTEXF_NONE));Check(d->SetSamplerState(slot,D3DSAMP_ADDRESSU,slot<2?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP));Check(d->SetSamplerState(slot,D3DSAMP_ADDRESSV,slot<2?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP));Check(d->SetSamplerState(slot,D3DSAMP_SRGBTEXTURE,FALSE));}
        if(mode==1) {
            const float dt = time * (1080.f/float(size)) / 620.f;
            renderer::rainfield::Motion motion;
            motion.Advance(dt,1.f,{windX,0.f});
            float phases[3][4]{};
            for(unsigned i=0;i<3;++i)motion.Layer(i,phases[i]);
            const float vertical=renderer::rainfield::VerticalSpeed(1.f,{windX,0.f});
            const float magnitude=std::hypot(windX,vertical);
            float c0[4]={1.001f,-.1f,1,1};
            float c1[4]={float(size),float(size),1.f/size,1.f/size};
            float c2[4]={1,1,1,1};
            float c3[4]={1,1,1,1};
            float c4[4]={1,0,1,debugMask?1.f:0.f}; // particle mask or final scene composite
            float c5[4]={windX/magnitude,vertical/magnitude,windX,0};
            float c9[4]={minZ,1.f/(maxZ-minZ),0,0};
            Check(d->SetPixelShader(weathervisuals::rainPS.Get()));
            Check(d->SetPixelShaderConstantF(0,c0,1));Check(d->SetPixelShaderConstantF(1,c1,1));
            Check(d->SetPixelShaderConstantF(2,c2,1));Check(d->SetPixelShaderConstantF(3,c3,1));
            Check(d->SetPixelShaderConstantF(4,c4,1));Check(d->SetPixelShaderConstantF(5,c5,1));
            float axes[3][4]{};
            for(unsigned i=0;i<3;++i) {
                const auto axis=motion.Direction(i,{windX,0.f},1.f);
                axes[i][0]=axis.x;axes[i][1]=axis.y;
            }
            Check(d->SetPixelShaderConstantF(6,phases[0],3));Check(d->SetPixelShaderConstantF(9,c9,1));
            Check(d->SetPixelShaderConstantF(10,axes[0],3));
            Check(d->BeginScene());Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex)));
            Check(d->EndScene());return pixels();
        }
        // c0..c5 carry the raw values; c6..c8 carry the same phases
        // wrapped exactly as BeforeDraw does (the motion asserts below
        // measure the 8px/4px steps those formulas produce).
        float constants[6][4]={{1.001f,-.1f,1,1},{time,1,float(mode),1},{windX,0,0,0},{float(size),float(size),1.f/size,1.f/size},{1,1,1,1},{minZ,1/(maxZ-minZ),0,0}};
        auto wrapped = renderer::ComputeWrappedPhases(double(time),double(windX),0.0,mode);
        Check(d->SetPixelShaderConstantF(0,constants[0],6));
        Check(d->SetPixelShaderConstantF(6,wrapped.c6,1));
        Check(d->SetPixelShaderConstantF(7,wrapped.c7,1));
        Check(d->SetPixelShaderConstantF(8,wrapped.c8,1));
        Check(d->SetPixelShaderConstantF(9,wrapped.c9,1));
        Check(d->SetPixelShaderConstantF(10,wrapped.c10,1));Check(d->BeginScene());Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex)));Check(d->EndScene());return pixels();
    };
    auto rain0=render(1,0,5),rain1=render(1,4,5);
    Require(nonblack(rain0)>100,"rain remains visible with no cloud or atmosphere renderer");
    Require(nonblack(render(1,0,100))>nonblack(rain0),
        "fine distant drops add depth behind the sparse foreground streaks");
    const double down=difference(rain0,rain1,0,4),up=difference(rain0,rain1,0,-4);
    std::printf("rain translation error down=%.5f up=%.5f\n",down,up);
    Require(down<.2&&up>down+.35,"actual rain pixels move downward between frames");
    // Same tilted particles at both times: horizontal + downward translation
    // follows the projected wind rather than moving against it.
    auto wind0=render(1,0,5,0,1,620),wind1=render(1,4,5,0,1,620);
    Require(difference(wind0,wind1,4,4)<.2 &&
            difference(wind0,wind1,-4,4)>.35,
            "rain drifts with positive projected wind, not against it");
    unsigned longest=0;
    for(UINT x=0;x<size;++x){unsigned run=0;for(UINT y=0;y<size;++y){run=(rain0[y*size+x]&255)>2?run+1:0;longest=std::max(longest,run);}}
    std::printf("longest rain streak=%u pixels of %u\n",longest,size);
    Require(longest<=28,"rain streaks are compact rather than screen-height beams");
    // Visual mode must scatter light against darkness, yet leave bright sky
    // bright rather than overlaying it with opaque white or a gray sheet.
    const auto darkRain=render(1,0,5,0,1,0,false,false);
    const auto skyRain=render(1,0,5,0,1,0,false,true);
    unsigned brightened=0,subtleSky=0;
    for(size_t i=0;i<darkRain.size();++i) {
        brightened+=(darkRain[i]&255)>3;
        const unsigned sky=skyRain[i]&255;
        subtleSky+=sky<255&&sky>200;
    }
    Require(brightened>100&&subtleSky>100,
        "rain has readable dark-scene highlights and restrained sky refraction");
    auto snow0=render(2,0,5),snow1=render(2,1.f/60,5);
    Require(difference(snow0,snow1,0,4)<.3&&difference(snow0,snow1,0,-4)>2,"shared snow UV motion also falls downward");
    Require(nonblack(render(1,0,1))==0,"near foreground fully occludes precipitation");
    auto compressed=render(1,0,5,0,.94f),offset=render(1,0,5,.1f,.94f);
    Require(difference(rain0,compressed,0,0)<.1&&difference(rain0,offset,0,0)<.1,"rain occlusion normalizes viewport MinZ and MaxZ");

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
    // Rain must also composite without captured depth or an atmosphere pass.
    // Debug mask deliberately excludes scene and lens colour from the test.
    weathervisuals::nativeWeatherMode=1;weathervisuals::rainDebugView=1;
    weathervisuals::composedThisFrame=false;
    Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0));
    Check(d->BeginScene());weathervisuals::BeforeDraw(d.Get());Check(d->EndScene());
    Require(nonblack(pixels())>100,"detected rain composites without clouds or captured depth");
    weathervisuals::rainDebugView=0;
    DWORD state=0;Check(d->GetRenderState(D3DRS_SCISSORTESTENABLE,&state));Require(state==TRUE,"scissor state restored after weather");
    Check(d->GetRenderState(D3DRS_FILLMODE,&state));Require(state==D3DFILL_WIREFRAME,"fill mode restored after weather");
    Check(d->GetRenderState(D3DRS_FOGENABLE,&state));Require(state==TRUE,"native fog restored after weather");
    Check(d->GetRenderState(D3DRS_STENCILENABLE,&state));Require(state==TRUE,"stencil restored after weather");
    Check(d->GetRenderState(D3DRS_SRGBWRITEENABLE,&state));Require(state==TRUE,"sRGB write state restored after weather");
    Check(d->GetSamplerState(3,D3DSAMP_ADDRESSU,&state));Require(state==D3DTADDRESS_MIRROR,"mist sampler addressing restored");
    Check(d->GetSamplerState(3,D3DSAMP_SRGBTEXTURE,&state));Require(state==TRUE,"mist sampler color space restored");
    float c[4]{};Check(d->GetPixelShaderConstantF(5,c,1));Require(memcmp(c,sentinel,sizeof(c))==0,"depth-range constants restored");
    Require(renderer::g_trackedState.vsHash==tracked.vsHash&&renderer::g_trackedState.currentPS==tracked.currentPS,"tracked game pipeline preserved");
    // A frame with no matching UI draw gets one late world-frame composite,
    // not a duplicate pass or an invisible rain frame.
    weathervisuals::composedThisFrame=false;
    weathervisuals::worldGeometrySeenThisFrame=true;
    renderer::g_trackedState.vsHash=0;renderer::g_trackedState.psHash=0;
    weathervisuals::rainDebugView=1;
    Require(weathervisuals::NeedsPresentFallback(),"missing UI draw requests one weather fallback");
    Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0));
    Check(d->BeginScene());weathervisuals::BeforeDraw(d.Get(),true);Check(d->EndScene());
    Require(weathervisuals::composedThisFrame&&weathervisuals::rainCompositeReady&&
        nonblack(pixels())>100&&!weathervisuals::NeedsPresentFallback(),
        "fallback renders detected rain once without a UI trigger");
    weathervisuals::rainDebugView=0;
    weathervisuals::worldGeometrySeenThisFrame=false;
    renderer::g_trackedState=tracked;
    weathervisuals::nativeWeatherMode=0;weathervisuals::composedThisFrame=false;
    Check(d->BeginScene());weathervisuals::BeforeDraw(d.Get());Check(d->EndScene());
    Require(!weathervisuals::composedThisFrame,"no native weather means no added weather pass");
    weathervisuals::rainDropTex.Reset();weathervisuals::CreateProceduralFallback(d.Get());
    D3DLOCKED_RECT r{};Check(weathervisuals::rainDropTex->LockRect(0,&r,nullptr,D3DLOCK_READONLY));unsigned maxAlpha=0;
    for(unsigned y=0;y<128;++y)for(unsigned x=0;x<16;++x)maxAlpha=std::max(maxAlpha,unsigned(reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+y*r.Pitch)[x]>>24));
    Check(weathervisuals::rainDropTex->UnlockRect(0));Require(maxAlpha>0&&maxAlpha<=38,"fallback rain matches the low-opacity authored mask");
    weathervisuals::Reset(d.Get());std::puts("Weather GPU regressions passed");return 0;
} catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
