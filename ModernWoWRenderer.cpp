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
#include "src/Effects/LocalLightingRenderer.h"
#include "src/Effects/GroundSurfaceCapture.h"

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

struct Settings
{
    bool enabled = true;
    BYTE fogOpacity = 42;
    BYTE fogRed = 86, fogGreen = 118, fogBlue = 145;
    BYTE raysOpacity = 36;
    float fogHeight = 0.58f;
    float sunX = 0.72f, sunY = 0.18f;
};

struct Vertex { float x, y, z, rhw; D3DCOLOR color; };
constexpr DWORD VERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

HMODULE g_systemD3D9 = nullptr;
Direct3DCreate9Fn g_realCreate9 = nullptr;
Direct3DCreate9ExFn g_realCreate9Ex = nullptr;
EndSceneFn g_originalEndScene = nullptr;
std::mutex g_hookMutex;
void** g_hookedTable = nullptr;
std::once_flag g_initialize;
thread_local bool g_drawingOverlay = false;
Settings g_settings;
std::wstring g_basePath;

void Log(char const* text)
{
    std::wofstream out(g_basePath + L"ModernWoWRenderer.log", std::ios::app);
    out << text << '\n';
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

int ReadInt(wchar_t const* key, int fallback)
{
    return GetPrivateProfileIntW(L"Atmosphere", key, fallback, (g_basePath + L"ModernWoWRenderer.ini").c_str());
}

float ReadFloat(wchar_t const* key, float fallback)
{
    wchar_t buffer[64]{};
    GetPrivateProfileStringW(L"Atmosphere", key, L"", buffer, std::size(buffer), (g_basePath + L"ModernWoWRenderer.ini").c_str());
    return buffer[0] ? static_cast<float>(wcstod(buffer, nullptr)) : fallback;
}

void ReloadSettings()
{
    g_settings.enabled = ReadInt(L"Enabled", 1) != 0;
    g_settings.fogOpacity = static_cast<BYTE>(std::clamp(ReadInt(L"FogOpacity", 42), 0, 255));
    g_settings.fogRed = static_cast<BYTE>(std::clamp(ReadInt(L"FogRed", 86), 0, 255));
    g_settings.fogGreen = static_cast<BYTE>(std::clamp(ReadInt(L"FogGreen", 118), 0, 255));
    g_settings.fogBlue = static_cast<BYTE>(std::clamp(ReadInt(L"FogBlue", 145), 0, 255));
    g_settings.raysOpacity = static_cast<BYTE>(std::clamp(ReadInt(L"RaysOpacity", 36), 0, 255));
    g_settings.fogHeight = std::clamp(ReadFloat(L"FogHeight", .58f), .0f, 1.0f);
    g_settings.sunX = std::clamp(ReadFloat(L"SunX", .72f), .0f, 1.0f);
    g_settings.sunY = std::clamp(ReadFloat(L"SunY", .18f), .0f, 1.0f);
}

void DrawQuad(IDirect3DDevice9* device, float left, float top, float right, float bottom, D3DCOLOR topColor, D3DCOLOR bottomColor)
{
    Vertex vertices[] = {{left, top, 0.f, 1.f, topColor}, {right, top, 0.f, 1.f, topColor}, {left, bottom, 0.f, 1.f, bottomColor}, {right, bottom, 0.f, 1.f, bottomColor}};
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
}

void DrawAtmosphere(IDirect3DDevice9*)
{
    // Obsolete 2D screen quad completely disabled: volume rendering provides true 3D atmosphere.
}

void RestoreDeviceHooksAfterReset(IDirect3DDevice9* device);

HRESULT WINAPI HookedEndScene(IDirect3DDevice9* device)
{
    // Overlays cannot draw on a lost device; forward the call untouched.
    if (g_availability == renderer::DeviceAvailability::Lost) return g_originalEndScene(device);
    DrawAtmosphere(device);
    g_drawingOverlay=true;
    if(unified){if(graphicsShowStatus){watereffect::DrawStatus(device,graphicsActive?"FX ON":"FX OFF",0);
        if(graphicsActive&&volume::enabled)watereffect::DrawStatus(device,volume::composed?"AIR ON":"AIR WAIT",1);}
    }
    else {watereffect::DrawStatus(device);
    if(distancefog::enabled&&distancefog::showStatus)watereffect::DrawStatus(device,!distancefog::active?"HAZE OFF":distancefog::matches?"HAZE ON":"HAZE WAIT",1);}
    if(weathervisuals::enabled&&weathervisuals::showStatus)watereffect::DrawStatus(device,weathervisuals::GetStatusText(),3);
    nativeshadowdiag::DrawPreview(device);
    tuningoverlay::Draw(device);
    renderer::MaterialCacheManager::Instance().DrawDebugOverlay(device);
    g_drawingOverlay=false;
    return g_originalEndScene(device);
}

HRESULT WINAPI HookedPresent(IDirect3DDevice9* d,const RECT* a,const RECT* b,HWND w,const RGNDATA* r) {
    const uint64_t now = GetTickCount64();
    // Every Present is a frame boundary. A long pause means the frame stream
    // was interrupted (Alt+Tab, minimised window, loading screen), so temporal
    // history from before the pause must not be blended into the next frame.
    if (g_frameGap.Observe(now, kFrameGapMs)) volume::InvalidateTemporalHistory();

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
    {
        const bool knownWorldDraw = weathervisuals::worldGeometrySeenThisFrame || watereffect::frameCandidates>0;
        const bool worldFrame = renderer::HasWorldContext(
            renderer::DepthCapture::Instance().HasDepth(), knownWorldDraw);
        renderer::EnvironmentProfileManager::Instance().Update(now, worldFrame,
            renderer::ShouldHoldLocation(worldFrame));
    }
    renderer::FrameContext::Current().environment=renderer::EnvironmentProfileManager::Instance().State();
    waterreflection::Present();
    // Consume the held native weather observation, independently of whether
    // precipitation replacement is enabled or its textures loaded. No draw
    // count is treated as a researched server-side storm intensity.
    volume::cloudWeather=static_cast<renderer::CloudWeather>(std::clamp(weathervisuals::nativeWeatherMode,0,3));
    volume::cloudWeatherStrength=weathervisuals::intensity;
    // Water reacts to the same observation, in the same frame, so a squall closes the sky and
    // roughens the sea together instead of only half of them agreeing that it is raining.
    watereffect::FeedWeather(std::clamp(weathervisuals::nativeWeatherMode,0,3),weathervisuals::intensity);
    volume::Present(d);
    renderer::MaterialCacheManager::Instance().Present(d);
    weathervisuals::Present(d,tuningoverlay::visible);
    const auto& environmentManager=renderer::EnvironmentProfileManager::Instance();
    const bool locationTuningChanged=renderer::locationtuning::Sync(g_basePath,environmentManager.Location(),environmentManager.LocationTuningVerified());
    if(locationTuningChanged)tuningoverlay::Configure(g_basePath);
    if(tuningoverlay::Update()||locationTuningChanged){watereffect::ReloadTuning();distancefog::ReloadTuning();volume::ReloadTuning();celestialhighlight::ReloadTuning(g_basePath);nativeshadowdiag::ReloadEnhancement();renderer::MaterialCacheManager::Instance().ReloadTuning(g_basePath);renderer::LocalLightManager::Instance().ReloadTuning();weathervisuals::ReloadTuning();Log("Location tuning reloaded");}
    if(unified){
        bool down=(GetAsyncKeyState(VK_F11)&0x8000)!=0;DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
        if(down&&!graphicsKeyDown&&pid==GetCurrentProcessId()){
            graphicsActive=!graphicsActive;watereffect::active=distancefog::active=volume::active=weathervisuals::active=graphicsActive;
            Log(graphicsActive?"F11 graphics ON":"F11 graphics OFF");
        }
        graphicsKeyDown=down;
    }
    {
        bool down=(GetAsyncKeyState(VK_F12)&0x8000)!=0;DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
        if(down&&!tuningKeyDown&&pid==GetCurrentProcessId()){watereffect::ReloadTuning();distancefog::ReloadTuning();volume::ReloadTuning();celestialhighlight::ReloadTuning(g_basePath);nativeshadowdiag::ReloadEnhancement();renderer::MaterialCacheManager::Instance().ReloadTuning(g_basePath);renderer::LocalLightManager::Instance().ReloadTuning();weathervisuals::ReloadTuning();renderer::EnvironmentProfileManager::Instance().Reload();tuningoverlay::Configure(g_basePath);waterdiag::Configure(g_basePath);Log("F12 reloaded GraphicsEffects.ini, EnvironmentProfiles.ini and water diagnostic settings");}
        tuningKeyDown=down;
    }
    waterhighlight::Present();
    watereffect::Present();
    distancefog::Present();
    waterdiag::Present(d);
    celestialdiag::Present(d);
    nativeshadowdiag::OnPresent(d);
    renderer::RendererDiagnostics::Instance().OnFrameEnd();
    const HRESULT presentResult = originalPresent(d,a,b,w,r);
    SetAvailability(renderer::ClassifyDeviceResult(presentResult));
    return presentResult;
}
HRESULT WINAPI HookedReset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* p) {
    tuningoverlay::font.Reset();
    waterreflection::Reset(d);
    volume::Reset(d);
    watereffect::Reset(d);
    waterdiag::Reset(d);
    celestialdiag::Reset(d);
    nativeshadowdiag::Reset();
    renderer::MaterialCacheManager::Instance().Reset(d);
    renderer::LocalLightingRenderer::Instance().Reset(d);
    renderer::LocalLightManager::Instance().Reset();
    weathervisuals::Reset(d);
    renderer::ShaderCache::Instance().Clear();
    renderer::DrawCallClassifier::Instance().ClearCache();
    renderer::DepthCapture::Instance().Reset(d);
    renderer::CameraCapture::Instance().Reset();
    HRESULT hr=originalReset(d,p);
    if(SUCCEEDED(hr)){
        renderer::g_trackedState={};
        RestoreDeviceHooksAfterReset(d);
        SetAvailability(renderer::DeviceAvailability::Operational);
    }
    else {
        // Stay paused; Present keeps probing and resumes once the device is usable.
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
    volume::BeforeClear(d,n,rect,flags,z);return originalClear(d,n,rect,flags,color,z,stencil);
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
        nativeshadowdiag::OnSetRenderTarget(index, surface);
    return hr;
}

HRESULT WINAPI HookedSetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* texture)
{
    const HRESULT hr = originalSetTexture(d, stage, texture);
    if (SUCCEEDED(hr) && !volume::internal && !weathervisuals::internal && !g_drawingOverlay &&
        !renderer::MaterialCacheManager::Instance().IsInternalPass())
    {
        nativeshadowdiag::OnSetTexture(stage, texture);
        renderer::MaterialCacheManager::Instance().OnSetTexture(stage, texture);
    }
    return hr;
}

