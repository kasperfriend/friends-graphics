#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <mutex>
#include "WaterDiagnostics.h"
#include "src/Diagnostics/CelestialDrawDiagnostics.h"
#include "WaterHighlight.h"
#include "CelestialHighlight.h"
#include "src/Effects/VegetationSway.h"
#include "WaterEffect.h"
#include "DistanceFog.h"
#include "src/Effects/EnvironmentFogCapture.h"
#include "VolumeIntegration.h"
#include "WaterReflection.h"
#include "WeatherVisuals.h"
#include "TuningOverlay.h"
#include "src/Core/ShaderCache.h"
#include "src/Core/FrameContext.h"
#include "src/Core/DeviceHealth.h"
#include "src/Diagnostics/RendererDiagnostics.h"
#include "src/D3D9/DepthCapture.h"
#include "src/D3D9/CameraCapture.h"
#include "src/D3D9/TrackedRenderState.h"
#include "src/D3D9/CelestialTracker.h"
#include "src/Scene/DrawCallClassifier.h"
#include "NativeShadowDiagnostics.h"
#include "src/Materials/MaterialCacheManager.h"
#include "src/Lighting/LocalLightManager.h"
#include "src/Effects/GlobalIllumination.h"
#include "src/Effects/LocalLightingRenderer.h"
#include "src/Effects/GroundSurfaceCapture.h"
#include "src/Environment/WindTuning.h"

static_assert(renderer::kD3DErrDeviceLost == D3DERR_DEVICELOST, "src/Core/DeviceHealth.h must match d3d9.h");
static_assert(renderer::kD3DErrDeviceNotReset == D3DERR_DEVICENOTRESET, "src/Core/DeviceHealth.h must match d3d9.h");

