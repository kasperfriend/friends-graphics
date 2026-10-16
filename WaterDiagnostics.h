#pragma once
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <vector>
#include <set>
#include <wrl/client.h>
#include "src/Core/FrameContext.h"

// Observation only. No resource locks, state changes, retained COM objects or GPU readbacks.
namespace waterdiag {
using Microsoft::WRL::ComPtr;
std::atomic<bool> enabled{false};
std::mutex mutex;
bool active=false, autoCapture=false, keyDown=false;
unsigned captures=0, draws=0, maxDraws=2048, skipDraws=0, seenDraws=0;
std::filesystem::path root, folder, statusPath;
std::ofstream log;
std::set<std::string> shaders;
IDirect3DDevice9* target=nullptr; // Identity only; never dereferenced outside a callback.

// Report in the already-used water log, OUTSIDE the capture directory. A
// missing/unwritable directory must not also hide the reason it is missing.
void Status(const std::wstring& message) noexcept {
    try {
        std::wofstream out(statusPath,std::ios::app);
        out<<L"water diagnostics: "<<message<<L'\n';
        out.flush();
        if(out)return;
    } catch(...) {}
    OutputDebugStringW(L"ModernWoWRenderer water diagnostics: ");
    OutputDebugStringW(message.c_str());
    OutputDebugStringW(L"\n");
}
void Abort(const wchar_t* operation) noexcept {
    enabled=false; active=false; autoCapture=false; target=nullptr;
    try { if(log.is_open())log.close(); log.clear(); } catch(...) {}
    Status(operation);
}
void Finish(const char* why) {
    if(active) {
        log<<"END reason="<<why<<" captured="<<draws<<" observed="<<seenDraws<<'\n';
        log.flush();
        const bool written=bool(log);
        log.close();
        Status(L"finished captured="+std::to_wstring(draws)+L" observed="+
            std::to_wstring(seenDraws)+L" file="+(folder/L"draws.txt").wstring());
        if(!written||log.fail())Abort(L"capture write/close failed; check free space and directory permissions");
    }
    active=false; target=nullptr;
}
void Configure(const std::wstring& base) noexcept {
    try {
        std::lock_guard lock(mutex);
        Finish("reload");
        statusPath=std::filesystem::path(base)/L"WaterEffect.log";
        const auto ini=std::filesystem::path(base)/L"ModernWoWRenderer.ini";
        enabled=GetPrivateProfileIntW(L"Diagnostics",L"Enabled",0,ini.c_str())!=0;
        autoCapture=enabled&&GetPrivateProfileIntW(L"Diagnostics",L"AutoCapture",0,ini.c_str())!=0;
        maxDraws=std::clamp(GetPrivateProfileIntW(L"Diagnostics",L"MaxDraws",2048,ini.c_str()),1u,8192u);
        skipDraws=std::min(GetPrivateProfileIntW(L"Diagnostics",L"SkipDraws",0,ini.c_str()),100000u);
        root=std::filesystem::path(base)/L"WaterDiagnostics";
        Status(L"configured enabled="+std::to_wstring(enabled.load())+L" auto="+
            std::to_wstring(autoCapture)+L" ini="+ini.wstring()+L" output="+root.wstring());
    } catch(...) { Abort(L"configuration failed"); }
}
// Separated from device observation so filesystem failures can be tested even
// on a headless runner. Called only under mutex by Start/Configure's callers.
bool OpenCapture() {
    if(captures>=8) { Status(L"capture limit reached (8); restart the client for more captures"); return false; }
    SYSTEMTIME t{}; GetLocalTime(&t);
    std::wostringstream name;
    name<<t.wYear<<L'-'<<t.wMonth<<L'-'<<t.wDay<<L'_'<<t.wHour<<L'-'<<t.wMinute<<L'-'<<t.wSecond<<L'_'<<GetCurrentProcessId()<<L'_'<<++captures;
    folder=root/name.str();
    std::error_code error;
    std::filesystem::create_directories(folder,error);
    if(error) {
        Status(L"create directory failed code="+std::to_wstring(error.value())+L" path="+folder.wstring());
        Abort(L"capture disabled after directory failure; fix the path/permissions, then press F12");
        return false;
    }
    if(log.is_open())log.close();
    log.clear(); // A previous failed open must not prevent recovery on F12.
    log.open(folder/L"draws.txt");
    if(!log) {
        Status(L"open failed path="+(folder/L"draws.txt").wstring());
        Abort(L"capture disabled after file failure; check permissions/free space, then press F12");
        return false;
    }
    log<<std::setprecision(9);
    draws=seenDraws=0; shaders.clear();
    return true;
}
void Start(IDirect3DDevice9* d) {
    autoCapture=false; // F8 and AutoCapture must never open competing files.
    if(active)return;
    if(!d) { Status(L"capture not started: no device"); return; }
    if(!OpenCapture())return;
    target=d; active=true;
    log<<"capture device="<<d<<" maxDraws="<<maxDraws<<" skipDraws="<<skipDraws<<" constants=F[0..35] textures=pixel[0..15]\n";
    D3DDEVICE_CREATION_PARAMETERS p{};
    HRESULT hr=d->GetCreationParameters(&p);
    log<<"creation hr="<<hr<<" flags="<<p.BehaviorFlags<<'\n';
    if(SUCCEEDED(hr) && (p.BehaviorFlags&D3DCREATE_PUREDEVICE))
        log<<"WARNING pure device: state queries may fail; device flags are not modified\n";
    log.flush();
    if(!log) { Abort(L"capture header write failed"); return; }
    Status(L"started file="+(folder/L"draws.txt").wstring());
}
// Inputs separated from Win32 key polling for deterministic regression tests.
void OnPresent(IDirect3DDevice9* d,bool down,bool foreground) noexcept {
    try {
        std::lock_guard lock(mutex);
        if(active && target==d) Finish("present");
        const bool pressed=down&&!keyDown&&foreground;
        keyDown=down;
        if(!enabled.load(std::memory_order_relaxed)) {
            if(pressed)Status(L"F8 ignored: [Diagnostics] Enabled=0; edit ModernWoWRenderer.ini and press F12");
            return;
        }
        // Arm at a frame boundary, not at a draw. Even zero observed draws
        // now produce a file and an explicit zero-draw completion report.
        if(!active&&(autoCapture||pressed))Start(d);
    } catch(...) { Abort(L"capture start/finish failed with an exception"); }
}
void Present(IDirect3DDevice9* d) noexcept {
    const bool down=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
    OnPresent(d,down,renderer::FrameContext::Current().wowFocused);
}
void Reset(IDirect3DDevice9* d) noexcept {
    try { std::lock_guard lock(mutex); if(active&&target==d) Finish("reset"); }
    catch(...) { Abort(L"capture reset failed"); }
}
template<class Shader> std::string DumpShader(Shader* shader,const char* stage,HRESULT hr) {
    if(FAILED(hr)) return "query-error:"+std::to_string(hr);
    if(!shader) return "fixed-function";
    UINT size=0;
    if(FAILED(shader->GetFunction(nullptr,&size))||!size||size>65536) return "bytecode-unavailable";
    std::vector<unsigned char> bytes(size);
    if(FAILED(shader->GetFunction(bytes.data(),&size))) return "bytecode-read-error";
    uint64_t hash=14695981039346656037ull;
    for(unsigned char c:bytes) { hash^=c; hash*=1099511628211ull; }
    std::ostringstream key; key<<stage<<'_'<<std::hex<<hash;
    const std::string id=key.str();
    if(!shaders.count(id)) {
        if(shaders.size()>=256) return id+":dump-limit";
        std::ofstream out(folder/(id+".bin"),std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        if(!out) return id+":write-error";
        shaders.insert(id);
    }
    return id;
}
void Surface(IDirect3DSurface9* s,const char* label,HRESULT hr) {
    log<<label<<" hr="<<hr<<" object="<<s;
    D3DSURFACE_DESC desc{};
    if(s&&SUCCEEDED(s->GetDesc(&desc))) log<<" size="<<desc.Width<<'x'<<desc.Height<<" format="<<desc.Format<<" usage="<<desc.Usage<<" multisample="<<desc.MultiSampleType;
    log<<'\n';
}
void Draw(IDirect3DDevice9* d,const char* kind,D3DPRIMITIVETYPE type,UINT count) noexcept {
    if(!enabled.load(std::memory_order_relaxed)) return;
    try {
        std::lock_guard lock(mutex);
        if(!active||target!=d) return;
        const unsigned ordinal=seenDraws++;
        if(ordinal<skipDraws) return;
        if(draws>=maxDraws) { Finish("draw-limit"); return; }
        log<<"DRAW "<<draws++<<" ordinal="<<ordinal<<" kind="<<kind<<" topology="<<type<<" primitives="<<count<<'\n';
        ComPtr<IDirect3DVertexShader9> vs; ComPtr<IDirect3DPixelShader9> ps;
        HRESULT vh=d->GetVertexShader(vs.GetAddressOf()), ph=d->GetPixelShader(ps.GetAddressOf());
        log<<"VS "<<DumpShader(vs.Get(),"vs",vh)<<" PS "<<DumpShader(ps.Get(),"ps",ph)<<'\n';
        for(int stage=0;stage<2;++stage) {
            float values[144]{};
            HRESULT hr=stage?d->GetPixelShaderConstantF(0,values,36):d->GetVertexShaderConstantF(0,values,36);
            log<<(stage?"PSF":"VSF")<<" hr="<<hr;
            if(SUCCEEDED(hr)) for(float value:values) log<<' '<<value;
            log<<'\n';
        }
        for(DWORD slot=0;slot<16;++slot) {
            ComPtr<IDirect3DBaseTexture9> texture;
            HRESULT hr=d->GetTexture(slot,texture.GetAddressOf());
            log<<"T "<<slot<<" hr="<<hr<<" object="<<texture.Get();
            if(texture) {
                auto textureType=texture->GetType(); log<<" type="<<textureType;
                D3DSURFACE_DESC desc{}; HRESULT dh=E_FAIL;
                if(textureType==D3DRTYPE_TEXTURE) dh=static_cast<IDirect3DTexture9*>(texture.Get())->GetLevelDesc(0,&desc);
                if(textureType==D3DRTYPE_CUBETEXTURE) dh=static_cast<IDirect3DCubeTexture9*>(texture.Get())->GetLevelDesc(0,&desc);
                if(SUCCEEDED(dh)) log<<" size="<<desc.Width<<'x'<<desc.Height<<" format="<<desc.Format<<" usage="<<desc.Usage<<" pool="<<desc.Pool;
                if(textureType==D3DRTYPE_VOLUMETEXTURE) {
                    D3DVOLUME_DESC vol{};
                    if(SUCCEEDED(static_cast<IDirect3DVolumeTexture9*>(texture.Get())->GetLevelDesc(0,&vol)))
                        log<<" size="<<vol.Width<<'x'<<vol.Height<<'x'<<vol.Depth<<" format="<<vol.Format;
                }
            }
            log<<'\n';
        }
        ComPtr<IDirect3DSurface9> rt,depth;
        HRESULT rh=d->GetRenderTarget(0,rt.GetAddressOf()), dh=d->GetDepthStencilSurface(depth.GetAddressOf());
        Surface(rt.Get(),"RT0",rh); Surface(depth.Get(),"DEPTH",dh);
        D3DVIEWPORT9 vp{}; HRESULT vpHr=d->GetViewport(&vp);
        log<<"VIEWPORT hr="<<vpHr<<' '<<vp.X<<' '<<vp.Y<<' '<<vp.Width<<' '<<vp.Height<<' '<<vp.MinZ<<' '<<vp.MaxZ<<'\n';
        DWORD fvf=0; HRESULT fh=d->GetFVF(&fvf); log<<"FVF hr="<<fh<<" value="<<fvf<<'\n';
        ComPtr<IDirect3DVertexDeclaration9> declaration;
        const HRESULT declHr=d->GetVertexDeclaration(declaration.GetAddressOf());
        log<<"DECL hr="<<declHr<<'\n';
        if(SUCCEEDED(declHr)&&declaration) {
            D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT length=MAXD3DDECLLENGTH+1;
            const HRESULT hr=declaration->GetDeclaration(elements,&length);
            log<<"ELEMENTS hr="<<hr<<" count="<<length<<'\n';
            if(SUCCEEDED(hr))for(UINT i=0;i<std::min<UINT>(length,MAXD3DDECLLENGTH+1);++i){
                const auto& e=elements[i];
                log<<"ELEMENT "<<e.Stream<<' '<<e.Offset<<' '<<unsigned(e.Type)<<' '
                   <<unsigned(e.Method)<<' '<<unsigned(e.Usage)<<' '<<unsigned(e.UsageIndex)<<'\n';
            }
        }
        for(auto transform:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) {
            D3DMATRIX matrix{}; HRESULT hr=d->GetTransform(transform,&matrix);
            log<<"TRANSFORM "<<transform<<" hr="<<hr;
            if(SUCCEEDED(hr)) for(auto& row:matrix.m) for(float value:row) log<<' '<<value;
            log<<'\n';
        }
        for(DWORD slot=0;slot<4;++slot) {
            for(auto state:{D3DTSS_COLOROP,D3DTSS_COLORARG1,D3DTSS_COLORARG2,D3DTSS_ALPHAOP,D3DTSS_TEXCOORDINDEX,D3DTSS_TEXTURETRANSFORMFLAGS}) {
                DWORD value=0; HRESULT hr=d->GetTextureStageState(slot,state,&value);
                log<<"TSS "<<slot<<' '<<state<<" hr="<<hr<<" value="<<value<<'\n';
            }
        }
        for(auto state:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_ALPHATESTENABLE,D3DRS_CULLMODE,D3DRS_FOGENABLE}) {
            DWORD value=0; HRESULT hr=d->GetRenderState(state,&value);
            log<<"RS "<<state<<" hr="<<hr<<" value="<<value<<'\n';
        }
        if(!log) { Abort(L"draw capture write failed; check free space and directory permissions"); }
    } catch(...) { Abort(L"draw capture failed with an exception"); }
}
}
