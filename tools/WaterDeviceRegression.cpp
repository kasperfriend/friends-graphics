// Tests the production water Scope, not a second implementation of it.
#include "../WaterEffect.h"
#include "../WaterDiagnostics.h"
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
    static std::unordered_set<std::string> reported;
    if(reported.insert(name).second)std::printf("PASS %s\n",name);
}
// Isolated settings, not the repository's shipped Enabled=1 files. Exercise
// the same Save -> ReloadTuning path as F7 with a stale startup Enabled=0.
struct WaterSettings {
    std::filesystem::path directory;
    std::wstring base;
    WaterSettings() {
        directory=std::filesystem::absolute(std::filesystem::path("build") /
            ("water-settings-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())));
        std::filesystem::create_directories(directory);
        base=directory.wstring()+L"\\";
        Check(WritePrivateProfileStringW(L"Water",L"Enabled",L"0",
            (directory/L"ModernWoWRenderer.ini").c_str())?S_OK:E_FAIL);
        renderer::locationtuning::Sync(base,{},false);
        renderer::locationtuning::editZone=false;
        Require(renderer::locationtuning::Save(L"Water",L"Enabled",0),"write initially disabled live water");
    }
    void Set(bool on) {
        Require(renderer::locationtuning::Save(L"Water",L"Enabled",on?1:0),"save water through menu settings path");
        watereffect::ReloadTuning();
        Require(watereffect::effectEnabled==on,"live water follows saved toggle");
        Require(watereffect::enabled,"live OFF never disables backend readiness or hook eligibility");
        Require(watereffect::NeedsScene()==(on&&watereffect::active),"capture follows live and master switches");
    }
    ~WaterSettings() {
        std::error_code error;
        std::filesystem::remove_all(directory,error);
    }
};
void TestWaterSettings(WaterSettings& settings) {
    watereffect::Configure(settings.base);
    Require(watereffect::enabled&&!watereffect::code.empty()&&!watereffect::vertexCode.empty(),
        "production shaders prepared even when startup and live water are OFF");
    Require(!watereffect::effectEnabled&&!watereffect::NeedsScene(),"starting OFF does not request water capture");
    const auto* ps=watereffect::code.data();const auto* vs=watereffect::vertexCode.data();
    settings.Set(true);
    Require(watereffect::code.data()==ps&&watereffect::vertexCode.data()==vs,"live enable needs no shader recompile or restart");
    settings.Set(false);settings.Set(true);
    // Missing live key uses the menu's ON default, not the legacy startup OFF.
    Require(WritePrivateProfileStringW(L"Water",L"Enabled",nullptr,
        renderer::locationtuning::baseFile.c_str())!=0,"remove live key to test defaults");
    watereffect::ReloadTuning();
    Require(watereffect::effectEnabled&&renderer::locationtuning::ReadEditorInt(
        L"Water",L"Enabled",1,renderer::locationtuning::baseFile.c_str())==1,
        "menu and renderer agree on default with stale startup OFF");
    settings.Set(false);
    renderer::locationtuning::Sync(settings.base,{571,65,4164,true},true);
    settings.Set(true);
    Require(GetPrivateProfileIntW(L"Water",L"Enabled",1,renderer::locationtuning::baseFile.c_str())==0,
        "location enable leaves global OFF untouched");
    renderer::locationtuning::Sync(settings.base,{571,65,4165,true},true);
    watereffect::ReloadTuning();
    Require(watereffect::enabled&&!watereffect::effectEnabled&&!watereffect::NeedsScene(),"leaving enabled area restores global OFF without disabling backend");
    renderer::locationtuning::Sync(settings.base,{571,65,4164,true},true);
    watereffect::ReloadTuning();
    Require(watereffect::effectEnabled&&watereffect::NeedsScene(),"returning to enabled area enables water without restart");
    renderer::locationtuning::Sync(settings.base,{},false);
    settings.Set(true);
    watereffect::active=false;
    settings.Set(true);
    Require(!watereffect::active,"water edits do not override F11 master OFF");
    watereffect::active=true;
}
std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
void SetDiagnostics(const WaterSettings& settings,bool on,bool automatic=false) {
    const auto ini=settings.directory/L"ModernWoWRenderer.ini";
    Require(WritePrivateProfileStringW(L"Diagnostics",L"Enabled",on?L"1":L"0",ini.c_str())!=0,"write diagnostic enable");
    Require(WritePrivateProfileStringW(L"Diagnostics",L"AutoCapture",automatic?L"1":L"0",ini.c_str())!=0,"write diagnostic auto capture");
    waterdiag::Configure(settings.base); // The same reload now called by F12.
}
void TestDiagnosticFiles(WaterSettings& settings) {
    SetDiagnostics(settings,false);
    Require(!waterdiag::enabled&&!std::filesystem::exists(waterdiag::root),"disabled diagnostics do not create a capture directory");
    waterdiag::OnPresent(nullptr,true,true);
    auto text=ReadText(waterdiag::statusPath);
    Require(text.find("F8 ignored")!=std::string::npos,"disabled F8 explains missing capture in existing water log");
    waterdiag::OnPresent(nullptr,true,true);
    Require(ReadText(waterdiag::statusPath)==text,"holding F8 does not spam the log");
    waterdiag::OnPresent(nullptr,false,true);
    waterdiag::OnPresent(nullptr,true,false);
    Require(ReadText(waterdiag::statusPath)==text,"background F8 does not request capture");
    waterdiag::OnPresent(nullptr,false,true);
    SetDiagnostics(settings,true,true);
    Require(waterdiag::enabled&&waterdiag::autoCapture,"reload enables and arms capture without restarting");
    text=ReadText(waterdiag::statusPath);
    Require(text.find("configured enabled=1 auto=1")!=std::string::npos&&text.find("ModernWoWRenderer.ini")!=std::string::npos,
        "configuration status identifies effective enable and input path");
    // A regular file where a directory belongs deterministically reproduces
    // write failure without depending on elevation or filesystem ACLs.
    const auto blocker=settings.directory/L"blocked";
    {std::ofstream out(blocker);out<<"not a directory";}
    waterdiag::root=blocker;
    Require(!waterdiag::OpenCapture()&&!waterdiag::enabled&&!waterdiag::active,"directory failure disables capture safely");
    text=ReadText(waterdiag::statusPath);
    Require(text.find("create directory failed code=")!=std::string::npos&&text.find("blocked")!=std::string::npos,
        "directory failure reports error code and path outside failed directory");
    SetDiagnostics(settings,true);
    Require(waterdiag::OpenCapture(),"reload recovers filesystem capture after failure");
    // Deliberately poison the stream to model disk-full/failed writes.
    waterdiag::active=true;
    waterdiag::log.setstate(std::ios::badbit);
    waterdiag::Finish("test-write-failure");
    Require(!waterdiag::enabled&&!waterdiag::active&&!waterdiag::log.is_open(),"failed write closes and disables capture");
    Require(ReadText(waterdiag::statusPath).find("write/close failed")!=std::string::npos,"write failure is reported");
    SetDiagnostics(settings,true);
    Require(waterdiag::OpenCapture(),"failed stream can be reopened after reload");
    waterdiag::active=true;waterdiag::Configure(settings.base);
    Require(!waterdiag::active&&!waterdiag::log.is_open(),"reload closes an in-progress capture");
    SetDiagnostics(settings,false);
}
void TestDiagnosticDevice(WaterSettings& settings,IDirect3DDevice9* device) {
    waterdiag::captures=0; // Independent fixture; production keeps its session cap.
    SetDiagnostics(settings,true,true);
    waterdiag::OnPresent(device,true,true); // F8 + auto request together.
    Require(waterdiag::active&&!waterdiag::autoCapture&&waterdiag::captures==1,"auto and F8 start exactly one capture");
    auto file=waterdiag::folder/L"draws.txt";
    waterdiag::OnPresent(device,false,true);
    Require(!waterdiag::active&&ReadText(file).find("captured=0 observed=0")!=std::string::npos,
        "frame with no draw callbacks still produces an explicit zero-draw capture");
    SetDiagnostics(settings,true,true);
    waterdiag::OnPresent(device,false,true);
    file=waterdiag::folder/L"draws.txt";
    waterdiag::Draw(device,"DrawPrimitive",D3DPT_TRIANGLELIST,2);
    waterdiag::OnPresent(device,false,true);
    const auto text=ReadText(file);
    Require(text.find("DRAW 0")!=std::string::npos&&text.find("VS vs_")!=std::string::npos&&
        text.find("T 0 hr=0")!=std::string::npos&&text.find("captured=1 observed=1")!=std::string::npos,
        "device capture records native shaders, textures and completion");
    Require(waterdiag::shaders.size()==2,"device capture writes both shader bytecodes");
    SetDiagnostics(settings,true,true);waterdiag::OnPresent(device,false,true);
    waterdiag::Reset(device);
    Require(!waterdiag::active&&!waterdiag::log.is_open(),"reset closes diagnostic capture");
    waterdiag::captures=8;
    SetDiagnostics(settings,true,true);waterdiag::OnPresent(device,false,true);
    Require(!waterdiag::active&&!waterdiag::autoCapture&&ReadText(waterdiag::statusPath).find("capture limit reached")!=std::string::npos,
        "capture limit is bounded and reported instead of silently ignoring requests");
    SetDiagnostics(settings,false);
}
// Inject one real COM failure after stage 2 has already changed. This
// reproduces the original partial-setup leak without maintaining a fake device.
using SetTextureFn=HRESULT(WINAPI*)(IDirect3DDevice9*,DWORD,IDirect3DBaseTexture9*);
SetTextureFn realSetTexture=nullptr;
bool failStage3=false;
HRESULT WINAPI FailTexture3(IDirect3DDevice9* d,DWORD stage,IDirect3DBaseTexture9* texture) {
    if(stage==3&&failStage3){failStage3=false;return D3DERR_INVALIDCALL;}
    return realSetTexture(d,stage,texture);
}
struct TextureFailure {
    void** entry;
    explicit TextureFailure(IDirect3DDevice9* d) {
        entry=&(*reinterpret_cast<void***>(d))[65];
        realSetTexture=reinterpret_cast<SetTextureFn>(*entry);
        DWORD protect=0;Require(VirtualProtect(entry,sizeof(void*),PAGE_READWRITE,&protect)!=0,"test fault hook writable");
        *entry=reinterpret_cast<void*>(FailTexture3);DWORD ignored=0;VirtualProtect(entry,sizeof(void*),protect,&ignored);
        failStage3=true;
    }
    ~TextureFailure() {
        DWORD protect=0;
        if(VirtualProtect(entry,sizeof(void*),PAGE_READWRITE,&protect)) {
            *entry=reinterpret_cast<void*>(realSetTexture);DWORD ignored=0;VirtualProtect(entry,sizeof(void*),protect,&ignored);
        }
    }
};
int main() try {
    WaterSettings settings;
    TestWaterSettings(settings);
    TestDiagnosticFiles(settings);
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
    // OriginalVS reuses the production vertex ABI. Keep its displacement zero
    // so OFF readbacks exercise visible native pixels, not clipped geometry.
    float savedPS[36],savedVS[4]={71,0,73,74},savedLights[68];
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
    {
        TextureFailure failure(d.Get());
        {watereffect::Scope scope(d.Get());Require(!scope.device&&!failStage3,"failed stage 3 bind rolls back partial setup");}
        restored();
    }
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
    auto pixel=[&](float debug,bool modern=true) {
        watereffect::waterDebugMode=debug;
        Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0));Check(d->BeginScene());
        {watereffect::Scope scope(d.Get());
            Require(scope.device==(modern?d.Get():nullptr),"production draw follows live water and master switches");
            wroteMRT=scope.maskBound&&scope.depthBound;
            Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex)));}
        Check(d->EndScene());Check(d->GetRenderTargetData(back.Get(),readback.Get()));
        D3DLOCKED_RECT r{};Check(readback->LockRect(&r,nullptr,D3DLOCK_READONLY));
        DWORD value=reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+64*r.Pitch)[64];Check(readback->UnlockRect());return value;
    };
    watereffect::reflectionScene=watereffect::reflectionDepth=nullptr;
    Require((pixel(1)&0xffffff)==0xff00ff,"debug mode proves replacement reaches the framebuffer");
    Require((pixel(0)&0xffffff)!=0,"modern water without capture is finite and nonblack");
    restored();
    // Identity fixture: the synthetic ABI-compatible shaders above stand in
    // for the captured native pair. Seed the same metadata that SetShader
    // hooks supply, but keep actual-device identity checks, classification,
    // capture, Scope, draw, readback and restoration in production code.
    // This is NOT a replay of captured WoW shader bytecode.
    {
        renderer::ScopedTrackedRenderState trackedRestore;
        auto& tracked=renderer::g_trackedState;
        tracked.currentVS=originalVS.Get();tracked.currentPS=originalPS.Get();
        tracked.vsHash=0x48a82796bd612aebull;tracked.psHash=0x7d4f078fa1876a09ull;
        const UINT sizes[][3]={{8,64,256},{8,64,128},{4,32,256},{2,16,128},{8,64,1024},{1,1,16}};
        const uint64_t vertexHashes[]={0x206d861fd0a721ddull,0xfdd9528ed3ac30eaull,0x48a82796bd612aebull};
        const uint64_t pixelHashes[]={0x7d4f078fa1876a09ull,0x17f042a7906ca126ull};
        unsigned fixture=0;
        for(const auto& size:sizes) {
            tracked.vsHash=vertexHashes[fixture%3];tracked.psHash=pixelHashes[fixture/3];++fixture;
            auto lookup=texture(size[0],size[1],0xff406080);
            auto waves=texture(size[2],size[2],0xff808080);
            Check(d->SetTexture(0,lookup.Get()));Check(d->SetTexture(1,waves.Get()));
            Require(renderer::water::IsMaterial(d.Get()),"known bound water identified at noncanonical resolutions");
            Require(classifier.Classify(d.Get(),ctx,translation,false,0).isWater,
                "classifier agrees with replacement at noncanonical resolutions");
            Check(d->BeginScene());waterreflection::Prepare(d.Get());Check(d->EndScene());
            Require(waterreflection::ready&&watereffect::reflectionScene,
                "noncanonical water still gets pre-water scene capture");
            waterreflection::Finish();
            Require((pixel(1)&0xffffff)==0xff00ff,"noncanonical water replacement reaches framebuffer");
            Require((pixel(0)&0xffffff)!=0,"noncanonical full water shader renders nonblack");
            restored();
            ComPtr<IDirect3DBaseTexture9> t0,t1;
            Check(d->GetTexture(0,t0.GetAddressOf()));Check(d->GetTexture(1,t1.GetAddressOf()));
            Require(t0.Get()==lookup.Get()&&t1.Get()==waves.Get(),"noncanonical native textures restored");
            std::printf("PASS water resolution %ux%u / %ux%u classifier, capture, pixels and state\n",
                size[0],size[1],size[2],size[2]);
        }
        auto small=texture(256,256,0xff808080);
        Check(d->SetTexture(0,base.Get()));Check(d->SetTexture(1,small.Get()));
        tracked.currentVS=nullptr;
        Require(!renderer::water::IsMaterial(d.Get()),"stale tracked VS identity cannot approve actual unknown shader");
        tracked.currentVS=originalVS.Get();tracked.currentPS=nullptr;
        Require(!renderer::water::IsMaterial(d.Get()),"stale tracked PS identity cannot approve actual unknown shader");
        tracked.currentPS=originalPS.Get();tracked.psHash=0;
        Require(!renderer::water::IsMaterial(d.Get()),"unknown PS cannot approve noncanonical water");
        Require((pixel(1,false)&0xffffff)!=0xff00ff,"unrecognized noncanonical draw stays native");
        restored();
        tracked.psHash=0x7d4f078fa1876a09ull;
        Check(d->SetPixelShader(nullptr));
        Require(!renderer::water::IsMaterial(d.Get()),"missing actual PS rejected despite tracked water pair");
        Check(d->SetPixelShader(originalPS.Get()));
        Check(d->SetTexture(1,nullptr));
        Require(!renderer::water::IsMaterial(d.Get()),"missing actual texture rejected despite tracked water pair");
        Check(d->SetTexture(1,surface.Get()));
    }
    waterreflection::Reset(d.Get());
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
    settings.Set(false);
    const DWORD nativePixel=pixel(1,false);
    Require((nativePixel&0xffffff)!=0xff00ff&&(nativePixel&0xffffff)!=0,"menu OFF renders visible native water");restored();
    settings.Set(true);
    Require((pixel(1)&0xffffff)==0xff00ff,"menu ON reaches framebuffer despite legacy startup OFF");restored();
    watereffect::active=false;
    Require(pixel(1,false)==nativePixel,"master OFF still renders native water");restored();
    watereffect::active=true;
    Require((pixel(1)&0xffffff)==0xff00ff,"master ON restores live enabled water");restored();
    watereffect::Reset(d.Get());Require(!watereffect::cachedReplacementPS&&!watereffect::cachedReplacementVS,"reset releases cached shaders");
    {watereffect::Scope scope(d.Get());Require(scope.device==d.Get(),"replacement recreated after reset");}
    restored();watereffect::Reset(d.Get());
    TestDiagnosticDevice(settings,d.Get());
    restored();
    std::puts("Water GPU regressions passed");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