namespace
{
using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
using Direct3DCreate9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
using EndSceneFn = HRESULT (WINAPI*)(IDirect3DDevice9*);
using PresentFn = HRESULT (WINAPI*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
using ResetFn = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
using DrawFn = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT);
using DrawIndexedFn = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
using DrawUPFn = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
using DrawIndexedUPFn = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
using SetLightFn = HRESULT (WINAPI*)(IDirect3DDevice9*,DWORD,const D3DLIGHT9*);
using LightEnableFn = HRESULT (WINAPI*)(IDirect3DDevice9*,DWORD,BOOL);
PresentFn originalPresent=nullptr;
ResetFn originalReset=nullptr;
DrawFn originalDraw=nullptr;
DrawIndexedFn originalDrawIndexed=nullptr;
DrawUPFn originalDrawUP=nullptr;
DrawIndexedUPFn originalDrawIndexedUP=nullptr;
SetLightFn originalSetLight=nullptr;
LightEnableFn originalLightEnable=nullptr;
using ClearFn=HRESULT(WINAPI*)(IDirect3DDevice9*,DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD);
ClearFn originalClear=nullptr;
bool unified=false,graphicsActive=true,graphicsKeyDown=false,graphicsShowStatus=false,tuningKeyDown=false;
// Alt+Tab / device-loss state. Only touched on the render thread. A gap longer
// than kFrameGapMs between presents means the frame stream was interrupted.
constexpr uint64_t kFrameGapMs = 1000;
renderer::DeviceAvailability g_availability = renderer::DeviceAvailability::Operational;
renderer::FrameGapDetector g_frameGap;
renderer::WindSystem g_windSystem;
renderer::FrameClock g_windClock;

HMODULE g_systemD3D9 = nullptr;
Direct3DCreate9Fn g_realCreate9 = nullptr;
Direct3DCreate9ExFn g_realCreate9Ex = nullptr;
EndSceneFn g_originalEndScene = nullptr;
std::mutex g_hookMutex;
void** g_hookedTable = nullptr;
std::once_flag g_initialize;
thread_local bool g_drawingOverlay = false;
std::wstring g_basePath;

void Log(char const* text) noexcept
{
    // A logging failure (or an allocation throw from the string concat) must
    // never escape into a hooked D3D9 call: an exception unwinding through
    // the driver's COM dispatch is how a zone-change hiccup becomes a
    // permanently frozen image.
    try
    {
        std::wofstream out(g_basePath + L"ModernWoWRenderer.log", std::ios::app);
        out << text << '\n';
    }
    catch (...) {}
}

void SetAvailability(renderer::DeviceAvailability next)
{
    if (next == g_availability) return;
    g_availability = next;
    // Presents keep arriving while the device is lost, so the frame-gap check
    // alone would not notice. History from either side of the transition is stale.
    volume::InvalidateTemporalHistory();
    Log(next == renderer::DeviceAvailability::Lost
        ? "Direct3D device lost (Alt+Tab, mode change or driver reset): effects paused until it is restored."
        : "Direct3D device restored: effects resumed.");
}

void ReloadVegetationTuning()
{
    const auto ini=g_basePath+L"GraphicsEffects.ini";
    vegetation::Configure(renderer::ReadVegetationSettings([&](const wchar_t* key,int fallback) {
        return static_cast<int>(renderer::locationtuning::ReadInt(L"Vegetation",key,fallback,ini.c_str()));
    }));
}

void ReloadWindTuning()
{
    const auto ini=g_basePath+L"GraphicsEffects.ini";
    g_windSystem.Configure(renderer::ReadWindSettings([&](const wchar_t* key,int fallback) {
        return static_cast<int>(renderer::locationtuning::ReadInt(L"Wind",key,fallback,ini.c_str()));
    }));
    ReloadVegetationTuning();
}

void RestoreDeviceHooksAfterReset(IDirect3DDevice9* device);
void ResetEffectStateForDeviceChange(IDirect3DDevice9* d) noexcept;

HRESULT WINAPI HookedEndScene(IDirect3DDevice9* device)
{
    // Overlays cannot draw on a lost device; forward the call untouched.
    if (g_availability == renderer::DeviceAvailability::Lost) return g_originalEndScene(device);
    g_drawingOverlay=true;
    try {
    if(unified){if(graphicsShowStatus){watereffect::DrawStatus(device,graphicsActive?"FX ON":"FX OFF",0);
        if(graphicsActive&&volume::enabled)watereffect::DrawStatus(device,volume::composed?"AIR ON":"AIR WAIT",1);}
    }
    else {watereffect::DrawStatus(device);
    if(distancefog::showStatus){
        const auto& fog=renderer::DirectionalVolumetricLighting::Instance().Settings();
        const bool volumeEdge=volume::enabled&&volume::active&&volume::composed&&fog.enabled&&fog.edgeFogEnabled;
        watereffect::DrawStatus(device,!distancefog::active||!distancefog::effectEnabled?"HAZE OFF":
            volumeEdge||distancefog::matches?"HAZE ON":"HAZE WAIT",1);
    }}
    if(weathervisuals::enabled&&weathervisuals::showStatus)watereffect::DrawStatus(device,weathervisuals::GetStatusText(),3);
    nativeshadowdiag::DrawPreview(device);
    tuningoverlay::DrawWindDebug(device,renderer::FrameContext::Current().wind,vegetation::DebugLine());
    tuningoverlay::Draw(device);
    renderer::MaterialCacheManager::Instance().DrawDebugOverlay(device);
    } catch (...) {
        // An overlay exception must not strand g_drawingOverlay=true (which
        // would disable every effect for the rest of the session) nor reach
        // the game: the original EndScene still runs.
    }
    g_drawingOverlay=false;
    return g_originalEndScene(device);
}

HRESULT WINAPI HookedPresent(IDirect3DDevice9* d,const RECT* a,const RECT* b,HWND w,const RGNDATA* r) {
    const uint64_t now = GetTickCount64();
    // Every Present is a frame boundary. A long pause means the frame stream
    // was interrupted (Alt+Tab, minimised window, loading screen), so temporal
    // history from before the pause must not be blended into the next frame.
    if (g_frameGap.Observe(now, kFrameGapMs)) volume::InvalidateTemporalHistory();

    // One focus sample per frame, shared by every hotkey (F7-F12) below
    // and in the module Present paths.
    {
        HWND foreground = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        renderer::FrameContext::Current().wowFocused = pid == GetCurrentProcessId();
        renderer::FrameContext::Current().wowForeground = foreground;
    }

    if (g_availability == renderer::DeviceAvailability::Lost) {
        // No effect pass can draw while the device is lost. Skip them instead
        // of letting each one fail (and log) every frame. Probe for recovery;
        // a device that needs a Reset is restored by HookedReset.
        if (SUCCEEDED(d->TestCooperativeLevel())) {
            SetAvailability(renderer::DeviceAvailability::Operational);
        } else {
            // Keep the per-frame state consistent and the weather detection
            // alive while the frame passes through untouched.
            waterreflection::Finish();
            volume::Finish(d);
            weathervisuals::Present(d, true);
            return originalPresent(d, a, b, w, r);
        }
    }

    // Scene depth is the strongest world-frame signal, but it is optional when
    // water is configured for waves without any scene-dependent features. In
    // that case a known terrain draw or accepted liquid-material draw is the
    // independent signal. A focused menu/loading frame with neither is NOT a
    // location sample: the provider would return an invalid tuple, whose debounce
    // eventually clears zone tuning.
    try {
    {
        const bool knownWorldDraw = weathervisuals::worldGeometrySeenThisFrame || watereffect::frameCandidates>0;
        const bool worldFrame = renderer::HasWorldContext(
            renderer::DepthCapture::Instance().HasDepth(), knownWorldDraw);
        auto& environment = renderer::EnvironmentProfileManager::Instance();
        environment.Update(now, worldFrame,
            renderer::ShouldHoldLocation(worldFrame, environment.Location().valid));
    }
    renderer::FrameContext::Current().environment=renderer::EnvironmentProfileManager::Instance().State();
    waterreflection::Present();
    volume::Present(d);
    renderer::MaterialCacheManager::Instance().Present(d);
    weathervisuals::Present(d,tuningoverlay::visible);
    const auto& environmentManager=renderer::EnvironmentProfileManager::Instance();
    const bool locationTuningChanged=renderer::locationtuning::Sync(g_basePath,environmentManager.Location(),environmentManager.LocationTuningVerified());
    if(locationTuningChanged)tuningoverlay::Configure(g_basePath);
    if(tuningoverlay::Update()||locationTuningChanged){ReloadWindTuning();watereffect::ReloadTuning();distancefog::ReloadTuning();volume::ReloadTuning();celestialhighlight::ReloadTuning(g_basePath);nativeshadowdiag::ReloadEnhancement();renderer::MaterialCacheManager::Instance().ReloadTuning(g_basePath);renderer::LocalLightManager::Instance().ReloadTuning();renderer::GlobalIllumination::Instance().ReloadTuning();weathervisuals::ReloadTuning();Log("Location tuning reloaded");}
    if(unified){
        bool down=(GetAsyncKeyState(VK_F11)&0x8000)!=0;
        if(down&&!graphicsKeyDown&&renderer::FrameContext::Current().wowFocused){
            graphicsActive=!graphicsActive;watereffect::active=distancefog::active=volume::active=weathervisuals::active=graphicsActive;vegetation::active=graphicsActive;
            Log(graphicsActive?"F11 graphics ON":"F11 graphics OFF");
        }
        graphicsKeyDown=down;
    }
    {
        bool down=(GetAsyncKeyState(VK_F12)&0x8000)!=0;
        if(down&&!tuningKeyDown&&renderer::FrameContext::Current().wowFocused){ReloadWindTuning();watereffect::ReloadTuning();distancefog::ReloadTuning();volume::ReloadTuning();celestialhighlight::ReloadTuning(g_basePath);nativeshadowdiag::ReloadEnhancement();renderer::MaterialCacheManager::Instance().ReloadTuning(g_basePath);renderer::LocalLightManager::Instance().ReloadTuning();renderer::GlobalIllumination::Instance().ReloadTuning();weathervisuals::ReloadTuning();renderer::EnvironmentProfileManager::Instance().Reload();tuningoverlay::Configure(g_basePath);waterdiag::Configure(g_basePath);Log("F12 reloaded GraphicsEffects.ini, EnvironmentProfiles.ini and water diagnostic settings");}
        tuningKeyDown=down;
    }
    // All scene/cloud rendering has finished. Publish ONE wind snapshot for
    // the NEXT frame, then prepare its sea state. Advancing before volume's
    // fallback composite would give clouds a different wind from the water
    // already drawn in this frame. Detection is held independently of F10,
    // replacement textures and F7; do not infer severity from particle count.
    auto& nextFrame=renderer::FrameContext::Current();
    const auto weather=renderer::ResolveWindWeather(weathervisuals::nativeWeatherMode,
        weathervisuals::intensity,nextFrame.environment[renderer::Weather]);
    nextFrame.wind=g_windSystem.Update(weather,
        g_windClock.Observe(renderer::PerformanceCounter(),renderer::PerformanceFrequency()));
    // The sway is a consumer of that one snapshot, advanced by the snapshot's
    // own bounded delta: no second clock, no second wind. BeginFrame starts the
    // new frame's counters and forgets nothing else, so an F7 edit, a reload,
    // a device reset or an Alt+Tab cannot move a plant.
    vegetation::Update(nextFrame.wind);
    vegetation::BeginFrame();
    volume::cloudWeather=static_cast<renderer::CloudWeather>(weather.kind);
    volume::cloudWeatherStrength=weather.strength;
    watereffect::FeedWeather(weather.kind,weather.strength);
    waterhighlight::Present();
    watereffect::Present();
    distancefog::Present();
    waterdiag::Present(d);
    celestialdiag::Present(d);
    nativeshadowdiag::OnPresent(d);
    renderer::RendererDiagnostics::Instance().OnFrameEnd();
    } catch (...) {
        // A throw anywhere in the effect frame (INI read, allocation, a file
        // op) must never reach the game: the frame still presents, the effect
        // state that a reload was halfway through re-arms on the next zone
        // change or F12, and the device handshake is untouched.
        Log("Present: effect pass threw; the frame still presents.");
    }
    const HRESULT presentResult = originalPresent(d,a,b,w,r);
    SetAvailability(renderer::ClassifyDeviceResult(presentResult));
    return presentResult;
}
// Drops every effect's device resources and cached GPU identity. Runs before
// the game's own Reset, and again when the game replaces the device entirely
// (a discarded device never gets a Reset, so the effects would otherwise
// keep holding the old device's dead resources).
void ResetEffectStateForDeviceChange(IDirect3DDevice9* d) noexcept
{
    try {
    tuningoverlay::font.Reset();
    tuningoverlay::windDebugFont.Reset();
    waterreflection::Reset(d);
    volume::Reset(d);
    watereffect::Reset(d);
    waterdiag::Reset(d);
    celestialdiag::Reset(d);
    nativeshadowdiag::Reset();
    vegetation::Reset(d);
    renderer::MaterialCacheManager::Instance().Reset(d);
    renderer::LocalLightingRenderer::Instance().Reset(d);
    renderer::LocalLightManager::Instance().Reset();
    weathervisuals::Reset(d);
    renderer::ShaderCache::Instance().Clear();
    renderer::DrawCallClassifier::Instance().ClearCache();
    renderer::DepthCapture::Instance().Reset(d);
    renderer::CameraCapture::Instance().Reset();
    renderer::g_trackedState={};
    } catch (...) {
        // Effect cleanup may throw (a failed file op, an allocation). It must
        // never block the game's own reset: partially cleaned state is
        // harmless - every readiness gate checks live, and the next reset or
        // the next frame finishes the job. Skipping originalReset here is
        // how a one-off error becomes a device that never comes back.
        Log("Reset: effect cleanup threw; the device reset still runs.");
    }
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* p) {
    ResetEffectStateForDeviceChange(d);
    HRESULT hr=originalReset(d,p);
    try {
    if(SUCCEEDED(hr)){
        RestoreDeviceHooksAfterReset(d);
        SetAvailability(renderer::DeviceAvailability::Operational);
    }
    else {
        // Stay paused; Present keeps probing and resumes once the device is usable.
        SetAvailability(renderer::DeviceAvailability::Lost);
    }
    } catch (...) {
        SetAvailability(renderer::DeviceAvailability::Lost);
    }
    return hr;
}
HRESULT WINAPI HookedClear(IDirect3DDevice9* d,DWORD n,const D3DRECT* rect,DWORD flags,D3DCOLOR color,float z,DWORD stencil){
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalClear(d,n,rect,flags,color,z,stencil);
    const bool waterNeedsDepth=watereffect::NeedsScene();
    if(volume::internal||g_drawingOverlay||(unified&&!graphicsActive)||(!volume::HasActiveEffects()&&!waterNeedsDepth))
        return originalClear(d,n,rect,flags,color,z,stencil);
    try { volume::BeforeClear(d,n,rect,flags,z); } catch (...) {}
    return originalClear(d,n,rect,flags,color,z,stencil);
}
using SetRenderStateFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using SetRenderTargetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetVertexDeclarationFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using SetFVFFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD);
using SetVertexShaderFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetPixelShaderFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);

