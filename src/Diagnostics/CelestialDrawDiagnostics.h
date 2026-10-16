#pragma once
#include <d3d9.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <wrl/client.h>
#include "src/Core/FrameContext.h"

// Observation only. No resource locks, state changes, retained COM objects,
// or GPU readbacks - same safety contract as WaterDiagnostics.h, which this
// mirrors. Captures every draw call for exactly one frame and flags ones
// that loosely match "could plausibly be a sky-phase sun/moon billboard"
// (small primitive count, alpha blended, no z-write). That heuristic is a
// filter to make the log readable, NOT a claim about what any given draw
// actually is - the point of this file is to gather evidence for that
// question, not answer it in advance.
namespace celestialdiag {
using Microsoft::WRL::ComPtr;
std::atomic<bool> enabled{false};
std::mutex mutex;
bool active=false, keyDown=false;
unsigned draws=0, candidates=0, maxDraws=4096;
unsigned presentsSinceStart=0, totalDrawCallsSeen=0;
std::filesystem::path root, folder;
std::ofstream log;
std::set<std::string> dumped;
IDirect3DDevice9* target=nullptr;
UINT mainWidth=0, mainHeight=0;

// Crosshair picker: for candidate draws, samples a tiny region at screen
// center before and after the real draw executes and logs whether it
// changed. Works regardless of shader/fixed-function - it observes the
// actual rendered result rather than trying to reconstruct screen position
// from transforms, which only works for fixed-function draws.
ComPtr<IDirect3DSurface9> readback;
UINT rbW=0, rbH=0; D3DFORMAT rbFmt=D3DFMT_UNKNOWN;
bool pendingWatch=false;
BYTE pendingBefore[4]{};
unsigned pendingOrdinal=0;
unsigned crosshairChecks=0;
constexpr unsigned kMaxCrosshairChecks=400;

bool SampleCrosshair(IDirect3DDevice9* d, BYTE outPixel[4]) {
    ComPtr<IDirect3DSurface9> rt;
    if(FAILED(d->GetRenderTarget(0,rt.GetAddressOf()))||!rt) return false;
    D3DSURFACE_DESC desc{};
    if(FAILED(rt->GetDesc(&desc))) return false;
    if(desc.MultiSampleType!=D3DMULTISAMPLE_NONE) return false;
    if(desc.Width!=mainWidth||desc.Height!=mainHeight) return false; // skip off-screen RTs (reflections, minimap, ...)
    if(desc.Format!=D3DFMT_A8R8G8B8&&desc.Format!=D3DFMT_X8R8G8B8) return false;
    if(!readback||rbW!=desc.Width||rbH!=desc.Height||rbFmt!=desc.Format) {
        readback.Reset();
        if(FAILED(d->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,readback.GetAddressOf(),nullptr)))
            return false;
        rbW=desc.Width; rbH=desc.Height; rbFmt=desc.Format;
    }
    if(FAILED(d->GetRenderTargetData(rt.Get(),readback.Get()))) return false;
    RECT r{ LONG(desc.Width/2)-2, LONG(desc.Height/2)-2, LONG(desc.Width/2)+2, LONG(desc.Height/2)+2 };
    D3DLOCKED_RECT locked{};
    if(FAILED(readback->LockRect(&r,&locked,D3DLOCK_READONLY))) return false;
    uint32_t sum[4]{}; int n=0;
    for(int y=0;y<4;++y) {
        const BYTE* row=static_cast<const BYTE*>(locked.pBits)+y*locked.Pitch;
        for(int x=0;x<4;++x) { sum[0]+=row[x*4+0]; sum[1]+=row[x*4+1]; sum[2]+=row[x*4+2]; sum[3]+=row[x*4+3]; ++n; }
    }
    readback->UnlockRect();
    for(int i=0;i<4;++i) outPixel[i]=BYTE(sum[i]/std::max(n,1));
    return true;
}