HRESULT WINAPI HookedSetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* vs)
{
    if (renderer::g_trackedState.currentVS != vs || (vs && renderer::g_trackedState.vsHash == 0))
    {
        renderer::g_trackedState.currentVS = vs;
        renderer::g_trackedState.vsHash = (vs && (!unified || graphicsActive)) ? renderer::ShaderCache::Instance().GetShaderHash(vs) : 0;
    }
    return originalSetVertexShader(d, vs);
}

HRESULT WINAPI HookedSetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* ps)
{
    if (!weathervisuals::internal &&
        (renderer::g_trackedState.currentPS != ps || (ps && renderer::g_trackedState.psHash == 0)))
    {
        renderer::g_trackedState.currentPS = ps;
        renderer::g_trackedState.psHash = (ps && (!unified || graphicsActive)) ? renderer::ShaderCache::Instance().GetShaderHash(ps) : 0;
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

renderer::DrawClassification ClassifyCurrentDraw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n)
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
        d, ctx, volume::capturedViewTranslation, volume::capturedViewValid, volume::cameraCaptureShaderHash);
    renderer::RendererDiagnostics::Instance().RecordDrawCall(dc);
    return dc;
}

HRESULT WINAPI HookedDraw(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT start,UINT n) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDraw(d,t,start,n);
    if(volume::internal||weathervisuals::internal)return originalDraw(d,t,start,n);
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawPrimitive",t,n);
    if(unified&&!graphicsActive)return originalDraw(d,t,start,n);
    if(!g_drawingOverlay){weathervisuals::ObserveWorldDraw();nativeshadowdiag::OnDraw(d,"DrawPrimitive",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);}
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    if(!g_drawingOverlay&&volume::ShouldUpdateShadows()){
        auto dc=ClassifyCurrentDraw(d,t,n);
        volume::ShadowDraw(d,dc,[&]{return originalDraw(d,t,start,n);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawPrimitive",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,0,start,n,false);
    if(n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{return originalDraw(d,t,start,n);});volume::internal=false;}
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDraw(d,t,start,n);
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
    return hr;
}

HRESULT WINAPI HookedDrawIndexed(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,INT base,UINT min,UINT vertices,UINT start,UINT n) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDrawIndexed(d,t,base,min,vertices,start,n);
    if(volume::internal||weathervisuals::internal)return originalDrawIndexed(d,t,base,min,vertices,start,n);
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawIndexedPrimitive",t,n);
    if(unified&&!graphicsActive)return originalDrawIndexed(d,t,base,min,vertices,start,n);
    if(!g_drawingOverlay){weathervisuals::ObserveWorldDraw();nativeshadowdiag::OnDraw(d,"DrawIndexedPrimitive",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);}
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    if(!g_drawingOverlay&&volume::ShouldUpdateShadows()){
        auto dc=ClassifyCurrentDraw(d,t,n);
        volume::ShadowDraw(d,dc,[&]{return originalDrawIndexed(d,t,base,min,vertices,start,n);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawIndexedPrimitive",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,base,start,n);
    if(n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{return originalDrawIndexed(d,t,base,min,vertices,start,n);});volume::internal=false;}
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDrawIndexed(d,t,base,min,vertices,start,n);
    if(SUCCEEDED(hr)&&!g_drawingOverlay&&renderer::MaterialCacheManager::Instance().IsActive()){
        auto dc=ClassifyCurrentDraw(d,t,n);
        if(dc.material==renderer::MaterialType::Terrain){
            renderer::MaterialCacheManager::Instance().ApplyNormalModulation(d,t,base,min,vertices,start,n,[&]{return originalDrawIndexed(d,t,base,min,vertices,start,n);});
        }
        else if(dc.material==renderer::MaterialType::WMO||dc.material==renderer::MaterialType::M2){
            renderer::MaterialCacheManager::Instance().ApplyObjectMaterial(d,n,[&]{return originalDrawIndexed(d,t,base,min,vertices,start,n);});
        }
    }
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
    return hr;
}

HRESULT WINAPI HookedDrawUP(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT n,const void* v,UINT stride) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDrawUP(d,t,n,v,stride);
    if(volume::internal||weathervisuals::internal)return originalDrawUP(d,t,n,v,stride);
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawPrimitiveUP",t,n);
    if(unified&&!graphicsActive)return originalDrawUP(d,t,n,v,stride);
    if(!g_drawingOverlay)nativeshadowdiag::OnDraw(d,"DrawPrimitiveUP",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    if(!g_drawingOverlay&&volume::ShouldUpdateShadows()){
        auto dc=ClassifyCurrentDraw(d,t,n);
        volume::ShadowDraw(d,dc,[&]{return originalDrawUP(d,t,n,v,stride);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawPrimitiveUP",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,0,0,n,false,v,stride);
    if(n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{return originalDrawUP(d,t,n,v,stride);});volume::internal=false;}
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::ObserveWorldDraw();
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDrawUP(d,t,n,v,stride);
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
    return hr;
}

HRESULT WINAPI HookedDrawIndexedUP(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT min,UINT vertices,UINT n,const void* indices,D3DFORMAT f,const void* v,UINT stride) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    if(volume::internal||weathervisuals::internal)return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawIndexedPrimitiveUP",t,n);
    if(unified&&!graphicsActive)return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    if(!g_drawingOverlay)nativeshadowdiag::OnDraw(d,"DrawIndexedPrimitiveUP",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    if(!g_drawingOverlay&&volume::ShouldUpdateShadows()){
        auto dc=ClassifyCurrentDraw(d,t,n);
        volume::ShadowDraw(d,dc,[&]{return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawIndexedPrimitiveUP",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,0,0,n,true,v,stride,indices,f,min+vertices);
    if(n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);});volume::internal=false;}
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::ObserveWorldDraw();
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
    return hr;
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
    if(waterdiag::enabled||waterhighlight::enabled||watereffect::enabled||distancefog::enabled||volume::enabled||nativeshadowdiag::enabled||unified){
        InterlockedExchangePointer(&table[16],reinterpret_cast<void*>(HookedReset));
        InterlockedExchangePointer(&table[17],reinterpret_cast<void*>(HookedPresent));
        InterlockedExchangePointer(&table[81],reinterpret_cast<void*>(HookedDraw));
        InterlockedExchangePointer(&table[82],reinterpret_cast<void*>(HookedDrawIndexed));
        InterlockedExchangePointer(&table[83],reinterpret_cast<void*>(HookedDrawUP));
        InterlockedExchangePointer(&table[84],reinterpret_cast<void*>(HookedDrawIndexedUP));
    }
    if(volume::enabled||watereffect::enabled){
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
        if (g_hookedTable != table)
            Log("Different device implementation: leaving this device untouched.");
        return;
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

    if(waterdiag::enabled || waterhighlight::enabled || watereffect::enabled || distancefog::enabled || volume::enabled || nativeshadowdiag::enabled || unified) {
        InterlockedExchangePointer(&table[16],reinterpret_cast<void*>(HookedReset));
        InterlockedExchangePointer(&table[17],reinterpret_cast<void*>(HookedPresent));
        InterlockedExchangePointer(&table[81],reinterpret_cast<void*>(HookedDraw));
        InterlockedExchangePointer(&table[82],reinterpret_cast<void*>(HookedDrawIndexed));
        InterlockedExchangePointer(&table[83],reinterpret_cast<void*>(HookedDrawUP));
        InterlockedExchangePointer(&table[84],reinterpret_cast<void*>(HookedDrawIndexedUP));
    }
    if(volume::enabled||watereffect::enabled){
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
    ReloadSettings();
    waterdiag::Configure(g_basePath);
    celestialdiag::Configure(g_basePath);
    waterhighlight::Configure(g_basePath);
    celestialhighlight::Configure(g_basePath);
    celestialhighlight::ReloadTuning(g_basePath);
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
    renderer::RendererDiagnostics::Instance().SetLogPath(g_basePath + L"ModernWoWRenderer.log");
    unified=GetPrivateProfileIntW(L"Graphics",L"UnifiedToggle",0,(g_basePath+L"ModernWoWRenderer.ini").c_str())!=0;
    if(unified){watereffect::hotkey=false;distancefog::hotkey=false;g_settings.enabled=false;
        graphicsShowStatus=GetPrivateProfileIntW(L"Graphics",L"ShowStatus",0,(g_basePath+L"ModernWoWRenderer.ini").c_str())!=0;
        graphicsActive=GetPrivateProfileIntW(L"Graphics",L"Enabled",1,(g_basePath+L"ModernWoWRenderer.ini").c_str())!=0;
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