SetRenderStateFn originalSetRenderState = nullptr;
SetRenderTargetFn originalSetRenderTarget = nullptr;
SetTextureFn originalSetTexture = nullptr;
SetVertexDeclarationFn originalSetVertexDeclaration = nullptr;
SetFVFFn originalSetFVF = nullptr;
SetVertexShaderFn originalSetVertexShader = nullptr;
SetPixelShaderFn originalSetPixelShader = nullptr;

HRESULT WINAPI HookedSetLight(IDirect3DDevice9* d, DWORD index, const D3DLIGHT9* light)
{
    const HRESULT hr = originalSetLight(d, index, light);
    if (SUCCEEDED(hr) && !volume::internal && !weathervisuals::internal && !g_drawingOverlay &&
        !renderer::MaterialCacheManager::Instance().IsInternalPass())
        renderer::LocalLightManager::Instance().OnSetLight(index, light);
    return hr;
}

HRESULT WINAPI HookedLightEnable(IDirect3DDevice9* d, DWORD index, BOOL enabled)
{
    const HRESULT hr = originalLightEnable(d, index, enabled);
    if (SUCCEEDED(hr) && !volume::internal && !weathervisuals::internal && !g_drawingOverlay &&
        !renderer::MaterialCacheManager::Instance().IsInternalPass())
        renderer::LocalLightManager::Instance().OnLightEnable(index, enabled);
    return hr;
}