void Configure(const std::wstring& base) {
    auto ini=base+L"ModernWoWRenderer.ini";
    enabled=GetPrivateProfileIntW(L"CelestialDiagnostics",L"Enabled",0,ini.c_str())!=0;
    root=std::filesystem::path(base)/L"CelestialDiagnostics";
}
void Finish(const char* why) {
    if(active) {
        log<<"END reason="<<why<<" draws="<<draws<<" candidates="<<candidates
           <<" presents_since_start="<<presentsSinceStart
           <<" total_draw_calls_seen_this_session="<<totalDrawCallsSeen<<'\n';
        log.close();
    }
    active=false; target=nullptr;
}
void Start(IDirect3DDevice9* d) {
    SYSTEMTIME t{}; GetLocalTime(&t);
    std::wostringstream name;
    name<<t.wYear<<L'-'<<t.wMonth<<L'-'<<t.wDay<<L'_'<<t.wHour<<L'-'<<t.wMinute<<L'-'<<t.wSecond;
    folder=root/name.str();
    std::filesystem::create_directories(folder);
    log.open(folder/L"celestial_candidates.txt");
    if(!log) { enabled=false; return; }
    draws=0; candidates=0; presentsSinceStart=0; crosshairChecks=0; pendingWatch=false; dumped.clear(); target=d; active=true;
    D3DVIEWPORT9 vp{};
    d->GetViewport(&vp);
    mainWidth=vp.Width; mainHeight=vp.Height;
    log<<"celestial draw capture, F9 to start.\n";
    log<<"CANDIDATE=1 heuristic: primitives<=64, alphaTest or alphaBlend. A filter to make this readable,\n";
    log<<"not a verdict - check the flagged draws by eye (texture dims/hash, VSF values) before trusting one.\n";
    log<<"CROSSHAIR_FOR_DRAW=<ordinal> ... HIT=1 means that specific draw actually changed the pixels at\n";
    log<<"screen center ("<<mainWidth/2<<","<<mainHeight/2<<") - this is the strongest signal in this file,\n";
    log<<"it observes the real rendered result rather than guessing from shader/transform state.\n";
}
void Present(IDirect3DDevice9* d) noexcept {
    if(!enabled.load(std::memory_order_relaxed)) return;
    try {
        std::lock_guard lock(mutex);
        if(active && target==d) {
            ++presentsSinceStart;
            // Finish once we've actually captured something from at least one
            // full Present-to-Present interval, OR bail out after a generous
            // number of empty frames so a capture can never hang silently
            // forever if draws truly aren't reaching this hook for some reason.
            if(draws>0 && presentsSinceStart>=2) Finish("present");
            else if(presentsSinceStart>=180) Finish("timeout-no-draws");
        }
        const bool down=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
        if(down&&!keyDown&&renderer::FrameContext::Current().wowFocused&&!active) Start(d);
        keyDown=down;
    } catch(...) { enabled=false; active=false; }
}
void Reset(IDirect3DDevice9* d) noexcept {
    if(!enabled.load(std::memory_order_relaxed)) return;
    try { std::lock_guard lock(mutex); if(active&&target==d) Finish("reset"); }
    catch(...) { enabled=false; active=false; }
}
template<class Shader> std::string HashShader(Shader* shader,const char* stage,HRESULT hr,bool dumpIfNew) {
    if(FAILED(hr)) return "query-error";
    if(!shader) return "fixed-function";
    ID3DBlob* blob=nullptr;
    if(FAILED(shader->GetFunction(&blob))||!blob) return "bytecode-unavailable";
    const UINT size=static_cast<UINT>(blob->GetBufferSize());
    if(!size||size>65536) { blob->Release(); return "bytecode-unavailable"; }
    std::vector<unsigned char> bytes(size);
    memcpy(bytes.data(),blob->GetBufferPointer(),size);
    blob->Release();
    uint64_t hash=14695981039346656037ull;
    for(unsigned char c:bytes) { hash^=c; hash*=1099511628211ull; }
    std::ostringstream key; key<<stage<<'_'<<std::hex<<hash;
    const std::string id=key.str();
    if(dumpIfNew && !dumped.count(id) && dumped.size()<128) {
        std::ofstream out(folder/(id+".bin"),std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        dumped.insert(id);
    }
    return id;
}
void Draw(IDirect3DDevice9* d,const char* kind,D3DPRIMITIVETYPE type,UINT count) noexcept {
    if(!enabled.load(std::memory_order_relaxed)) return;
    try {
        std::lock_guard lock(mutex);
        ++totalDrawCallsSeen; // unconditional - lets Finish() report whether Draw() is even reached
        if(!active||target!=d) return;
        if(draws>=maxDraws) { Finish("draw-limit"); return; }

        DWORD alphaBlend=0, zWrite=0, zEnable=0, alphaTest=0, cull=0;
        d->GetRenderState(D3DRS_ALPHABLENDENABLE,&alphaBlend);
        d->GetRenderState(D3DRS_ZWRITEENABLE,&zWrite);
        d->GetRenderState(D3DRS_ZENABLE,&zEnable);
        d->GetRenderState(D3DRS_ALPHATESTENABLE,&alphaTest);
        d->GetRenderState(D3DRS_CULLMODE,&cull);

        // Loosened from the original (count<=8, alphaBlend, !zWrite): that
        // only matched fixed-function draws, which turned out to be a minor/
        // decorative object, not the actual sun/moon. Drop the fixed-
        // function assumption entirely and let the crosshair pixel check
        // below do the real discrimination instead of the shader/state
        // heuristic.
        bool candidate = (count<=64) && (alphaTest||alphaBlend);
        if(candidate) ++candidates;

        unsigned ordinal=draws++;
        log<<"DRAW "<<ordinal<<" kind="<<kind<<" topology="<<type<<" primitives="<<count
           <<" CANDIDATE="<<(candidate?1:0)<<" alphaBlend="<<alphaBlend<<" zWrite="<<zWrite
           <<" zEnable="<<zEnable<<" alphaTest="<<alphaTest<<" cull="<<cull<<'\n';

        pendingWatch=false;
        if(candidate && crosshairChecks<kMaxCrosshairChecks) {
            if(SampleCrosshair(d,pendingBefore)) { pendingWatch=true; pendingOrdinal=ordinal; ++crosshairChecks; }
        }

        ComPtr<IDirect3DVertexShader9> vs; ComPtr<IDirect3DPixelShader9> ps;
        HRESULT vh=d->GetVertexShader(vs.GetAddressOf()), ph=d->GetPixelShader(ps.GetAddressOf());
        log<<"  VS="<<HashShader(vs.Get(),"vs",vh,candidate)<<" PS="<<HashShader(ps.Get(),"ps",ph,candidate)<<'\n';

        for(DWORD slot=0;slot<2;++slot) {
            ComPtr<IDirect3DBaseTexture9> texture;
            HRESULT hr=d->GetTexture(slot,texture.GetAddressOf());
            log<<"  T"<<slot<<" object="<<texture.Get();
            if(SUCCEEDED(hr)&&texture&&texture->GetType()==D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC desc{};
                if(SUCCEEDED(static_cast<IDirect3DTexture9*>(texture.Get())->GetLevelDesc(0,&desc)))
                    log<<" size="<<desc.Width<<'x'<<desc.Height<<" format="<<desc.Format;
            }
            log<<'\n';
        }

        if(candidate) {
            float vsConst[8][4]{};
            HRESULT vch=d->GetVertexShaderConstantF(0,vsConst[0],8);
            log<<"  VSF[0..7] hr="<<vch;
            if(SUCCEEDED(vch)) for(auto& row:vsConst) for(float v:row) log<<' '<<v;
            log<<'\n';

            // Fixed-function draws (no vertex shader) place geometry via the
            // classic transform pipeline instead of shader constants - this
            // is the only way to recover where such a draw actually sits.
            for(auto xf:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) {
                D3DMATRIX m{};
                HRESULT th=d->GetTransform(xf,&m);
                log<<"  XF"<<int(xf)<<" hr="<<th;
                if(SUCCEEDED(th)) for(auto& row:m.m) for(float v:row) log<<' '<<v;
                log<<'\n';
            }
            ComPtr<IDirect3DVertexDeclaration9> decl;
            HRESULT dh=d->GetVertexDeclaration(decl.GetAddressOf());
            log<<"  VDECL hr="<<dh<<" object="<<decl.Get()<<'\n';
            DWORD fvf=0; HRESULT fh=d->GetFVF(&fvf);
            log<<"  FVF hr="<<fh<<" value="<<fvf<<'\n';
        }

        if(!log) { Finish("write-error"); enabled=false; }
    } catch(...) { enabled=false; active=false; }
}
// Call immediately after the real draw executes (after originalDraw(...)
// returns), for the SAME draw Draw() was just called for. Compares the
// screen-center pixel to what Draw() sampled just before this draw ran.
void AfterDraw(IDirect3DDevice9* d) noexcept {
    if(!enabled.load(std::memory_order_relaxed)) return;
    try {
        std::lock_guard lock(mutex);
        if(!pendingWatch) return;
        pendingWatch=false;
        if(!active||target!=d) return;
        BYTE after[4]{};
        if(!SampleCrosshair(d,after)) return;
        int delta = std::abs(int(after[0])-int(pendingBefore[0]))
                  + std::abs(int(after[1])-int(pendingBefore[1]))
                  + std::abs(int(after[2])-int(pendingBefore[2]));
        bool hit = delta>12;
        log<<"  CROSSHAIR_FOR_DRAW="<<pendingOrdinal
           <<" before=("<<int(pendingBefore[0])<<' '<<int(pendingBefore[1])<<' '<<int(pendingBefore[2])<<')'
           <<" after=("<<int(after[0])<<' '<<int(after[1])<<' '<<int(after[2])<<')'
           <<" delta="<<delta<<" HIT="<<(hit?1:0)<<'\n';
        if(!log) { Finish("write-error"); enabled=false; }
    } catch(...) { enabled=false; active=false; }
}
}
