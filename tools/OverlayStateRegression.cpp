// Render the actual F7 menu through setter hooks, then check BOTH the D3D9
// device and the software mirror. A state-block-only/unhooked test misses the
// regression: Apply normally does not call the device's public Set* vtable.
#include "../TuningOverlay.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using renderer::TrackedRenderState;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D9 HRESULT=" + std::to_string(hr)); }

// Instrument the same five setters as the proxy; no draw hook is necessary
// here since we call production tuningoverlay::Draw directly.
using RS = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using VS = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using PS = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using Decl = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using FVF = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD);
RS originalRS; VS originalVS; PS originalPS; Decl originalDecl; FVF originalFVF;
unsigned setterCalls = 0;
HRESULT WINAPI SetRS(IDirect3DDevice9* d, D3DRENDERSTATETYPE state, DWORD value) {
    ++setterCalls;
    auto& t = renderer::g_trackedState;
    switch (state) {
    case D3DRS_ZENABLE: t.zEnable = value != 0; break;
    case D3DRS_ZWRITEENABLE: t.zWrite = value != 0; break;
    case D3DRS_ALPHABLENDENABLE: t.alphaBlend = value != 0; break;
    case D3DRS_ALPHATESTENABLE: t.alphaTest = value != 0; break;
    default: break;
    }
    return originalRS(d, state, value);
}
HRESULT WINAPI SetVS(IDirect3DDevice9* d, IDirect3DVertexShader9* vs) {
    ++setterCalls; renderer::g_trackedState.currentVS = vs;
    renderer::g_trackedState.vsHash = vs ? 0x279ee550dad80fecull : 0;
    return originalVS(d, vs);
}
HRESULT WINAPI SetPS(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
    ++setterCalls; renderer::g_trackedState.currentPS = ps;
    renderer::g_trackedState.psHash = ps ? 0x12345678ull : 0;
    return originalPS(d, ps);
}
HRESULT WINAPI SetDecl(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* decl) {
    ++setterCalls; renderer::g_trackedState.currentVDecl = decl; return originalDecl(d, decl);
}
HRESULT WINAPI SetFVF(IDirect3DDevice9* d, DWORD fvf) {
    ++setterCalls; renderer::g_trackedState.currentFVF = fvf; return originalFVF(d, fvf);
}
struct SetterHooks {
    void** table;
    const unsigned slots[5] = {57, 87, 89, 92, 107};
    void* saved[5]{};
    explicit SetterHooks(IDirect3DDevice9* device) {
        table = *reinterpret_cast<void***>(device);
        DWORD protection = 0;
        Require(VirtualProtect(table + 57, 51 * sizeof(void*), PAGE_READWRITE, &protection) != 0, "hook table writable");
        for (unsigned i = 0; i < 5; ++i) saved[i] = table[slots[i]];
        originalRS = reinterpret_cast<RS>(saved[0]); originalDecl = reinterpret_cast<Decl>(saved[1]);
        originalFVF = reinterpret_cast<FVF>(saved[2]); originalVS = reinterpret_cast<VS>(saved[3]); originalPS = reinterpret_cast<PS>(saved[4]);
        void* replacements[] = {reinterpret_cast<void*>(SetRS), reinterpret_cast<void*>(SetDecl),
            reinterpret_cast<void*>(SetFVF), reinterpret_cast<void*>(SetVS), reinterpret_cast<void*>(SetPS)};
        for (unsigned i = 0; i < 5; ++i) table[slots[i]] = replacements[i];
        DWORD ignored = 0; VirtualProtect(table + 57, 51 * sizeof(void*), protection, &ignored);
    }
    ~SetterHooks() {
        DWORD protection = 0;
        if (VirtualProtect(table + 57, 51 * sizeof(void*), PAGE_READWRITE, &protection)) {
            for (unsigned i = 0; i < 5; ++i) table[slots[i]] = saved[i];
            DWORD ignored = 0; VirtualProtect(table + 57, 51 * sizeof(void*), protection, &ignored);
        }
    }
};
void CheckRestored(IDirect3DDevice9* d, const TrackedRenderState& before) {
    const auto& after = renderer::g_trackedState;
    Require(after.currentVS == before.currentVS && after.currentPS == before.currentPS &&
        after.currentVDecl == before.currentVDecl && after.currentFVF == before.currentFVF &&
        after.vsHash == before.vsHash && after.psHash == before.psHash &&
        after.alphaBlend == before.alphaBlend && after.alphaTest == before.alphaTest &&
        after.zWrite == before.zWrite && after.zEnable == before.zEnable, "F7 leaked software render state");
    ComPtr<IDirect3DVertexShader9> vs; ComPtr<IDirect3DPixelShader9> ps; ComPtr<IDirect3DVertexDeclaration9> decl;
    Check(d->GetVertexShader(vs.GetAddressOf())); Check(d->GetPixelShader(ps.GetAddressOf()));
    Check(d->GetVertexDeclaration(decl.GetAddressOf()));
    Require(vs.Get() == before.currentVS && ps.Get() == before.currentPS && decl.Get() == before.currentVDecl,
        "F7 leaked actual device shaders/declaration");
    DWORD value = 0; Check(d->GetFVF(&value)); Require(value == before.currentFVF, "F7 leaked actual FVF");
    const D3DRENDERSTATETYPE states[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE};
    const bool expected[] = {before.zEnable, before.zWrite, before.alphaBlend, before.alphaTest};
    for (unsigned i = 0; i < 4; ++i) { Check(d->GetRenderState(states[i], &value)); Require((value != 0) == expected[i], "F7 leaked actual depth/blend/test state"); }
}
// Persistence is CPU-only: run this before the optional HAL checks so a
// headless CI runner still verifies every tab, including the new wind tab.
void TestResetDefaults() {
    const auto directory = std::filesystem::absolute("build/overlay-state-reset");
    std::filesystem::create_directories(directory);
    const auto base = directory.wstring() + L"\\";
    renderer::menudiagnostics::Configure(base);
    renderer::locationtuning::Sync(base, {571, 65, 4164, true}, false);
    tuningoverlay::Configure(base);
    using namespace tuningoverlay;
    for (int tabIndex = 0; tabIndex < int(Tabs().size()); ++tabIndex) {
        const auto tab = Tabs()[tabIndex];
        std::vector<int> expected;
        for (const auto& item : items) expected.push_back(item.value);
        // begin already points past the header; do not skip the first toggle.
        for (int index = tab.begin; index < tab.end; ++index) {
            auto& item = items[index];
            item.value = defaults[index] == item.lo ? item.hi : item.lo;
            Require(Save(item), "Could not persist nondefault tab setting");
            expected[index] = defaults[index];
        }
        layout.SelectTab(tabIndex);
        Require(ResetActiveTab(), "Reset Defaults did not report changed settings");
        for (int index = 0; index < int(items.size()); ++index) {
            const auto& item = items[index];
            if (item.kind == KIND_HEADER) continue;
            Require(item.value == expected[index], "Reset Defaults skipped a setting or changed another tab");
            Require(renderer::locationtuning::ReadEditorInt(item.section, item.key, defaults[index], ini.c_str()) == expected[index],
                "Reset Defaults did not persist every setting or changed another tab's file values");
        }
        Require(!ResetActiveTab(), "Reset Defaults was not idempotent");
    }
    layout.SelectTab(0);
    std::printf("PASS F7 Reset Defaults restores and persists all settings in every tab, without changing other tabs\n");
}
void TestGlobalScope() {
    using namespace tuningoverlay;
    using namespace renderer::locationtuning;
    const auto directory=std::filesystem::absolute("build/overlay-global-scope");
    std::filesystem::create_directories(directory);
    std::filesystem::remove(directory/L"LocationGraphics.ini");
    std::filesystem::remove(directory/L"GraphicsEffects.ini");
    const auto base=directory.wstring()+L"\\";
    Sync(base,{571,65,4164,true},true);
    Configure(base);
    Require(SelectScope(2)&&editGlobal,"Global scope selectable with verified location");
    Require(HitScope(layout.Scope(2).x+1,layout.Scope(2).y+1)==2,"Global scope hit test");
    layout.SelectTab(0);
    auto& item=items[Tabs()[0].begin];
    item.value=item.value==item.lo?item.hi:item.lo;
    Require(Save(item)&&HasGlobalValue(item.section,item.key),"Global tab writes explicit override");
    Require(SelectScope(0)&&!editGlobal&&editZone,"zone scope selectable after Global");
    Require(!CanClearGlobalTab()&&!ClearGlobalActiveTab(),"zone scope cannot clear Global overrides");
    Require(SelectScope(1)&&!editGlobal&&!editZone,"area scope selectable after Global");
    Require(SelectScope(2),"return to Global");
    // A different category must not be cleared with the active category.
    const auto& other=items[Tabs()[1].begin];
    Require(renderer::locationtuning::Save(other.section,other.key,other.lo),"save another Global category");
    Require(ClearGlobalActiveTab()&&!HasGlobalValue(item.section,item.key),"Clear Overrides removes active category overrides");
    Require(HasGlobalValue(other.section,other.key),"Clear Overrides preserves other Global categories");
    Require(!ClearGlobalActiveTab(),"Clear Overrides is idempotent");
    // Reset means explicit defaults in Global, not removal/inheritance.
    item.value=item.value==item.lo?item.hi:item.lo;
    Require(Save(item)&&ResetActiveTab(),"Global Reset Defaults reports changes");
    for(int index=Tabs()[0].begin;index<Tabs()[0].end;++index)
        Require(HasGlobalValue(items[index].section,items[index].key),"Global Reset Defaults persists every key");
    Require(ClearGlobalActiveTab(),"Global defaults can be cleared");
    layout.SelectTab(int(Tabs().size())-1);
    Require(!CanClearGlobalTab(),"Image & Color retains existing global-only behavior");
    Sync(base,{530,10127,0,true},true);
    Require(!SelectScope(1)&&SelectScope(0)&&SelectScope(2),"Area 0 disables only subarea, not Global");
    Sync(base,{},false);
    Require(!SelectScope(0)&&!SelectScope(1)&&SelectScope(2),"unverified location still permits Global");
    editGlobal=false;editZone=false;layout.SelectTab(0);
    std::printf("PASS Global scope selection, persistence, clearing and reset behavior\n");
}
int main() try {
    TestResetDefaults();
    TestGlobalScope();
    wchar_t path[MAX_PATH]{}; GetSystemDirectoryW(path, MAX_PATH); wcscat_s(path, L"\\d3d9.dll");
    auto runtime = LoadLibraryW(path); Require(runtime != nullptr, "system D3D9 loaded");
    auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(runtime, "Direct3DCreate9"));
    Require(create != nullptr, "Direct3DCreate9 found");
    ComPtr<IDirect3D9> api; api.Attach(create(D3D_SDK_VERSION)); Require(bool(api), "D3D9 available");
    HWND window = CreateWindowW(L"STATIC", L"F7 state regression", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window; pp.BackBufferWidth = 1280; pp.BackBufferHeight = 720; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    HRESULT hr = api->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.GetAddressOf());
    if (FAILED(hr)) { std::printf("SKIP GPU tests: no D3D9 HAL (hr=%08lx)\n", hr); DestroyWindow(window); return 77; }
    auto* d = device.Get();
    ComPtr<ID3DBlob> vsCode, psCode;
    const char* vsText = "float4 main(float4 p:POSITION):POSITION{return p;}";
    const char* psText = "float4 main():COLOR0{return float4(1,1,1,1);}";
    Check(D3DCompile(vsText, strlen(vsText), nullptr, nullptr, nullptr, "main", "vs_3_0", 0, 0, vsCode.GetAddressOf(), nullptr));
    Check(D3DCompile(psText, strlen(psText), nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0, psCode.GetAddressOf(), nullptr));
    ComPtr<IDirect3DVertexShader9> vs; ComPtr<IDirect3DPixelShader9> ps;
    Check(d->CreateVertexShader(static_cast<DWORD*>(vsCode->GetBufferPointer()), vs.GetAddressOf()));
    Check(d->CreatePixelShader(static_cast<DWORD*>(psCode->GetBufferPointer()), ps.GetAddressOf()));
    {
        SetterHooks hooks(d);
        Check(d->SetVertexShader(vs.Get())); Check(d->SetPixelShader(ps.Get()));
        Check(d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1));
        ComPtr<IDirect3DVertexDeclaration9> decl; Check(d->GetVertexDeclaration(decl.GetAddressOf()));
        renderer::g_trackedState.currentVDecl = decl.Get();
        Check(d->SetRenderState(D3DRS_ZENABLE, TRUE)); Check(d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
        Check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE)); Check(d->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE));
        const auto world = renderer::g_trackedState;
        // No world setters inside this loop: the game can cache them across
        // frames. Check immediately after Draw, not after a repair in Present.
        for (unsigned frame = 0; frame < 180; ++frame) {
            tuningoverlay::visible = frame < 150; // also cover close/reopen
            if (frame >= 170) tuningoverlay::visible = true;
            const unsigned before = setterCalls;
            Check(d->BeginScene());
            renderer::WindState wind; wind.debugView=frame%3!=0;
            tuningoverlay::DrawWindDebug(d,wind);
            CheckRestored(d,world); // Debug works with F7 closed, without state leakage.
            if(!wind.debugView)Require(setterCalls==before,"disabled wind HUD touched the device");
            tuningoverlay::Draw(d); Check(d->EndScene());
            Require(!tuningoverlay::visible || setterCalls > before, "test did not observe menu setters");
            CheckRestored(d, world);
        }
        // An early exit must restore the GPU state and mirror, not just one.
        const auto earlyReturn = [&] {
            renderer::ScopedStateBlock saved(d); Check(saved.CaptureResult());
            Check(d->SetVertexShader(nullptr)); Check(d->SetRenderState(D3DRS_ZENABLE, FALSE));
            return;
        };
        earlyReturn(); CheckRestored(d, world);
        tuningoverlay::font.Reset();
        tuningoverlay::windDebugFont.Reset();
    }
    tuningoverlay::visible = false;
    DestroyWindow(window);
    std::puts("PASS F7 open/closed/reopened: actual GPU and tracked state survive hooked menu drawing");
    return 0;
} catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