HRESULT WINAPI HookedSetRenderTarget(IDirect3DDevice9* d, DWORD index, IDirect3DSurface9* surface)
{
    const HRESULT hr = originalSetRenderTarget(d, index, surface);
    if (SUCCEEDED(hr) && !volume::internal && !weathervisuals::internal && !g_drawingOverlay &&
        !renderer::MaterialCacheManager::Instance().IsInternalPass())
        try { nativeshadowdiag::OnSetRenderTarget(index, surface); } catch (...) {}
    return hr;
}

HRESULT WINAPI HookedSetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* texture)
{
    const HRESULT hr = originalSetTexture(d, stage, texture);
    if (SUCCEEDED(hr) && !volume::internal && !weathervisuals::internal && !g_drawingOverlay &&
        !renderer::MaterialCacheManager::Instance().IsInternalPass())
    {
        try {
            nativeshadowdiag::OnSetTexture(stage, texture);
            renderer::MaterialCacheManager::Instance().OnSetTexture(stage, texture);
        } catch (...) {}
    }
    return hr;
}

HRESULT WINAPI HookedSetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* vs)
{
    if (renderer::g_trackedState.currentVS != vs || (vs && renderer::g_trackedState.vsHash == 0))
    {
        try {
            // Hash first, then commit both: a throw leaves the mirror
            // untouched so the next setter retries instead of stranding a
            // stale hash under the new shader pointer.
            const uint64_t h = (vs && (!unified || graphicsActive)) ? renderer::ShaderCache::Instance().GetShaderHash(vs) : 0;
            renderer::g_trackedState.vsHash = h;
            renderer::g_trackedState.currentVS = vs;
        } catch (...) {
            // A hashing throw must not reach the game; the original setter
            // still runs and the mirror keeps its previous identity.
        }
    }
    return originalSetVertexShader(d, vs);
}

HRESULT WINAPI HookedSetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* ps)
{
    if (!weathervisuals::internal &&
        (renderer::g_trackedState.currentPS != ps || (ps && renderer::g_trackedState.psHash == 0)))
    {
        try {
            const uint64_t h = (ps && (!unified || graphicsActive)) ? renderer::ShaderCache::Instance().GetShaderHash(ps) : 0;
            renderer::g_trackedState.psHash = h;
            renderer::g_trackedState.currentPS = ps;
        } catch (...) {
            // Same as the vertex path: swallow, keep the old identity, and
            // let the original setter run.
        }
    }
    return originalSetPixelShader(d, ps);
}

HRESULT WINAPI HookedSetVertexDeclaration(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* decl)
{
    renderer::g_trackedState.currentVDecl = decl;
    return originalSetVertexDeclaration(d, decl);
}

HRESULT WINAPI HookedSetFVF(IDirect3DDevice9* d, DWORD fvf)
{
    renderer::g_trackedState.currentFVF = fvf;
    return originalSetFVF(d, fvf);
}

HRESULT WINAPI HookedSetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE state, DWORD val)
{
    switch (state)
    {
    case D3DRS_ALPHABLENDENABLE: renderer::g_trackedState.alphaBlend = (val != 0); break;
    case D3DRS_ALPHATESTENABLE:  renderer::g_trackedState.alphaTest = (val != 0); break;
    case D3DRS_ZWRITEENABLE:     renderer::g_trackedState.zWrite = (val != 0); break;
    case D3DRS_ZENABLE:          renderer::g_trackedState.zEnable = (val != 0); break;
    default: break;
    }
    return originalSetRenderState(d, state, val);
}

renderer::DrawClassification ClassifyCurrentDraw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n,
    const renderer::water::Observation* waterObservation=nullptr)
{
    renderer::DrawCallContext ctx;
    ctx.primitiveType = t;
    ctx.primitiveCount = n;
    ctx.vertexShader = renderer::g_trackedState.currentVS;
    ctx.pixelShader = renderer::g_trackedState.currentPS;
    ctx.vertexDecl = renderer::g_trackedState.currentVDecl;
    ctx.vsHash = renderer::g_trackedState.vsHash;
    ctx.psHash = renderer::g_trackedState.psHash;
    ctx.alphaBlend = renderer::g_trackedState.alphaBlend;
    ctx.alphaTest = renderer::g_trackedState.alphaTest;
    ctx.zWrite = renderer::g_trackedState.zWrite;
    ctx.zEnable = renderer::g_trackedState.zEnable;

    auto dc = renderer::DrawCallClassifier::Instance().Classify(
        d, ctx, volume::capturedViewTranslation, volume::capturedViewValid,
        volume::cameraCaptureShaderHash, waterObservation);
    renderer::RendererDiagnostics::Instance().RecordDrawCall(dc);
    return dc;
}

struct WaterDrawObservation {
    renderer::water::Observation material{};
    bool available=false;
};
WaterDrawObservation ObserveWaterForDraw(IDirect3DDevice9* d,bool requested) {
    const bool effectNeedsMaterial=watereffect::enabled&&watereffect::active&&watereffect::effectEnabled;
    if(g_drawingOverlay||(!requested&&!effectNeedsMaterial))return {};
    return {renderer::water::Observe(d),true};
}

HRESULT WINAPI HookedDraw(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT start,UINT n) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDraw(d,t,start,n);
    if(volume::internal||weathervisuals::internal)return originalDraw(d,t,start,n);
    bool gameDrawRan=false;
    try {
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawPrimitive",t,n);
    if(unified&&!graphicsActive)return originalDraw(d,t,start,n);
    if(!g_drawingOverlay){weathervisuals::ObserveWorldDraw();nativeshadowdiag::OnDraw(d,"DrawPrimitive",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);}
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    const bool updateShadows=!g_drawingOverlay&&volume::ShouldUpdateShadows();
    const bool captureGround=n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed;
    const bool needsWaterMaterial=updateShadows||captureGround||
        renderer::MaterialCacheManager::Instance().IsActive();
    const auto waterDraw=ObserveWaterForDraw(d,needsWaterMaterial);
    if(updateShadows){
        auto dc=ClassifyCurrentDraw(d,t,n,waterDraw.available?&waterDraw.material:nullptr);
        volume::ShadowDraw(d,dc,[&]{gameDrawRan=true;return originalDraw(d,t,start,n);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d,waterDraw.available?&waterDraw.material:nullptr);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawPrimitive",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,0,start,n,false);
    if(captureGround){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{gameDrawRan=true;return originalDraw(d,t,start,n);},waterDraw.available?&waterDraw.material:nullptr);volume::internal=false;}
    vegetation::Scope vegetationSway(d,g_drawingOverlay);
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed,
        waterDraw.available?&waterDraw.material:nullptr);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDraw(d,t,start,n);
    gameDrawRan=true;
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
    return hr;
    } catch (...) {
        // An effect throw must never drop the game's draw nor strand the
        // internal flags (a stuck `volume::internal` would silence every
        // effect for the rest of the session): clear both, and run the game's
        // draw below if the pass threw before it ran. The injected per-draw
        // state is gone (its scopes unwound), but the geometry still draws.
        volume::internal=false;
        weathervisuals::internal=false;
        if (gameDrawRan) return S_OK;
        // The retry itself may throw (the driver call is the very thing that
        // just threw); a swallowed driver error is preferable to an exception
        // escaping the hook.
        try { return originalDraw(d,t,start,n); } catch (...) { return 0; }
    }
}

HRESULT WINAPI HookedDrawIndexed(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,INT base,UINT min,UINT vertices,UINT start,UINT n) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDrawIndexed(d,t,base,min,vertices,start,n);
    if(volume::internal||weathervisuals::internal)return originalDrawIndexed(d,t,base,min,vertices,start,n);
    bool gameDrawRan=false;
    try {
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawIndexedPrimitive",t,n);
    if(unified&&!graphicsActive)return originalDrawIndexed(d,t,base,min,vertices,start,n);
    if(!g_drawingOverlay){weathervisuals::ObserveWorldDraw();nativeshadowdiag::OnDraw(d,"DrawIndexedPrimitive",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);}
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    const bool updateShadows=!g_drawingOverlay&&volume::ShouldUpdateShadows();
    const bool captureGround=n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed;
    const bool needsWaterMaterial=updateShadows||captureGround||
        renderer::MaterialCacheManager::Instance().IsActive();
    const auto waterDraw=ObserveWaterForDraw(d,needsWaterMaterial);
    if(updateShadows){
        auto dc=ClassifyCurrentDraw(d,t,n,waterDraw.available?&waterDraw.material:nullptr);
        volume::ShadowDraw(d,dc,[&]{gameDrawRan=true;return originalDrawIndexed(d,t,base,min,vertices,start,n);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d,waterDraw.available?&waterDraw.material:nullptr);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawIndexedPrimitive",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,base,start,n);
    if(captureGround){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{gameDrawRan=true;return originalDrawIndexed(d,t,base,min,vertices,start,n);},waterDraw.available?&waterDraw.material:nullptr);volume::internal=false;}
    vegetation::Scope vegetationSway(d,g_drawingOverlay);
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed,
        waterDraw.available?&waterDraw.material:nullptr);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDrawIndexed(d,t,base,min,vertices,start,n);
    gameDrawRan=true;
    if(SUCCEEDED(hr)&&!g_drawingOverlay&&renderer::MaterialCacheManager::Instance().IsActive()){
            auto dc=ClassifyCurrentDraw(d,t,n,waterDraw.available?&waterDraw.material:nullptr);
            if(dc.material==renderer::MaterialType::Terrain){
                renderer::MaterialCacheManager::Instance().ApplyNormalModulation(d,t,base,min,vertices,start,n,[&]{gameDrawRan=true;return originalDrawIndexed(d,t,base,min,vertices,start,n);});
            }
            else if(dc.material==renderer::MaterialType::WMO||dc.material==renderer::MaterialType::M2){
                renderer::MaterialCacheManager::Instance().ApplyObjectMaterial(d,n,[&]{gameDrawRan=true;return originalDrawIndexed(d,t,base,min,vertices,start,n);});
            }
        }
        if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
        return hr;
    } catch (...) {
        // An effect throw must never drop the game's draw nor strand the
        // internal flags (a stuck `volume::internal` would silence every
        // effect for the rest of the session): clear both, and run the game's
        // draw below if the pass threw before it ran. The injected per-draw
        // state is gone (its scopes unwound), but the geometry still draws.
        volume::internal=false;
        weathervisuals::internal=false;
        if (gameDrawRan) return S_OK;
        // The retry itself may throw (the driver call is the very thing that
        // just threw); a swallowed driver error is preferable to an exception
        // escaping the hook.
        try { return originalDrawIndexed(d,t,base,min,vertices,start,n); } catch (...) { return 0; }
    }
}

HRESULT WINAPI HookedDrawUP(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT n,const void* v,UINT stride) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDrawUP(d,t,n,v,stride);
    if(volume::internal||weathervisuals::internal)return originalDrawUP(d,t,n,v,stride);
    bool gameDrawRan=false;
    try {
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawPrimitiveUP",t,n);
    if(unified&&!graphicsActive)return originalDrawUP(d,t,n,v,stride);
    if(!g_drawingOverlay)nativeshadowdiag::OnDraw(d,"DrawPrimitiveUP",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    const bool updateShadows=!g_drawingOverlay&&volume::ShouldUpdateShadows();
    const bool captureGround=n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed;
    const bool needsWaterMaterial=updateShadows||captureGround||
        renderer::MaterialCacheManager::Instance().IsActive();
    const auto waterDraw=ObserveWaterForDraw(d,needsWaterMaterial);
    if(updateShadows){
        auto dc=ClassifyCurrentDraw(d,t,n,waterDraw.available?&waterDraw.material:nullptr);
        volume::ShadowDraw(d,dc,[&]{gameDrawRan=true;return originalDrawUP(d,t,n,v,stride);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d,waterDraw.available?&waterDraw.material:nullptr);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawPrimitiveUP",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,0,0,n,false,v,stride);
    if(captureGround){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{gameDrawRan=true;return originalDrawUP(d,t,n,v,stride);},waterDraw.available?&waterDraw.material:nullptr);volume::internal=false;}
    vegetation::Scope vegetationSway(d,g_drawingOverlay);
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed,
        waterDraw.available?&waterDraw.material:nullptr);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::ObserveWorldDraw();
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDrawUP(d,t,n,v,stride);
    gameDrawRan=true;
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
        return hr;
    } catch (...) {
        // An effect throw must never drop the game's draw nor strand the
        // internal flags (a stuck `volume::internal` would silence every
        // effect for the rest of the session): clear both, and run the game's
        // draw below if the pass threw before it ran. The injected per-draw
        // state is gone (its scopes unwound), but the geometry still draws.
        volume::internal=false;
        weathervisuals::internal=false;
        if (gameDrawRan) return S_OK;
        // The retry itself may throw (the driver call is the very thing that
        // just threw); a swallowed driver error is preferable to an exception
        // escaping the hook.
        try { return originalDrawUP(d,t,n,v,stride); } catch (...) { return 0; }
    }
}

HRESULT WINAPI HookedDrawIndexedUP(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT min,UINT vertices,UINT n,const void* indices,D3DFORMAT f,const void* v,UINT stride) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    if(volume::internal||weathervisuals::internal)return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    bool gameDrawRan=false;
    try {
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawIndexedPrimitiveUP",t,n);
    if(unified&&!graphicsActive)return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    if(!g_drawingOverlay)nativeshadowdiag::OnDraw(d,"DrawIndexedPrimitiveUP",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    const bool updateShadows=!g_drawingOverlay&&volume::ShouldUpdateShadows();
    const bool captureGround=n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed;
    const bool needsWaterMaterial=updateShadows||captureGround||
        renderer::MaterialCacheManager::Instance().IsActive();
    const auto waterDraw=ObserveWaterForDraw(d,needsWaterMaterial);
    if(updateShadows){
        auto dc=ClassifyCurrentDraw(d,t,n,waterDraw.available?&waterDraw.material:nullptr);
        volume::ShadowDraw(d,dc,[&]{gameDrawRan=true;return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d,waterDraw.available?&waterDraw.material:nullptr);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawIndexedPrimitiveUP",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,0,0,n,true,v,stride,indices,f,min+vertices);
    if(captureGround){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{gameDrawRan=true;return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);},waterDraw.available?&waterDraw.material:nullptr);volume::internal=false;}
    vegetation::Scope vegetationSway(d,g_drawingOverlay);
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed,
        waterDraw.available?&waterDraw.material:nullptr);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::ObserveWorldDraw();
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    gameDrawRan=true;
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
        return hr;
    } catch (...) {
        // An effect throw must never drop the game's draw nor strand the
        // internal flags (a stuck `volume::internal` would silence every
        // effect for the rest of the session): clear both, and run the game's
        // draw below if the pass threw before it ran. The injected per-draw
        // state is gone (its scopes unwound), but the geometry still draws.
        volume::internal=false;
        weathervisuals::internal=false;
        if (gameDrawRan) return S_OK;
        // The retry itself may throw (the driver call is the very thing that
        // just threw); a swallowed driver error is preferable to an exception
        // escaping the hook.
        try { return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride); } catch (...) { return 0; }
    }
}

void RestoreDeviceHooksAfterReset(IDirect3DDevice9* device)
{
    if(!device)return;
    void** table=*reinterpret_cast<void***>(device);
    DWORD oldProtect=0;
    if(!VirtualProtect(&table[16],95*sizeof(void*),PAGE_READWRITE,&oldProtect))return;

    InterlockedExchangePointer(&table[42],reinterpret_cast<void*>(HookedEndScene));
    InterlockedExchangePointer(&table[51],reinterpret_cast<void*>(HookedSetLight));
    InterlockedExchangePointer(&table[53],reinterpret_cast<void*>(HookedLightEnable));
    InterlockedExchangePointer(&table[37],reinterpret_cast<void*>(HookedSetRenderTarget));
    InterlockedExchangePointer(&table[57],reinterpret_cast<void*>(HookedSetRenderState));
    InterlockedExchangePointer(&table[65],reinterpret_cast<void*>(HookedSetTexture));
    InterlockedExchangePointer(&table[87],reinterpret_cast<void*>(HookedSetVertexDeclaration));
    InterlockedExchangePointer(&table[89],reinterpret_cast<void*>(HookedSetFVF));
    InterlockedExchangePointer(&table[92],reinterpret_cast<void*>(HookedSetVertexShader));
    InterlockedExchangePointer(&table[107],reinterpret_cast<void*>(HookedSetPixelShader));
    // Wind/weather observation and F7 need frame/draw hooks even when all
    // visual backends are disabled or a shader failed. Re-enabling is live.
    {
        InterlockedExchangePointer(&table[16],reinterpret_cast<void*>(HookedReset));
        InterlockedExchangePointer(&table[17],reinterpret_cast<void*>(HookedPresent));
        InterlockedExchangePointer(&table[81],reinterpret_cast<void*>(HookedDraw));
        InterlockedExchangePointer(&table[82],reinterpret_cast<void*>(HookedDrawIndexed));
        InterlockedExchangePointer(&table[83],reinterpret_cast<void*>(HookedDrawUP));
        InterlockedExchangePointer(&table[84],reinterpret_cast<void*>(HookedDrawIndexedUP));
    }
    // Water's backend is ready or still compiling (its startup shader compile
    // runs on a worker thread): install the capture hooks either way, since a
    // failed compile just leaves the hooks dormant (their per-frame gates all
    // check readiness live) and a late success must not need a device reset.
    if(volume::enabled||watereffect::enabled||watereffect::compiling){
        InterlockedExchangePointer(&table[39],reinterpret_cast<void*>(volume::SetDepth));
        InterlockedExchangePointer(&table[40],reinterpret_cast<void*>(volume::GetDepth));
        InterlockedExchangePointer(&table[43],reinterpret_cast<void*>(HookedClear));
    }
    DWORD ignored=0;
    VirtualProtect(&table[16],95*sizeof(void*),oldProtect,&ignored);
    g_hookedTable=table;
}

void HookDevice(IDirect3DDevice9* device)
{
    if (!device)
        return;
    std::lock_guard lock(g_hookMutex);
    void** table = *reinterpret_cast<void***>(device);
    if (g_hookedTable)
    {
        if (g_hookedTable == table)
            return;
        // A new device implementation: the game re-created its device
        // (driver recovery, full render re-init). The old table may belong
        // to a destroyed device - never touch it - but this one must be
        // hooked or every effect dies for the rest of the session while the
        // game itself keeps running normally. A discarded device never gets
        // a Reset, so drop the old device's resources now.
        Log("New device implementation: re-hooking.");
        ResetEffectStateForDeviceChange(device);
        SetAvailability(renderer::DeviceAvailability::Operational);
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(&table[16], 95*sizeof(void*), PAGE_READWRITE, &oldProtect))
        return;
    g_originalEndScene = reinterpret_cast<EndSceneFn>(table[42]);
    originalReset=reinterpret_cast<ResetFn>(table[16]);
    originalPresent=reinterpret_cast<PresentFn>(table[17]);
    originalSetRenderTarget=reinterpret_cast<SetRenderTargetFn>(table[37]);
    originalSetRenderState=reinterpret_cast<SetRenderStateFn>(table[57]);
    originalSetTexture=reinterpret_cast<SetTextureFn>(table[65]);
    originalDraw=reinterpret_cast<DrawFn>(table[81]);
    originalDrawIndexed=reinterpret_cast<DrawIndexedFn>(table[82]);
    originalDrawUP=reinterpret_cast<DrawUPFn>(table[83]);
    originalDrawIndexedUP=reinterpret_cast<DrawIndexedUPFn>(table[84]);
    originalSetVertexDeclaration=reinterpret_cast<SetVertexDeclarationFn>(table[87]);
    originalSetFVF=reinterpret_cast<SetFVFFn>(table[89]);
    originalSetVertexShader=reinterpret_cast<SetVertexShaderFn>(table[92]);
    originalSetPixelShader=reinterpret_cast<SetPixelShaderFn>(table[107]);
    originalSetLight=reinterpret_cast<SetLightFn>(table[51]);
    originalLightEnable=reinterpret_cast<LightEnableFn>(table[53]);

    InterlockedExchangePointer(&table[42], reinterpret_cast<void*>(HookedEndScene));
    InterlockedExchangePointer(&table[51], reinterpret_cast<void*>(HookedSetLight));
    InterlockedExchangePointer(&table[53], reinterpret_cast<void*>(HookedLightEnable));
    InterlockedExchangePointer(&table[37], reinterpret_cast<void*>(HookedSetRenderTarget));
    InterlockedExchangePointer(&table[57], reinterpret_cast<void*>(HookedSetRenderState));
    InterlockedExchangePointer(&table[65], reinterpret_cast<void*>(HookedSetTexture));
    InterlockedExchangePointer(&table[87], reinterpret_cast<void*>(HookedSetVertexDeclaration));
    InterlockedExchangePointer(&table[89], reinterpret_cast<void*>(HookedSetFVF));
    InterlockedExchangePointer(&table[92], reinterpret_cast<void*>(HookedSetVertexShader));
    InterlockedExchangePointer(&table[107], reinterpret_cast<void*>(HookedSetPixelShader));

    // CPU wind and its F7 controls must not depend on a working visual
    // backend. Draw hooks retain native weather observation while effects
    // are off; each optional effect still checks its own rendering gates.
    {
        InterlockedExchangePointer(&table[16],reinterpret_cast<void*>(HookedReset));
        InterlockedExchangePointer(&table[17],reinterpret_cast<void*>(HookedPresent));
        InterlockedExchangePointer(&table[81],reinterpret_cast<void*>(HookedDraw));
        InterlockedExchangePointer(&table[82],reinterpret_cast<void*>(HookedDrawIndexed));
        InterlockedExchangePointer(&table[83],reinterpret_cast<void*>(HookedDrawUP));
        InterlockedExchangePointer(&table[84],reinterpret_cast<void*>(HookedDrawIndexedUP));
    }
    // Same gate as the first hook pass: water's compile may still be in
    // flight (worker thread), and a late success must not need a reset.
    if(volume::enabled||watereffect::enabled||watereffect::compiling){
        volume::setDepth=reinterpret_cast<volume::SetDepthFn>(table[39]);
        volume::getDepth=reinterpret_cast<volume::GetDepthFn>(table[40]);
        renderer::DepthCapture::Instance().SetFunctions(volume::setDepth, volume::getDepth);
        originalClear=reinterpret_cast<ClearFn>(table[43]);
        InterlockedExchangePointer(&table[39],reinterpret_cast<void*>(volume::SetDepth));
        InterlockedExchangePointer(&table[40],reinterpret_cast<void*>(volume::GetDepth));
        InterlockedExchangePointer(&table[43],reinterpret_cast<void*>(HookedClear));
    }
    DWORD ignored = 0;
    VirtualProtect(&table[16], 95*sizeof(void*), oldProtect, &ignored);
    g_hookedTable = table;
    Log("D3D9 device hooked with fast state tracking; see INI for atmosphere/diagnostics switches.");
}

class Direct3D9Proxy final : public IDirect3D9
{
public:
    explicit Direct3D9Proxy(IDirect3D9* real) : _real(real) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9))
        {
            *out = static_cast<IDirect3D9*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG refs = --_refs; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* init) override { return _real->RegisterSoftwareDevice(init); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return _real->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* id) override { return _real->GetAdapterIdentifier(a, f, id); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT f) override { return _real->GetAdapterModeCount(a, f); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* d) override { return _real->EnumAdapterModes(a, f, m, d); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* d) override { return _real->GetAdapterDisplayMode(a, d); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT ad, D3DFORMAT bf, BOOL w) override { return _real->CheckDeviceType(a, t, ad, bf, w); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE r, D3DFORMAT c) override { return _real->CheckDeviceFormat(a, t, af, u, r, c); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m, DWORD* q) override { return _real->CheckDeviceMultiSampleType(a, t, f, w, m, q); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT ds) override { return _real->CheckDepthStencilMatch(a, t, af, rf, ds); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d) override { return _real->CheckDeviceFormatConversion(a, t, s, d); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9* c) override { return _real->GetDeviceCaps(a, t, c); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override { return _real->GetAdapterMonitor(a); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT a, D3DDEVTYPE t, HWND w, DWORD flags, D3DPRESENT_PARAMETERS* p, IDirect3DDevice9** device) override
    {
        HRESULT result = _real->CreateDevice(a, t, w, flags, p, device);
        if (SUCCEEDED(result) && device && *device)
            HookDevice(*device);
        return result;
    }
private:
    ~Direct3D9Proxy() { _real->Release(); }
    IDirect3D9* _real;
    std::atomic<ULONG> _refs{1};
};
void Initialize()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, std::size(path));
    std::wstring exe(path);
    g_basePath = exe.substr(0, exe.find_last_of(L"\\/") + 1);
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, std::size(systemDirectory)))
        return;
    g_systemD3D9 = LoadLibraryW((std::wstring(systemDirectory) + L"\\d3d9.dll").c_str());
    if (!g_systemD3D9)
        return;
    g_realCreate9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(g_systemD3D9, "Direct3DCreate9"));
    g_realCreate9Ex = reinterpret_cast<Direct3DCreate9ExFn>(GetProcAddress(g_systemD3D9, "Direct3DCreate9Ex"));
    waterdiag::Configure(g_basePath);
    celestialdiag::Configure(g_basePath);
    waterhighlight::Configure(g_basePath);
    celestialhighlight::Configure(g_basePath);
    celestialhighlight::ReloadTuning(g_basePath);
    ReloadWindTuning();
    renderer::FrameContext::Current().wind=g_windSystem.Update({},0.f);
    watereffect::Configure(g_basePath);
    distancefog::Configure(g_basePath);
    volume::Configure(g_basePath);
    weathervisuals::Configure(g_basePath);
    waterreflection::Configure(g_basePath);
    tuningoverlay::Configure(g_basePath);
    renderer::EnvironmentProfileManager::Instance().Configure(g_basePath);
    nativeshadowdiag::Configure(g_basePath);
    renderer::MaterialCacheManager::Instance().Configure(g_basePath);
    renderer::LocalLightManager::Instance().Configure(g_basePath);
    renderer::LocalLightingRenderer::Instance().Configure(g_basePath);
    renderer::GlobalIllumination::Instance().Configure(g_basePath);
    renderer::RendererDiagnostics::Instance().SetLogPath(g_basePath + L"ModernWoWRenderer.log");
    const std::wstring modernIni=g_basePath+L"ModernWoWRenderer.ini";
    unified=renderer::locationtuning::ReadCachedInt(modernIni.c_str(),L"Graphics",L"UnifiedToggle",0)!=0;
    if(unified){watereffect::hotkey=false;distancefog::hotkey=false;
        graphicsShowStatus=renderer::locationtuning::ReadCachedInt(modernIni.c_str(),L"Graphics",L"ShowStatus",0)!=0;
        graphicsActive=renderer::locationtuning::ReadCachedInt(modernIni.c_str(),L"Graphics",L"Enabled",1)!=0;
        watereffect::active=distancefog::active=volume::active=weathervisuals::active=graphicsActive;
    }
    Log("Modern WoW Renderer proxy loaded.");
    // Device vtable entries remain hooked for the process lifetime.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&Initialize), &pinned);
}
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    std::call_once(g_initialize, Initialize);
    if (!g_realCreate9) return nullptr;
    IDirect3D9* real = g_realCreate9(sdkVersion);
    return real ? new Direct3D9Proxy(real) : nullptr;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out)
{
    std::call_once(g_initialize, Initialize);
    Log("Direct3D9Ex requested: passthrough only, effects unavailable.");
    return g_realCreate9Ex ? g_realCreate9Ex(sdkVersion, out) : D3DERR_NOTAVAILABLE;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
