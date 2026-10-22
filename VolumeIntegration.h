#include "src/Effects/ImagePostProcess.h"
#include "src/Environment/LocationTuning.h"
#pragma once
#include "NativeShadowDiagnostics.h"
#include "VolumeEffects.h"
#include <cstdio>
#include <unordered_set>
#include <unordered_map>
#include "src/Core/FrameClock.h"
#include "src/Core/ShaderCache.h"
#include "src/Diagnostics/RendererDiagnostics.h"
#include "src/Diagnostics/PerformanceProfiler.h"
#include "src/D3D9/DepthCapture.h"
#include "src/D3D9/CameraCapture.h"
#include "src/D3D9/TrackedRenderState.h"
#include "src/D3D9/ScopedPassState.h"
#include "src/D3D9/ShaderBytecodeCache.h"
#include "src/Scene/DrawCallClassifier.h"
#include "src/Effects/DirectionalVolumetricLighting.h"
#include "src/Effects/VolumetricClouds.h"
#include "src/Effects/AmbientOcclusion.h"
#include "src/Effects/Underwater.h"
#include "src/Effects/GroundSurfaceCapture.h"
#include "src/Effects/LocalLightingCore.h"
#include "src/Effects/LocalLightingRenderer.h"
#include "src/Lighting/LocalLightManager.h"
#include "src/Diagnostics/CelestialMemoryProbe.h"
#include "src/D3D9/CelestialTracker.h"
#include "src/D3D9/CelestialStability.h"
#include "src/D3D9/CameraContinuity.h"
#include "src/Environment/EnvironmentProfileManager.h"

namespace volume {
using Microsoft::WRL::ComPtr;
using SetDepthFn=HRESULT(WINAPI*)(IDirect3DDevice9*,IDirect3DSurface9*);
using GetDepthFn=HRESULT(WINAPI*)(IDirect3DDevice9*,IDirect3DSurface9**);
SetDepthFn setDepth=nullptr; GetDepthFn getDepth=nullptr;
bool enabled=false,active=true,ready=false,composed=false,internal=false;
bool cloudsEffectEnabled=true;
// Held native observation; the cloud controller smooths weather transitions.
renderer::CloudWeather cloudWeather=renderer::CloudWeather::Clear;
float cloudWeatherStrength=1.f;
bool fogEffectEnabled=true,shaftsEffectEnabled=true,shadowsEffectEnabled=true,shadowMapEnabled=false,temporalShaftsEnabled=false;
bool postProcessEffectEnabled=true;
// Anti-aliasing has its own switch instead of riding the grade master above, because the two answer
// different questions and a player needs to be able to answer them separately: the master is "do I
// want my colours graded" (off for maximum FPS), this one is "do I want my edges smoothed". It is
// the replacement for hardware multisampling, which this renderer cannot use at all above 1x - a
// multisampled depth buffer cannot be sampled by a pixel shader in D3D9, and every depth-reading
// effect here loses its depth when the client is set higher. Making AA a passenger on the grade
// master would mean the fix for that is invisible to anyone who has never touched PostProcess.
bool antiAliasingEnabled=true;
bool sunGlareEnabled=true;
float sunGlareStrength=0.25f;
UINT shadowMapUpdateInterval=2;
bool volumetricCharacterShadows=false;
std::wstring savedBasePath;
IDirect3DDevice9* owner=nullptr;

struct LegacyVolumeShaders
{
    IDirect3DDevice9* owner = nullptr;
    ComPtr<IDirect3DPixelShader9> volume;
    ComPtr<IDirect3DPixelShader9> blur;
    ComPtr<IDirect3DPixelShader9> copy;
    ComPtr<IDirect3DPixelShader9> temporal;
    ComPtr<IDirect3DPixelShader9> radial;
    ComPtr<IDirect3DPixelShader9> postProcess;
    ComPtr<IDirect3DPixelShader9> rayComposite;
    ComPtr<IDirect3DPixelShader9> celestialMarker;

    bool Ensure(IDirect3DDevice9* d);
    void Reset();
};

struct LegacyFrameTargets
{
    IDirect3DDevice9* owner = nullptr;
    UINT width = 0;
    UINT height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    UINT rayWidth = 0;
    UINT rayHeight = 0;

    ComPtr<IDirect3DTexture9> scene;
    ComPtr<IDirect3DSurface9> sceneSurface;

    ComPtr<IDirect3DTexture9> rayA;
    ComPtr<IDirect3DSurface9> rayASurface;

    ComPtr<IDirect3DTexture9> rayB;
    ComPtr<IDirect3DSurface9> rayBSurface;

    ComPtr<IDirect3DTexture9> rayHistory;
    ComPtr<IDirect3DSurface9> rayHistorySurface;
    bool historyValid = false;

    bool Ensure(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT fmt, UINT rW, UINT rH);
    void Reset();
};

LegacyVolumeShaders legacyShaders;
LegacyFrameTargets legacyTargets;

struct FrameResources {
    ComPtr<IDirect3DTexture9> depth, noise, shadowDepth, shadowColor, waterMask, waterDepth;
    ComPtr<IDirect3DSurface9> surface, originalDepth, target, shadowDepthSurface, shadowColorSurface, waterMaskSurface, waterDepthSurface;
    IDirect3DDevice9* noiseOwner = nullptr;
    IDirect3DDevice9* shadowOwner = nullptr;
    UINT shadowSize = 0;
    IDirect3DDevice9* waterMaskOwner = nullptr;
    UINT waterMaskWidth = 0, waterMaskHeight = 0;
};
FrameResources& resources = *new FrameResources;
auto& depth = resources.depth; auto& surface = resources.surface;
auto& originalDepth = resources.originalDepth; auto& target = resources.target;

ComPtr<ID3DBlob> bytecode, blurBytecode, copyBytecode, temporalBytecode, radialBytecode, postProcessBytecode, rayCompositeBytecode, celestialMarkerBytecode;
float constants[14][4]{};
uint64_t cameraCaptureShaderHash = 0;
float capturedViewTranslation[3]{};
bool capturedViewValid = false;
float baseHeight = 60, density = .004f, falloff = .07f, strength = 2.6f, moonStrength = .35f, variation = 1.35f, lowLayer = .35f, raySoftness = 5.f, rayFalloff = 2.f, fogWash = .45f, sunVerticalScale = .4f, sunOffsetX = 0, sunOffsetY = 0, sunSourceThreshold = .60f;
// Second, independent height-fog layer: low, dense ground mist hugging the
// terrain, on top of the broader distance haze (`density`/`falloff`/
// `baseHeight` above). Combined by summing optical depth - physically the
// right way to stack two independent scattering media along the same ray.
float groundMistHeight = 4.f, groundMistFalloff = .35f, groundMistDensity = .008f;
float directionalShadowStrength = .16f, shadowMapDistance = 140.f, shadowMapBias = .0008f, shadowSoftness = 1.6f, cloudShadowStrength = .06f;
UINT configuredShadowMapSize = 1024;
int brightnessPercent = 0, contrastPercent = 100, gammaPercent = 100, sharpnessPercent = 35;
int sunGlowPercent = 150;
int moonGlowPercent = 80;
float shadowMatrix[4][4]{}, viewToShadowMatrix[4][4]{}, shadowView[4][4]{}, shadowProjection[4][4]{};
bool shadowFrameStarted = false, shadowFrameValid = false;
bool stableShadowLightValid = false;
float stableShadowLight[3]{};
bool shadowCacheValid = false, shadowAnchorValid = false;
float shadowAnchor[3]{};
float celestialDaylight = 0, celestialMoonlight = 0;
float celestialShadowLight = 0;
// Stable direct-light estimate built in Composite (see
// src/D3D9/CelestialStability.h). Persistent across frames on purpose:
// its whole job is to remember the light through frames where the disc
// draw is culled, missed or covered, so clouds/fog do not blink with it.
renderer::CelestialLightStabilizer celestialLightStabilizer;
renderer::CelestialDirectionStabilizer celestialDirectionStabilizer;
// Performance-counter clock for the celestial stabilizers below: their
// fades are integrated from this delta, and the ~15.6 ms steps of
// GetTickCount64 made the sky's light pulse instead of gliding.
renderer::FrameClock celestialLightClock;
float smoothSunX = -1, smoothSunY = -1;
bool shaftDebug = false;
bool celestialMarkerDebug = false;
bool celestialMemoryProbeDebug = false;
std::wstring celestialProbeLogPath;
unsigned celestialProbeLogCounter = 0;
unsigned frames = 0, applied = 0, depthFrames = 0, cameraFrames = 0, shadowDraws = 0, shadowFrameDraws = 0;
std::wstring logPath;
std::wstring mainIni, tuningIni;

renderer::WorldSceneObservation ResolveWorldSceneForFrame() {
    auto& frame=renderer::FrameContext::Current();
    const bool knownWorldDraw=frame.worldTerrainSeenThisFrame||frame.worldWaterSeenThisFrame;
    // An early depth-clear probe catches a verified login/menu transition before
    // the first UI composite. If the provider is unverified, leave Unknown open
    // to be upgraded once a terrain/liquid draw supplies independent evidence.
    if(!frame.worldSceneResolved||
       (frame.worldScene.state==renderer::WorldSceneState::Unknown&&knownWorldDraw)) {
        const bool renderContext=renderer::HasWorldContext(
            renderer::DepthCapture::Instance().HasDepth(),knownWorldDraw);
        frame.worldScene=renderer::EnvironmentProfileManager::Instance().ObserveWorldScene(
            renderContext,knownWorldDraw);
        frame.worldSceneResolved=true;
    }
    watereffect::ObserveWorldSceneForUnderwater(frame);
    if(frame.worldScene.state!=renderer::WorldSceneState::InWorld)
        renderer::DirectionalVolumetricLighting::Instance().InvalidateHistory();
    return frame.worldScene;
}

// This file is NOT on the shared sink, even though it is written on a crossing:
// PerformanceProfiler is handed the same path below and appends to it directly
// every frame, and the sink may only buffer a file it is the sole writer of -
// otherwise a direct append can land before an older buffered line and the log
// reads out of order. See the ownership rule in src/Diagnostics/FileSink.h.
void Log(const char* s) { std::ofstream(std::filesystem::path(logPath), std::ios::app) << s << '\n'; }
int ReadTuning(const wchar_t* key, int fallback, const wchar_t* section = L"Atmosphere") {
    wchar_t value[64]{};
    renderer::locationtuning::ReadString(section, key, L"", value, std::size(value), tuningIni.c_str());
    return value[0] ? int(wcstol(value, nullptr, 10)) : renderer::locationtuning::ReadInt(section, key, fallback, mainIni.c_str());
}

bool LegacyVolumeShaders::Ensure(IDirect3DDevice9* d)
{
    if (owner == d && volume && blur && copy && temporal && radial)
        return true;

    Reset();
    owner = d;
    if (!d) return false;

    // Each shader reads the on-disk bytecode cache first (renderer::
    // shaderbytecode::GetShader) and only compiles - and stores the result -
    // on a miss, so a repeat launch skips the eight active shader passes.
    // GetShader also recompiles once and retries if the driver refuses a cached blob.
    const std::wstring cacheDir = renderer::shaderbytecode::Dir(savedBasePath);
    auto createPS = [&](ID3DBlob* b, ComPtr<IDirect3DPixelShader9>& out)
    { return SUCCEEDED(d->CreatePixelShader(static_cast<DWORD*>(b->GetBufferPointer()), out.GetAddressOf())); };

    if (!renderer::shaderbytecode::GetShader(cacheDir, volumePixelSource, "main", "ps_3_0", bytecode, [&](ID3DBlob* b) { return createPS(b, volume); }))
        return false;
    if (!renderer::shaderbytecode::GetShader(cacheDir, volumeBlurPixelSource, "main", "ps_3_0", blurBytecode, [&](ID3DBlob* b) { return createPS(b, blur); }))
        return false;
    if (!renderer::shaderbytecode::GetShader(cacheDir, volumeCopyPixelSource, "main", "ps_3_0", copyBytecode, [&](ID3DBlob* b) { return createPS(b, copy); }))
        return false;
    if (!renderer::shaderbytecode::GetShader(cacheDir, volumeTemporalPixelSource, "main", "ps_3_0", temporalBytecode, [&](ID3DBlob* b) { return createPS(b, temporal); }))
        return false;
    if (!renderer::shaderbytecode::GetShader(cacheDir, solarRadialPixelSource, "main", "ps_3_0", radialBytecode, [&](ID3DBlob* b) { return createPS(b, radial); }))
        return false;

    // These three are optional: a compile/create failure only disables their
    // feature, it must not abort the rest of the pass.
    renderer::shaderbytecode::GetShader(cacheDir, postProcessPixelSource, "main", "ps_3_0", postProcessBytecode, [&](ID3DBlob* b) { return createPS(b, postProcess); });
    renderer::shaderbytecode::GetShader(cacheDir, rayCompositePixelSource, "main", "ps_3_0", rayCompositeBytecode, [&](ID3DBlob* b) { return createPS(b, rayComposite); });
    renderer::shaderbytecode::GetShader(cacheDir, celestialMarkerPixelSource, "main", "ps_3_0", celestialMarkerBytecode, [&](ID3DBlob* b) { return createPS(b, celestialMarker); });

    return true;
}

void LegacyVolumeShaders::Reset()
{
    volume.Reset();
    blur.Reset();
    copy.Reset();
    temporal.Reset();
    radial.Reset();
    postProcess.Reset();
    rayComposite.Reset();
    celestialMarker.Reset();
    owner = nullptr;
}

bool LegacyFrameTargets::Ensure(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT fmt, UINT rW, UINT rH)
{
    if (owner == d && width == w && height == h && format == fmt &&
        rayWidth == rW && rayHeight == rH &&
        scene && rayA && rayB)
    {
        return true;
    }

    Reset();
    owner = d;
    width = w;
    height = h;
    format = fmt;
    rayWidth = rW;
    rayHeight = rH;

    if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, scene.GetAddressOf(), nullptr)) ||
        FAILED(scene->GetSurfaceLevel(0, sceneSurface.GetAddressOf())))
        return false;

    if (FAILED(d->CreateTexture(rW, rH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, rayA.GetAddressOf(), nullptr)) ||
        FAILED(rayA->GetSurfaceLevel(0, rayASurface.GetAddressOf())) ||
        FAILED(d->CreateTexture(rW, rH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, rayB.GetAddressOf(), nullptr)) ||
        FAILED(rayB->GetSurfaceLevel(0, rayBSurface.GetAddressOf())))
        return false;

    if (FAILED(d->CreateTexture(rW, rH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, rayHistory.GetAddressOf(), nullptr)) ||
        FAILED(rayHistory->GetSurfaceLevel(0, rayHistorySurface.GetAddressOf())))
        return false;

    historyValid = false;
    return true;
}

void LegacyFrameTargets::Reset()
{
    sceneSurface.Reset();
    scene.Reset();
    rayASurface.Reset();
    rayA.Reset();
    rayBSurface.Reset();
    rayB.Reset();
    rayHistorySurface.Reset();
    rayHistory.Reset();
    historyValid = false;
    owner = nullptr;
    width = height = rayWidth = rayHeight = 0;
    format = D3DFMT_UNKNOWN;
}

void ReloadTuning() {
    fogEffectEnabled = ReadTuning(L"FogEnabled", 1) != 0;
    shaftsEffectEnabled = ReadTuning(L"ShaftsEnabled", 1) != 0;
    shadowsEffectEnabled = ReadTuning(L"ShadowsEnabled", 1) != 0;
    shadowMapEnabled = ReadTuning(L"ShadowMapEnabled", 0) != 0;
    shadowMapUpdateInterval = UINT(std::clamp(ReadTuning(L"ShadowMapUpdateInterval", 2), 1, 10));
    volumetricCharacterShadows = ReadTuning(L"VolumetricCharacterShadows", 0) != 0;
    temporalShaftsEnabled = ReadTuning(L"TemporalShaftsEnabled", 0) != 0;
    baseHeight = float(ReadTuning(L"BaseHeight", 60));
    density = std::clamp(ReadTuning(L"DensityPermille", 4), 0, 20) * .001f;
    strength = std::clamp(ReadTuning(L"ShaftPercent", 160), 0, 800) * .01f;
    variation = std::clamp(ReadTuning(L"VariationPercent", 135), 0, 250) * .01f;
    moonStrength = std::clamp(ReadTuning(L"MoonShaftPercent", 60), 0, 800) * .01f;
    lowLayer = std::clamp(ReadTuning(L"LowLayerPercent", 35), 0, 150) * .01f;
    // Ground mist: low, dense, tightly height-limited - a second fog layer
    // stacked on top of the broad distance haze above, not a replacement.
    groundMistHeight = float(std::clamp(ReadTuning(L"GroundMistHeight", 4), -20, 60));
    groundMistFalloff = std::clamp(ReadTuning(L"GroundMistFalloffPermille", 350), 20, 2000) * .001f;
    groundMistDensity = std::clamp(ReadTuning(L"GroundMistDensityPermille", 8), 0, 40) * .001f;
    raySoftness = float(std::clamp(ReadTuning(L"ShaftSoftnessPixels", 5), 0, 16));
    rayFalloff = std::clamp(ReadTuning(L"ShaftFalloffPercent", 200), 50, 500) * .01f;
    fogWash = std::clamp(ReadTuning(L"FogWashPercent", 35), 0, 100) * .01f;
    sunVerticalScale = std::clamp(ReadTuning(L"SunVerticalProjectionPercent", 100), 10, 100) * .01f;
    sunOffsetX = std::clamp(ReadTuning(L"SunOffsetXPercent", 0), -50, 50) * .01f;
    sunOffsetY = std::clamp(ReadTuning(L"SunOffsetYPercent", 0), -50, 50) * .01f;
    sunSourceThreshold = std::clamp(ReadTuning(L"SunSourceThresholdPercent", 60), 10, 95) * .01f;
    // Genuinely local: a "contact" shadow grounds objects and shades creases.
    // Long-range occlusion belongs to the light-space directional shadow map,
    // never to this screen-space depth raymarch (off-screen occluders don't
    // exist here, which is what produced ghosting/disocclusion at long range).
    directionalShadowStrength = std::clamp(ReadTuning(L"DirectionalShadowPercent", 45), 0, 100) * .01f;
    shadowMapDistance = float(std::clamp(ReadTuning(L"ShadowMapDistance", ReadTuning(L"ShadowReach", 180)), 50, 300));
    configuredShadowMapSize = UINT(std::clamp(ReadTuning(L"ShadowMapSize", 1024), 256, 4096));
    brightnessPercent = std::clamp(ReadTuning(L"BrightnessPercent", 0, L"PostProcess"), -50, 50);
    contrastPercent = std::clamp(ReadTuning(L"ContrastPercent", 100, L"PostProcess"), 50, 180);
    gammaPercent = std::clamp(ReadTuning(L"GammaPercent", 100, L"PostProcess"), 50, 180);
    sharpnessPercent = std::clamp(ReadTuning(L"SharpnessPercent", 35, L"PostProcess"), 0, 100);
    postProcessEffectEnabled = ReadTuning(L"Enabled", 1, L"PostProcess") != 0;
    antiAliasingEnabled = ReadTuning(L"AntiAliasing", 1, L"PostProcess") != 0;
    // One line per CHANGE, not one per reload: this runs on every settled crossing
    // as well as at startup, and a line here is an open, a write and a close of a
    // file in the game folder - which a scanner watching that folder turns into
    // milliseconds, on exactly the path a player walks a border on. The line carries the
    // anti-aliasing switch as well as the grade, so a log answers "is AA on" without a second
    // lookup - it is the setting that decides whether the client should be at 1x multisampling.
    {
        char message[192];
        sprintf_s(message, sizeof(message), "PostProcess effective enabled=%d brightness=%d contrast=%d gamma=%d sharpness=%d anti-aliasing=%d",
            postProcessEffectEnabled,brightnessPercent,contrastPercent,gammaPercent,sharpnessPercent,antiAliasingEnabled);
        static std::string lastPostProcessLine;
        if(!logPath.empty()&&lastPostProcessLine!=message){lastPostProcessLine=message;Log(message);}
    }
    sunGlowPercent = std::clamp(ReadTuning(L"SunGlowPercent", 80), 0, 400);
    moonGlowPercent = std::clamp(ReadTuning(L"MoonGlowPercent", 80), 0, 400);
    sunGlareEnabled = ReadTuning(L"SunGlareEnabled", 1) != 0;
    sunGlareStrength = float(ReadTuning(L"SunGlareStrengthPercent", 15)) * 0.01f;
    shaftDebug = ReadTuning(L"ShaftDebugMask", 0) != 0;
    celestialMarkerDebug = ReadTuning(L"DebugCelestialMarker", 0) != 0;
    // Read-only comparison of the current v[24]-projection marker (RED)
    // against two candidate directions read from client-process globals
    // (GREEN=sun candidate, CYAN=moon candidate) per CelestialMemoryProbe.h.
    // Off by default; never affects rendering/rays, debug overlay only.
    celestialMemoryProbeDebug = ReadTuning(L"DebugCelestialMemoryProbe", 0) != 0;

    renderer::PerformanceProfiler::Instance().SetGpuProfilingEnabled(ReadTuning(L"GpuProfilingEnabled", 0) != 0);

    if (!savedBasePath.empty()) {
        renderer::DirectionalVolumetricLighting::Instance().Configure(savedBasePath);
        renderer::VolumetricClouds::Instance().Configure(savedBasePath);
        renderer::AmbientOcclusion::Instance().Configure(savedBasePath);
        renderer::Underwater::Instance().Configure(savedBasePath);
        // The water stage shades the liquid's own pixels - the ones this pass
        // deliberately leaves alone - so the dirt in the water has to reach it:
        // the player's turbidity dial is published here, where its section is
        // read, and each water draw scales it by its zone's multiplier beside
        // the clarity it already scales. A disabled pass publishes 0, which
        // keeps the surface's exit wash under the same switch as the medium.
        const auto& underwaterSettings = renderer::Underwater::Instance().Settings();
        watereffect::underwaterFrame.turbidity = underwaterSettings.enabled
            ? underwaterSettings.turbidity : 0.f;
    }
    cloudsEffectEnabled = renderer::VolumetricClouds::Instance().Settings().enabled;
}

void Configure(const std::wstring& base) {
    savedBasePath = base;
    mainIni = base + L"ModernWoWRenderer.ini";
    tuningIni = base + L"GraphicsEffects.ini";
    logPath = base + L"VolumeEffects.log";
    celestialProbeLogPath = base + L"CelestialProbe.log";
    renderer::PerformanceProfiler::Instance().SetLogPath(logPath);
    renderer::DirectionalVolumetricLighting::Instance().Configure(base);
    renderer::VolumetricClouds::Instance().Configure(base);
    renderer::Underwater::Instance().Configure(base);
    enabled = renderer::locationtuning::ReadInt(L"Volume", L"Enabled", 0, mainIni.c_str()) != 0;
    ReloadTuning();
}

// A confirmed zone change: the precipitation the client was drawing belongs to the
// place the player left, so neither the detected weather nor the deck coverage it
// forced may be carried across (see weathervisuals::ForgetLocation and
// CloudDynamics::ForgetWeather). The cloud field's own climate still eases.
inline void ForgetWeather() noexcept {
    cloudWeather=renderer::CloudWeather::Clear;
    cloudWeatherStrength=0.f;
    renderer::VolumetricClouds::Instance().ForgetWeather();
}

// Frame-scoped DEFAULT resources: persistent textures STAY ALIVE across frames!
void Finish(IDirect3DDevice9* d) {
    // A recreated device has a different pointer. Drop frame-owned references
    // regardless; DepthCapture::Reset handles whether restoration is safe.
    renderer::DepthCapture::Instance().Reset(d);
    surface.Reset(); depth.Reset(); originalDepth.Reset(); target.Reset();
    owner = nullptr; ready = false; composed = false;
    watereffect::mainTargetSurface = nullptr;
    watereffect::waterTargetSurface = watereffect::waterMaskSurface = watereffect::waterDepthSurface = nullptr;
    watereffect::waterTargetDesc = {}; watereffect::waterTargetDescValid = false;
    auto& frame = renderer::FrameContext::Current();
    frame.waterMaskTexture = frame.waterDepthTexture = nullptr;
}

void Reset(IDirect3DDevice9* d) {
    Finish(d);
    legacyShaders.Reset();
    legacyTargets.Reset();
    renderer::DepthCapture::Instance().Reset(d);
    renderer::CameraCapture::Instance().Reset();
    renderer::ShaderCache::Instance().Clear();
    renderer::DrawCallClassifier::Instance().ClearCache();
    renderer::DirectionalVolumetricLighting::Instance().Reset(d);
    renderer::VolumetricClouds::Instance().Reset(d);
    renderer::AmbientOcclusion::Instance().Reset(d);
    renderer::Underwater::Instance().Reset(d);
    renderer::GroundSurfaceCapture::Instance().Reset();
    renderer::PerformanceProfiler::Instance().Reset(d);
    resources.shadowDepthSurface.Reset(); resources.shadowColorSurface.Reset();
    resources.shadowDepth.Reset(); resources.shadowColor.Reset();
    resources.shadowOwner = nullptr; resources.shadowSize = 0;
    resources.waterMaskSurface.Reset(); resources.waterMask.Reset();
    resources.waterDepthSurface.Reset(); resources.waterDepth.Reset();
    resources.waterMaskOwner = nullptr; resources.waterMaskWidth = resources.waterMaskHeight = 0;
    watereffect::waterMaskSurface = nullptr;
    watereffect::waterDepthSurface = nullptr;
    shadowFrameStarted = shadowFrameValid = false;
    stableShadowLightValid = false;
    shadowCacheValid = shadowAnchorValid = false;
    celestialLightStabilizer.Reset();
    celestialDirectionStabilizer.Reset();
    celestialLightClock.Reset();
}

// A break in the presented frame stream (Alt+Tab, minimise, loading screen)
// leaves temporal history describing a view the player no longer has. Drop it
// without touching any GPU resource; the next frame rebuilds it from scratch.
void InvalidateTemporalHistory() {
    legacyTargets.historyValid = false;
    renderer::DirectionalVolumetricLighting::Instance().InvalidateHistory();
    // The next presented frame re-primes the stabilizer at its current
    // target instead of fading the sky in from the pre-break state.
    celestialLightStabilizer.Reset();
    celestialDirectionStabilizer.Reset();
    celestialLightClock.Reset();
}

HRESULT WINAPI SetDepth(IDirect3DDevice9* d, IDirect3DSurface9* s) {
    const HRESULT hr = renderer::DepthCapture::Instance().HookSetDepth(d, s);
    if (SUCCEEDED(hr) && !internal) nativeshadowdiag::OnSetDepth(s);
    return hr;
}
HRESULT WINAPI GetDepth(IDirect3DDevice9* d, IDirect3DSurface9** out) {
    return renderer::DepthCapture::Instance().HookGetDepth(d, out);
}

inline bool HasOtherActiveEffects() {
    const auto& atmosphere = renderer::DirectionalVolumetricLighting::Instance().Settings();
    const bool hasFog = !shaftDebug && (fogEffectEnabled || atmosphere.localFogEnabled ||
        atmosphere.edgeFogEnabled);
    const bool hasVolumetric = shaftsEffectEnabled && atmosphere.enabled;
    const bool hasGlare = sunGlareEnabled && shaftsEffectEnabled &&
        (constants[2][2] > 0.001f || shaftDebug);
    const bool hasClouds = !shaftDebug && cloudsEffectEnabled &&
        renderer::VolumetricClouds::Instance().Settings().enabled;
    const bool hasPost = (postProcessEffectEnabled &&
        (brightnessPercent != 0 || contrastPercent != 100 || gammaPercent != 100 || sharpnessPercent > 0)) ||
        antiAliasingEnabled;
    const bool hasAO = !shaftDebug && renderer::AmbientOcclusion::Instance().IsActive();
    const bool hasLightScatteringDebug = atmosphere.enabled &&
        atmosphere.debugMode == renderer::VolumetricDebugMode::LocalLightScattering;
    // The underwater pass is here for a reason that is not about drawing: it reads the water
    // mask to leave the liquid's own pixels alone, and that mask is allocated below only when
    // this function says some effect will consume it. Without this line a diver with every
    // other effect switched off gets no mask, and the surface is shaded twice - once by the
    // water stage's own camera column and once by the medium.
    const bool hasUnderwater = !shaftDebug && renderer::Underwater::Instance().IsActive();
    return hasFog || hasVolumetric || hasGlare || hasClouds || hasPost || hasAO ||
           hasLightScatteringDebug || hasUnderwater;
}

inline bool HasActiveEffects() {
    const auto& localLights = renderer::LocalLightManager::Instance();
    const bool hasDynamicLighting = renderer::locallighting::ShouldActivate(
        localLights.Enabled(), localLights.IntensityScale(), localLights.DebugMode());
    return HasOtherActiveEffects() || hasDynamicLighting;
}

void BeforeClear(IDirect3DDevice9* d, DWORD count, const D3DRECT* rects, DWORD flags, float z) {
    if (internal || (!(enabled && active) && !watereffect::NeedsScene())) return;
    if (!renderer::DepthCapture::Instance().BeforeClear(d, count, rects, flags, z)) return;
    renderer::PerformanceProfiler::Instance().OnFrameBegin(d);
    // (The diagnostics' own OnFrameBegin was called here and had nothing to do
    // that this pass could be trusted with: this hook runs only when the volume
    // effect is on, so a per-frame number reset here is a number that is wrong
    // whenever the effect is off. The draws the client made are counted as a
    // session total and differenced at Present, which IS every frame.)
    depth = renderer::DepthCapture::Instance().GetDepthTexture();
    surface = renderer::DepthCapture::Instance().GetDepthSurface();
    originalDepth = renderer::DepthCapture::Instance().GetOriginalDepth();
    target = renderer::DepthCapture::Instance().GetRenderTarget();
    // The surface the player's own view is rendered into this frame, published
    // for the water draws: their plane readings accept only draws aimed here,
    // which is what keeps the client's reflection pass (its own target, a
    // mirrored camera) out of the submersion tracker.
    watereffect::mainTargetSurface = target.Get();
    // The underwater pass counts presented frames here toward building its
    // DEVICE shader (its bytecode is compiled on a worker from Configure), so
    // neither a cold fxc run nor a cold driver JIT is ever paid inside the
    // first frames of a launch - the white login window that synchronous
    // preparation in this hook used to produce - nor mid-dive.
    renderer::Underwater::Instance().Prewarm(d);
    owner = renderer::DepthCapture::Instance().GetOwner();
    depthFrames = renderer::DepthCapture::Instance().GetDepthFrames();
    if (renderer::DepthCapture::Instance().HasDepth()) {
        auto& frameCtx = renderer::FrameContext::Current();
        frameCtx.ResetForNewFrame(frameCtx.frameIndex + 1, d);
        ResolveWorldSceneForFrame();
        // Precipitation composites itself over the scene during the client's draw stream, which
        // starts now and ends before this frame's water draws settle the medium at Present. So it
        // cannot read underwaterRamp (cleared just above) or wait for the composite: publish the
        // ramp the last composite settled, so the precipitation stage can fade out below the
        // surface instead of drawing rain over the water. One frame late, which the ramp's own
        // third-of-a-second smoothing makes invisible. Gated exactly like the composite's own
        // publish (including the shaft debug mask), so with the underwater pass off this is
        // always 0 and precipitation above and below the surface is untouched.
        frameCtx.compositedUnderwaterRamp = (!shaftDebug && enabled && active &&
            renderer::Underwater::Instance().IsActive())
            ? std::clamp(watereffect::submersionTracker.ramp, 0.f, 1.f) : 0.f;
        shadowFrameStarted = false; shadowFrameValid = false; shadowFrameDraws = 0;
        renderer::CelestialTracker::Instance().BeginFrame(frameCtx.frameIndex);
        renderer::CameraCapture::Instance().BeginFrame();
        // Both describe this frame's capture attempt. A frame that never
        // latches a camera must not hand DrawCallClassifier the previous
        // frame's "terrain shader" or view translation.
        capturedViewValid = false;
        cameraCaptureShaderHash = 0;

        // Per-frame water mask: watereffect::Scope binds this as a second
        // render target during replacement-water draws; when that shader is
        // switched off, the draw hooks replay native liquid geometry through
        // a mask-only pixel shader instead. Replacement coverage stays in red;
        // the native-only green tag lets the final pass preserve only those pixels.
        // The replacement path also supplies a surface-depth MRT. Native
        // water has no matching MRT, so the final atmosphere composite uses
        // this frame's capture flag to pass those marked pixels through rather
        // than reconstructing fog to the seabed and painting over the sheet.
        // Sized to the real render target, cleared fresh every frame before
        // any water draws happen.
        watereffect::waterTargetSurface = watereffect::waterMaskSurface = watereffect::waterDepthSurface = nullptr;
        watereffect::waterTargetDesc = {}; watereffect::waterTargetDescValid = false;
        frameCtx.waterMaskTexture = frameCtx.waterDepthTexture = nullptr;
        D3DSURFACE_DESC targetDesc{};
        // Water still needs scene/depth capture for SSR and refraction when
        // the volume renderer is disabled, but no pass then consumes its
        // atmosphere-only mask or surface-depth MRTs. Avoid allocating and
        // clearing two full-resolution targets on that independent path.
        const bool volumeConsumesWaterBoundary = enabled && active && HasActiveEffects();
        if (volumeConsumesWaterBoundary && target && SUCCEEDED(target->GetDesc(&targetDesc))) {
            const bool sizeChanged = resources.waterMaskOwner != d ||
                !renderer::water::TargetsMatch(targetDesc.Width, targetDesc.Height,
                                               resources.waterMaskWidth, resources.waterMaskHeight);
            if (sizeChanged) {
                // Invalidate the pair BEFORE updating dimensions. Previously
                // the mask updated the shared dimensions first, leaving the
                // depth texture at its old size after a window resize.
                resources.waterMaskSurface.Reset(); resources.waterMask.Reset();
                resources.waterDepthSurface.Reset(); resources.waterDepth.Reset();
                resources.waterMaskOwner = d;
                resources.waterMaskWidth = targetDesc.Width;
                resources.waterMaskHeight = targetDesc.Height;
            }
            if (!resources.waterMaskSurface) {
                resources.waterMask.Reset();
                if (SUCCEEDED(d->CreateTexture(targetDesc.Width, targetDesc.Height, 1, D3DUSAGE_RENDERTARGET,
                        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, resources.waterMask.GetAddressOf(), nullptr)))
                    resources.waterMask->GetSurfaceLevel(0, resources.waterMaskSurface.GetAddressOf());
            }
            if (!resources.waterDepthSurface) {
                resources.waterDepth.Reset();
                // R32F keeps all three MRTs at 32 bits per pixel. A 64-bit
                // fallback is invalid on devices without independent MRT bit depths.
                if (SUCCEEDED(d->CreateTexture(targetDesc.Width, targetDesc.Height, 1, D3DUSAGE_RENDERTARGET,
                        D3DFMT_R32F, D3DPOOL_DEFAULT, resources.waterDepth.GetAddressOf(), nullptr)))
                    resources.waterDepth->GetSurfaceLevel(0, resources.waterDepthSurface.GetAddressOf());
            }
            // A mask without matching, freshly cleared depth would stop air at
            // stale water. Expose both or neither, and fail without disabling water.
            auto clearWaterSurface = [&](IDirect3DSurface9* surface) -> bool {
                IDirect3DSurface9* previous = nullptr;
                if (FAILED(d->GetRenderTarget(0, &previous))) return false;
                const bool ok = SUCCEEDED(d->SetRenderTarget(0, surface)) &&
                                SUCCEEDED(d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0));
                if (previous) { d->SetRenderTarget(0, previous); previous->Release(); }
                return ok;
            };
            if (resources.waterMaskSurface && resources.waterDepthSurface &&
                clearWaterSurface(resources.waterMaskSurface.Get()) &&
                clearWaterSurface(resources.waterDepthSurface.Get())) {
                watereffect::waterTargetSurface = target.Get();
                watereffect::waterMaskSurface = resources.waterMaskSurface.Get();
                watereffect::waterDepthSurface = resources.waterDepthSurface.Get();
                watereffect::waterTargetDesc = targetDesc;
                watereffect::waterTargetDescValid = true;
                frameCtx.waterMaskTexture = resources.waterMask.Get();
                frameCtx.waterDepthTexture = resources.waterDepth.Get();
            }
        } else if (!volumeConsumesWaterBoundary && resources.waterMaskOwner) {
            // Release once on disable as well as skipping the per-frame clears;
            // at 4K this pair otherwise strands 64 MiB of default-pool VRAM.
            resources.waterMaskSurface.Reset(); resources.waterMask.Reset();
            resources.waterDepthSurface.Reset(); resources.waterDepth.Reset();
            resources.waterMaskOwner = nullptr;
            resources.waterMaskWidth = resources.waterMaskHeight = 0;
        }
    }
}

template<class T> uint64_t Hash(T* shader) {
    if (!shader) return 0;
    if constexpr (std::is_same_v<T, IDirect3DVertexShader9>)
        return renderer::ShaderCache::Instance().GetShaderHash(shader);
    else if constexpr (std::is_same_v<T, IDirect3DPixelShader9>)
        return renderer::ShaderCache::Instance().GetShaderHash(shader);
    else
        return 0;
}

uint32_t NoiseHash(int x, int y) { uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
float SmoothNoise(float x, float y, int cells) {
    int x0 = int(floor(x)), y0 = int(floor(y)); float fx = x - x0, fy = y - y0; fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    auto sample = [&](int sx, int sy) {return float(NoiseHash(sx & (cells - 1), sy & (cells - 1)) & 65535u) / 65535.f; };
    float a = sample(x0, y0), b = sample(x0 + 1, y0), c = sample(x0, y0 + 1), e = sample(x0 + 1, y0 + 1);
    return (a + (b - a) * fx) * (1 - fy) + (c + (e - c) * fx) * fy;
}

bool EnsureNoise(IDirect3DDevice9* d) {
    if (resources.noise && resources.noiseOwner == d) return true;
    resources.noise.Reset(); resources.noiseOwner = nullptr;
    ComPtr<IDirect3DTexture9> texture; if (FAILED(d->CreateTexture(128, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture.GetAddressOf(), nullptr))) return false;
    D3DLOCKED_RECT locked{}; if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) return false;
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        float sum = 0, weight = 0, amplitude = 1;
        for (int octave = 0; octave < 4; ++octave) { int cells = 4 << octave; sum += SmoothNoise(x * cells / 128.f, y * cells / 128.f, cells) * amplitude; weight += amplitude; amplitude *= .52f; }
        BYTE n = BYTE(std::clamp(sum / weight, 0.f, 1.f) * 255 + .5f); *reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch + x * 4) = D3DCOLOR_ARGB(255, n, n, n);
    }
    texture->UnlockRect(0); resources.noise = texture; resources.noiseOwner = d; return true;
}

bool CaptureCamera(IDirect3DDevice9* d) {
    renderer::CameraCaptureConfig cfg;
    cfg.sunOffsetX = sunOffsetX;
    cfg.sunOffsetY = sunOffsetY;
    cfg.sunVerticalScale = sunVerticalScale;
    cfg.strength = strength;
    cfg.moonStrength = moonStrength;
    cfg.baseHeight = baseHeight;
    cfg.falloff = falloff;
    cfg.density = density;
    cfg.fogWash = fogWash;
    cfg.shaftDebug = shaftDebug;
    cfg.variation = variation;
    cfg.lowLayer = lowLayer;
    cfg.raySoftness = raySoftness;
    cfg.rayFalloff = rayFalloff;

    renderer::FrameContext& frameCtx = renderer::FrameContext::Current();
    bool ok = renderer::CameraCapture::Instance().Capture(
        d, frameCtx, cfg, constants, capturedViewTranslation, capturedViewValid,
        renderer::g_trackedState.vsHash);
    if (ok) {
        renderer::RendererDiagnostics::Instance().RecordCameraCapture();
        celestialDaylight = frameCtx.daylightFactor;
        celestialMoonlight = frameCtx.moonlightFactor;
        celestialShadowLight = frameCtx.shadowLightFactor;
        smoothSunX = frameCtx.sunScreenX;
        smoothSunY = frameCtx.sunScreenY;
        frameCtx.depthTexture = resources.depth.Get();
        frameCtx.depthSurface = resources.surface.Get();
        frameCtx.depthAvailable = (resources.depth != nullptr);
        // c13: second height-fog layer (ground mist) - x=height,y=falloff,z=density
        constants[13][0] = groundMistHeight;
        constants[13][1] = groundMistFalloff;
        constants[13][2] = groundMistDensity;
        constants[13][3] = 0.f;
    }
    return ok;
}

bool CaptureSkyFallback() {
    renderer::CameraCaptureConfig cfg;
    cfg.sunOffsetX = sunOffsetX;
    cfg.sunOffsetY = sunOffsetY;
    cfg.sunVerticalScale = sunVerticalScale;
    cfg.strength = strength;
    cfg.moonStrength = moonStrength;
    cfg.baseHeight = baseHeight;
    cfg.falloff = falloff;
    cfg.density = density;
    cfg.fogWash = fogWash;
    cfg.shaftDebug = shaftDebug;
    cfg.variation = variation;
    cfg.lowLayer = lowLayer;
    cfg.raySoftness = raySoftness;
    cfg.rayFalloff = rayFalloff;

    renderer::FrameContext& frameCtx = renderer::FrameContext::Current();
    bool ok = renderer::CameraCapture::Instance().ApplySkyFallback(
        frameCtx, cfg, constants, capturedViewTranslation, capturedViewValid);
    if (ok) {
        renderer::RendererDiagnostics::Instance().RecordCameraCapture();
        frameCtx.depthTexture = resources.depth.Get();
        frameCtx.depthSurface = resources.surface.Get();
        frameCtx.depthAvailable = (resources.depth != nullptr);
        constants[13][0] = groundMistHeight;
        constants[13][1] = groundMistFalloff;
        constants[13][2] = groundMistDensity;
        constants[13][3] = 0.f;
    }
    return ok;
}

struct Vec3 { float x, y, z; };
Vec3 Add(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 Mul(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
Vec3 Normalize(Vec3 a) { float n = sqrtf(std::max(Dot(a, a), 1e-8f)); return Mul(a, 1.f / n); }

bool EnsureShadowResources(IDirect3DDevice9* d) {
    if (resources.shadowOwner == d && resources.shadowSize == configuredShadowMapSize && resources.shadowDepth && resources.shadowColor && resources.shadowDepthSurface && resources.shadowColorSurface) return true;
    resources.shadowDepthSurface.Reset(); resources.shadowColorSurface.Reset(); resources.shadowDepth.Reset(); resources.shadowColor.Reset(); resources.shadowOwner = nullptr; resources.shadowSize = 0; shadowCacheValid = false;
    if (FAILED(d->CreateTexture(configuredShadowMapSize, configuredShadowMapSize, 1, D3DUSAGE_DEPTHSTENCIL, static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DPOOL_DEFAULT, resources.shadowDepth.GetAddressOf(), nullptr)) ||
        FAILED(resources.shadowDepth->GetSurfaceLevel(0, resources.shadowDepthSurface.GetAddressOf())) ||
        FAILED(d->CreateTexture(configuredShadowMapSize, configuredShadowMapSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, resources.shadowColor.GetAddressOf(), nullptr)) ||
        FAILED(resources.shadowColor->GetSurfaceLevel(0, resources.shadowColorSurface.GetAddressOf()))) return false;
    resources.shadowOwner = d; resources.shadowSize = configuredShadowMapSize; return true;
}

void BuildShadowCamera() {
    Vec3 camera{ constants[7][0], constants[7][1], constants[7][2] };
    Vec3 invX{ constants[4][0], constants[4][1], constants[4][2] }, invY{ constants[5][0], constants[5][1], constants[5][2] }, invZ{ constants[6][0], constants[6][1], constants[6][2] };
    Vec3 lightView{ constants[10][0], constants[10][1], constants[10][2] };
    Vec3 measuredLight = Normalize(Add(Add(Mul(invX, lightView.x), Mul(invY, lightView.y)), Mul(invZ, lightView.z)));
    if (measuredLight.z < 0.04f) measuredLight.z = 0.04f;
    measuredLight = Normalize(measuredLight);
    Vec3 towardLight = measuredLight;
    if (!stableShadowLightValid) {
        stableShadowLight[0] = measuredLight.x; stableShadowLight[1] = measuredLight.y; stableShadowLight[2] = measuredLight.z; stableShadowLightValid = true;
    }
    else {
        Vec3 stable{ stableShadowLight[0], stableShadowLight[1], stableShadowLight[2] };
        if (Dot(stable, measuredLight) < 0.999f) {
            stableShadowLight[0] = measuredLight.x; stableShadowLight[1] = measuredLight.y; stableShadowLight[2] = measuredLight.z;
        }
        else towardLight = stable;
    }
    Vec3 forward = Mul(towardLight, -1.f), reference = fabsf(forward.z) > .94f ? Vec3{ 0,1,0 } : Vec3{ 0,0,1 };
    Vec3 right = Normalize(Cross(reference, forward)), up = Normalize(Cross(forward, right));
    float halfExtent = shadowMapDistance * 0.95f;
    float texelWorld = (halfExtent * 2.f) / float(std::max<UINT>(configuredShadowMapSize, 1));
    Vec3 center = camera;
    float alongRight = Dot(center, right), alongUp = Dot(center, up);
    center = Add(center, Mul(right, roundf(alongRight / texelWorld) * texelWorld - alongRight));
    center = Add(center, Mul(up, roundf(alongUp / texelWorld) * texelWorld - alongUp));
    Vec3 eye = Add(center, Mul(towardLight, shadowMapDistance * 2.0f));
    memset(shadowView, 0, sizeof(shadowView));
    shadowView[0][0] = right.x; shadowView[0][1] = up.x; shadowView[0][2] = forward.x;
    shadowView[1][0] = right.y; shadowView[1][1] = up.y; shadowView[1][2] = forward.y;
    shadowView[2][0] = right.z; shadowView[2][1] = up.z; shadowView[2][2] = forward.z;
    shadowView[3][0] = -Dot(right, eye); shadowView[3][1] = -Dot(up, eye); shadowView[3][2] = -Dot(forward, eye); shadowView[3][3] = 1;
    float nearPlane = 1.f, farPlane = shadowMapDistance * 4.5f;
    memset(shadowProjection, 0, sizeof(shadowProjection));
    shadowProjection[0][0] = 1.f / halfExtent; shadowProjection[1][1] = 1.f / halfExtent; shadowProjection[2][2] = 1.f / (farPlane - nearPlane); shadowProjection[3][2] = -nearPlane / (farPlane - nearPlane); shadowProjection[3][3] = 1;
    for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) { shadowMatrix[column][row] = 0; for (int k = 0; k < 4; ++k) shadowMatrix[column][row] += shadowProjection[k][row] * shadowView[column][k]; }
    float inverseView[4][4] = { {constants[4][0], constants[4][1], constants[4][2], 0},
                             {constants[5][0], constants[5][1], constants[5][2], 0},
                             {constants[6][0], constants[6][1], constants[6][2], 0},
                             {constants[7][0], constants[7][1], constants[7][2], 1} };
    for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) { viewToShadowMatrix[column][row] = 0; for (int k = 0; k < 4; ++k) viewToShadowMatrix[column][row] += shadowMatrix[k][row] * inverseView[column][k]; }
    auto& ctx = renderer::FrameContext::Current();
    for (int col = 0; col < 4; ++col) for (int r = 0; r < 4; ++r) {
        ctx.shadowMatrix.m[col][r] = shadowMatrix[col][r];
        ctx.viewToShadowMatrix.m[col][r] = viewToShadowMatrix[col][r];
    }
    ctx.shadowTexture = resources.shadowDepth.Get();
    ctx.shadowMapValid = true;
    ctx.shadowMapSize = configuredShadowMapSize;
    ctx.shadowMapDistance = shadowMapDistance;
}

// SAFETY GATE - do not flip this via ini. Reported in-game: shadow
// orientation changing with camera rotation, jagged/crawling silhouettes,
// and a client crash while this light-space path was exercised. It saves/
// restores render state and vertex-shader constants around REPLAYING the
// original WoW draw call into the shadow target - that replay has not been
// proven state-safe (texture stages/samplers/alpha-ref/vertex declaration
// are never saved or restored, only a fixed constant-register range and a
// handful of render states are). Until that is audited and the world-space
// stability issue is root-caused, this path must stay unreachable even if
// ShadowMapEnabled=1 is set in GraphicsEffects.ini.
constexpr bool kAllowExperimentalLightSpaceShadows = false;

inline bool ShouldUpdateShadows()
{
    if (!kAllowExperimentalLightSpaceShadows) return false;
    if (!enabled || !active || internal || !shadowsEffectEnabled || !shadowMapEnabled || directionalShadowStrength <= 0)
        return false;
    return (frames % shadowMapUpdateInterval == 0);
}

template<class DrawCall> void ShadowDraw(IDirect3DDevice9* d, const renderer::DrawClassification& dc, DrawCall&& draw) {
    if (!kAllowExperimentalLightSpaceShadows) return;
    if (!enabled || !active || internal || !ready || !shadowsEffectEnabled || !shadowMapEnabled || directionalShadowStrength <= 0 || owner != d) return;
    if (!dc.castsShadow) return;
    if (!volumetricCharacterShadows &&
        (dc.material == renderer::MaterialType::Character || dc.material == renderer::MaterialType::M2)) return;
    if (!ShouldUpdateShadows()) return;

    renderer::ScopedCpuTimer shadowTimer(renderer::PerfStage::ShadowMapBuild);

    if (!EnsureShadowResources(d)) return;
    if (!shadowFrameStarted) {
        shadowFrameStarted = true;
        internal = true;
        try {
            BuildShadowCamera();
            setDepth(d, nullptr);
            d->SetRenderTarget(0, resources.shadowColorSurface.Get());
            setDepth(d, resources.shadowDepthSurface.Get());
            D3DVIEWPORT9 vp{ 0, 0, configuredShadowMapSize, configuredShadowMapSize, 0, 1 };
            d->SetViewport(&vp);
            d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffffffff, 1.0f, 0);
            setDepth(d, nullptr);
            d->SetRenderTarget(0, target.Get());
            setDepth(d, surface.Get());
            D3DVIEWPORT9 mainVp{ 0, 0, legacyTargets.width, legacyTargets.height, 0, 1 };
            d->SetViewport(&mainVp);
            shadowFrameValid = true;
        } catch (...) {
            // A half-built shadow frame is unusable (shadowFrameValid stays
            // false, so this frame skips shadows) but the framebuffer must be
            // back on the main target or every later draw of the frame would
            // land in the shadow map.
            d->SetRenderTarget(0, target.Get());
            setDepth(d, surface.Get());
            D3DVIEWPORT9 mainVp{ 0, 0, legacyTargets.width, legacyTargets.height, 0, 1 };
            d->SetViewport(&mainVp);
        }
        internal = false;
    }
    if (!shadowFrameValid) return;

    // The block holds the render states, the pixel shader and the vertex
    // constants at once; only the framebuffer attachments (which a block
    // cannot hold) are saved manually. The framebuffer restore runs no
    // matter what - a throw mid-pass must not leave the shadow map bound
    // as RT0, which would silently swallow every later draw of the frame.
    ComPtr<IDirect3DSurface9> rt0, depth0;
    D3DVIEWPORT9 oldVp{};
    if (FAILED(d->GetRenderTarget(0, rt0.GetAddressOf()))) return;
    renderer::DepthCapture::Instance().RawGetDepth(d, depth0.GetAddressOf());
    if (FAILED(d->GetViewport(&oldVp))) return;
    DWORD alphaTest = 0;
    d->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
    DWORD colorWrite = 0, zEnable = 0, zWrite = 0, zFunc = 0, fog = 0;
    DWORD alphaBlend = 0, oldDepthBias = 0, oldSlopeBias = 0, oldCull = 0;
    ComPtr<IDirect3DPixelShader9> originalPixelShader;
    float originalConsts[8][4]{};
    const bool block = renderer::detail::BeginPassState(d);
    if (!block) {
        d->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
        d->GetRenderState(D3DRS_ZENABLE, &zEnable);
        d->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
        d->GetRenderState(D3DRS_ZFUNC, &zFunc);
        d->GetRenderState(D3DRS_FOGENABLE, &fog);
        d->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
        d->GetRenderState(D3DRS_DEPTHBIAS, &oldDepthBias);
        d->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &oldSlopeBias);
        d->GetRenderState(D3DRS_CULLMODE, &oldCull);
        d->GetPixelShader(originalPixelShader.GetAddressOf());
        d->GetVertexShaderConstantF(0, originalConsts[0], 8);
    }
    auto restoreFramebuffer = [&] {
        d->SetRenderTarget(0, rt0.Get());
        renderer::DepthCapture::Instance().RawSetDepth(d, depth0.Get());
        d->SetViewport(&oldVp);
    };
    internal = true;
    try {
        setDepth(d, nullptr);
        d->SetRenderTarget(0, resources.shadowColorSurface.Get());
        setDepth(d, resources.shadowDepthSurface.Get());
        D3DVIEWPORT9 shadowVp{ 0, 0, configuredShadowMapSize, configuredShadowMapSize, 0, 1 };
        d->SetViewport(&shadowVp);

        bool isWmo = (dc.material == renderer::MaterialType::WMO);
        if (isWmo) {
            d->SetVertexShaderConstantF(4, viewToShadowMatrix[0], 4);
        }
        else {
            float shadowRows[4][4]{};
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    shadowRows[row][col] = viewToShadowMatrix[col][row];
            d->SetVertexShaderConstantF(2, shadowRows[0], 4);
        }

        float rasterBias = .00045f, slopeBias = 2.25f;
        DWORD rasterBiasBits = 0, slopeBiasBits = 0;
        memcpy(&rasterBiasBits, &rasterBias, sizeof(DWORD));
        memcpy(&slopeBiasBits, &slopeBias, sizeof(DWORD));
        d->SetRenderState(D3DRS_DEPTHBIAS, rasterBiasBits);
        d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, slopeBiasBits);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        d->SetRenderState(D3DRS_ZENABLE, TRUE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        if (!alphaTest) d->SetPixelShader(nullptr);

        HRESULT shadowHr = draw();
        if (SUCCEEDED(shadowHr)) {
            ++shadowFrameDraws;
            if (++shadowDraws == 1) Log("uncapped depth-only light-space shadows active");
        }
    } catch (...) {
        // The pass state and framebuffer are restored below regardless; the
        // throw only costs this one shadow draw.
    }
    if (block) {
        renderer::detail::EndPassState();
    } else {
        d->SetPixelShader(originalPixelShader.Get());
        d->SetVertexShaderConstantF(0, originalConsts[0], 8);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
        d->SetRenderState(D3DRS_ZENABLE, zEnable);
        d->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        d->SetRenderState(D3DRS_ZFUNC, zFunc);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, alphaBlend);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, alphaTest);
        d->SetRenderState(D3DRS_DEPTHBIAS, oldDepthBias);
        d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, oldSlopeBias);
        d->SetRenderState(D3DRS_CULLMODE, oldCull);
        d->SetRenderState(D3DRS_FOGENABLE, fog);
    }
    restoreFramebuffer();
    internal = false;
}

bool Composite(IDirect3DDevice9* d) {
    ResolveWorldSceneForFrame();
    const auto& sceneFrame=renderer::FrameContext::Current();
    const bool playableWorldEvidence=renderer::HasPlayableWorldEvidence(
        sceneFrame.worldScene.state,sceneFrame.worldTerrainSeenThisFrame||sceneFrame.worldWaterSeenThisFrame);
    if(!playableWorldEvidence) renderer::DirectionalVolumetricLighting::Instance().InvalidateHistory();
    // Every branch below samples or writes the scene copy the clear hook publishes.
    // A camera captured without a depth capture (which is now possible, and is the
    // point) must not reach them: there is nothing to composite onto.
    if (!target || !depth) return false;
    const auto& localLightManager = renderer::LocalLightManager::Instance();
    const bool dynamicLightDebugView = localLightManager.Enabled() &&
        renderer::locallighting::IsDebugView(localLightManager.DebugMode());
    const bool dynamicLightingActive = renderer::locallighting::ShouldActivate(
        localLightManager.Enabled(), localLightManager.IntensityScale(), localLightManager.DebugMode());
    const bool otherEffectsActive = HasOtherActiveEffects();
    if (!dynamicLightingActive && !otherEffectsActive) return true;

    renderer::ScopedCpuTimer totalTimer(renderer::PerfStage::TotalInjectedFrame);
    renderer::ScopedCpuTimer compTimer(renderer::PerfStage::LegacyComposite);

    // Isolated local-light diagnostics and a dynamic-light-only composite do
    // not need the atmosphere's noise, temporal buffers or legacy shader set.
    // Copy just the scene texture that the surface pass samples, render it,
    // and return before unrelated full-screen work can tint the result.
    if (dynamicLightDebugView || (dynamicLightingActive && !otherEffectsActive)) {
        D3DSURFACE_DESC sceneDesc{};
        if (!target || FAILED(target->GetDesc(&sceneDesc))) return true;
        const UINT rayWidth = std::max<UINT>(1, sceneDesc.Width / 2);
        const UINT rayHeight = std::max<UINT>(1, sceneDesc.Height / 2);
        if (!legacyTargets.Ensure(d, sceneDesc.Width, sceneDesc.Height, sceneDesc.Format,
                                  rayWidth, rayHeight)) return true;
        if (FAILED(d->StretchRect(target.Get(), nullptr, legacyTargets.sceneSurface.Get(),
                                  nullptr, D3DTEXF_NONE))) return true;
        auto& debugFrame = renderer::FrameContext::Current();
        debugFrame.sceneColor = legacyTargets.scene.Get();
        debugFrame.sceneSurface = legacyTargets.sceneSurface.Get();
        struct InternalGuard {
            bool& flag;
            bool previous;
            ~InternalGuard() { flag = previous; }
        } internalGuard{internal, internal};
        internal = true;
        if (!renderer::LocalLightingRenderer::Instance().Render(d, debugFrame)) return true;
        d->StretchRect(debugFrame.sceneSurface, nullptr, target.Get(), nullptr, D3DTEXF_NONE);
        return true;
    }

    if (!EnsureNoise(d)) return false;
    if (!legacyShaders.Ensure(d)) return false;

    // Real celestial source override. Replaces the v[24]-projection
    // ("assume the light-direction shader constant is the disc's screen
    // position") that in-game testing disproved. CelestialTracker
    // intercepts the actual sun/moon billboard draw call each frame (see
    // src/D3D9/CelestialTracker.h for the two confirmed draw signatures)
    // and reports its true screen position and view direction - only for
    // frames where that draw call actually happened, so an off-screen or
    // below-horizon body naturally yields no source instead of a guessed
    // one. constants[2]/constants[10] are re-uploaded to the shaders below
    // via SetPixelShaderConstantF, so overriding them here in place before
    // any of those uploads happen is sufficient - no separate plumbing.
    {
        const renderer::CelestialBody& sun = renderer::CelestialTracker::Instance().Sun();
        const renderer::CelestialBody& moon = renderer::CelestialTracker::Instance().Moon();
        // Gating strength on celestialDaylight (the OLD, separate v[26]-
        // luminance/tint heuristic) was a leftover mistake: it can read
        // near-zero in perfectly sunny scenes with a cool/blue-tinted
        // direct light color (this server's skies lean that way), silently
        // killing rays even though CelestialTracker has ALREADY confirmed
        // the real sun disc is drawn and visible this frame - a marker
        // sitting correctly on the sun with no rays was exactly that bug.
        // Visibility from the actual draw call is the ground truth now;
        // it doesn't need a second, weaker opinion to also say yes.
        auto& frameCtx = renderer::FrameContext::Current();
        // A stale pose means the disc position/depth were measured against a
        // camera this frame is not using: the atmosphere, the cloud deck and the
        // water treat that signal as "the moon is here", so a stale one would
        // pin the moon's protection circle to the wrong part of the ground. No
        // pose, no disc.
        frameCtx.moonDiscValid = moon.visible && moon.viewSpacePosition.z > 0.05f &&
            !frameCtx.cameraPoseStale;
        if (frameCtx.moonDiscValid) {
            frameCtx.moonDiscScreenX = moon.screenX;
            frameCtx.moonDiscScreenY = moon.screenY;
            frameCtx.moonDiscViewDepth = moon.viewSpacePosition.z;
        }
        // Both discs can be rendered in the same frame.  "Sun wins whenever
        // visible" consequently kept the radial pass attached to the bright
        // sun candidate even in a moon-lit sky, producing an oversized halo
        // while the actual moon had no shafts. CameraCapture already supplies
        // the authoritative day/night blend: prefer the tracked moon when
        // moonlight dominates and the sun when daylight dominates. There is
        // still exactly one directional source, matching the scene lighting.
        const bool nightDominant = frameCtx.moonlightFactor > frameCtx.daylightFactor;
        bool useMoon = moon.visible && (nightDominant || !sun.visible);
        bool useSun = sun.visible && !useMoon;
        // The tracked screen position belongs to the camera that was captured
        // when the sprite was drawn. With a stale pose it points somewhere the
        // current view does not, and every consumer of it - the glare, the
        // halo, the disc protection - would place a bright disc on whatever
        // now occupies that pixel. One frame without a celestial source is
        // invisible; a sun painted onto a wall is not.
        const renderer::CelestialBody* body = frameCtx.cameraPoseStale
            ? nullptr : (useSun ? &sun : (useMoon ? &moon : nullptr));
        if (body) {
            constants[2][0] = body->screenX;
            constants[2][1] = body->screenY;
            constants[2][2] = (useSun ? strength : strength * moonStrength) * frameCtx.environment[renderer::Rays];
            constants[10][0] = body->viewSpaceDirection.x;
            constants[10][1] = body->viewSpaceDirection.y;
            constants[10][2] = body->viewSpaceDirection.z;
            constants[10][3] = 0.f;
        } else {
            constants[2][0] = -2.f;
            constants[2][1] = -2.f;
            constants[2][2] = 0.f;
        }

        // CelestialTracker is the single source of truth for celestial
        // direction/position; mirror it into FrameContext too so anything
        // else reading these (currently only the debug-only, off-by-default
        // DirectionalVolumetricLighting path) can't disagree with it by
        // still carrying the old v[24]-derived value.
        // The tracker's worldSpaceDirection is derived from the disc draw's own
        // matrices. For the sun that is harmless - its billboard lives in world
        // space - but the moon billboard is camera-relative by construction
        // (near-identity WORLD, direction carried in the VIEW translation), so
        // converting with the draw's own VIEW yields a direction expressed
        // against the camera, not the world. Fed into the cloud deck, that is
        // "look at the moon and the cloud lighting follows the camera". Convert
        // the reported view-space direction with this frame's captured scene
        // camera instead (CameraCapture gated it, so it is the real one); only
        // when no camera was captured fall back to the tracker's derivation.
        auto celestialWorldDirection = [&frameCtx](const renderer::CelestialBody& celestial) {
            if (frameCtx.viewRawValid) {
                const renderer::Vec3& view = celestial.viewSpaceDirection;
                float world[3];
                renderer::ViewDirectionToWorld(&frameCtx.viewRaw.m[0][0], &view.x, world);
                return renderer::Vec3{world[0], world[1], world[2]};
            }
            return celestial.worldSpaceDirection;
        };
        if (body) {
            frameCtx.sunScreenX = body->screenX;
            frameCtx.sunScreenY = body->screenY;
            frameCtx.sunStrength = constants[2][2];
            frameCtx.sunDirectionView = body->viewSpaceDirection;
            frameCtx.sunDirectionWorld = celestialWorldDirection(*body);
            // Source kind and visibility come from CelestialTracker. The
            // atmosphere owns the sun/moon intensity ratio, so keep this a
            // normalized visibility signal instead of applying moon strength
            // twice (once here and once in the medium integration).
            frameCtx.celestialIntensity = 1.f;
            frameCtx.celestialIsMoon = useMoon;
        } else {
            frameCtx.sunScreenX = -1.f;
            frameCtx.sunScreenY = -1.f;
            frameCtx.sunStrength = 0.f;
            frameCtx.celestialIntensity = 0.f;
            frameCtx.celestialIsMoon = false;
        }

        // World lighting must not inherit `body` / `useMoon` above: those
        // select a visible glare origin, and turning toward a daytime moon
        // used to replace the sun's shadow projection. Scene lighting alone
        // selects the dominant body and strength. Track each disc direction
        // independently, even when it is not the selected glare/light source.
        {
            const float lightDt = celestialLightClock.Observe(
                renderer::PerformanceCounter(), renderer::PerformanceFrequency());
            renderer::CelestialLightInput lightInput;
            lightInput.daylight = frameCtx.daylightFactor;
            lightInput.moonlight = frameCtx.moonlightFactor;
            lightInput.directionalLuminance =
                frameCtx.directionalLightColor.x * .2126f +
                frameCtx.directionalLightColor.y * .7152f +
                frameCtx.directionalLightColor.z * .0722f;
            lightInput.deltaSeconds = lightDt;
            const auto stable = celestialLightStabilizer.Update(lightInput);
            frameCtx.directLightIntensity = stable.intensity;
            frameCtx.directLightIsMoon = stable.isMoon;
            frameCtx.directLightValid = true;
            // Directions enter the stabilizer already in world space, converted
            // above: stabilizing the tracker's camera-relative moon vector here
            // only made the camera tracking smoother, not smaller.
            const renderer::Vec3 sunDirectionWorld = celestialWorldDirection(sun);
            const renderer::Vec3 moonDirectionWorld = celestialWorldDirection(moon);
            const auto sunWorld = celestialDirectionStabilizer.Update(
                sun.visible ? &sunDirectionWorld : nullptr, false, lightDt);
            const auto moonWorld = celestialDirectionStabilizer.Update(
                moon.visible ? &moonDirectionWorld : nullptr, true, lightDt);
            frameCtx.directLightDirectionWorld = stable.isMoon ? moonWorld : sunWorld;
            const auto& world = frameCtx.directLightDirectionWorld;
            frameCtx.directLightDirectionView = {
                renderer::Dot(world, {frameCtx.inverseView.m[0][0], frameCtx.inverseView.m[0][1], frameCtx.inverseView.m[0][2]}),
                renderer::Dot(world, {frameCtx.inverseView.m[1][0], frameCtx.inverseView.m[1][1], frameCtx.inverseView.m[1][2]}),
                renderer::Dot(world, {frameCtx.inverseView.m[2][0], frameCtx.inverseView.m[2][1], frameCtx.inverseView.m[2][2]})};
        }
    }

    D3DSURFACE_DESC desc{};
    if (FAILED(target->GetDesc(&desc))) return false;

    UINT rayWidth = std::max<UINT>(1, desc.Width / 2);
    UINT rayHeight = std::max<UINT>(1, desc.Height / 2);

    if (!legacyTargets.Ensure(d, desc.Width, desc.Height, desc.Format, rayWidth, rayHeight))
        return false;

    if (FAILED(d->StretchRect(target.Get(), nullptr, legacyTargets.sceneSurface.Get(), nullptr, D3DTEXF_NONE)))
        return false;

    auto& frameCtx = renderer::FrameContext::Current();
    frameCtx.sceneColor = legacyTargets.scene.Get();
    frameCtx.sceneSurface = legacyTargets.sceneSurface.Get();
    frameCtx.atmosphereNoise = resources.noise.Get();
    // The camera's own medium, settled by the water stage from the liquid draws of the frame
    // that just finished (see AdvanceUnderwater in WaterEffect.h). Published first, before any
    // pass reads it, because three of them do: the atmosphere fades its air densities out on
    // the ramp, the cloud deck stops landing on a surface seen from underneath, and the
    // underwater pass fades its own medium in on it. One number, one answer, no pass guessing.
    // Gated on the medium's own switch, so UNDERWATER=0 restores the air passes exactly as
    // they were: the tracker still settles (the water pixel stage classifies from it, and
    // that classification is a bug fix rather than a feature), but nothing fades on it.
    const bool underwaterActive = !shaftDebug && renderer::Underwater::Instance().IsActive();
    const bool confirmedPlayableWorld = frameCtx.worldScene.state==renderer::WorldSceneState::InWorld;
    frameCtx.underwaterRamp = underwaterActive && confirmedPlayableWorld
        ? watereffect::submersionTracker.ramp : 0.f;
    frameCtx.underwaterYards = watereffect::submersionTracker.yards;

    // One Capture on the shared D3DSBT_ALL stack replaces the ~170-call
    // manual save; the passes nested inside (AO, clouds, local lights)
    // run on their own stack levels, so all of them stay on the fast path.
    // A device without state blocks gets the exact legacy save.
    renderer::ScopedPassState scopedState(d);

    internal = true;
    bool ok = true;
    auto check = [&](HRESULT hr, int line = __LINE__) {
        if (FAILED(hr) && ok) {
            ok = false;
            char msg[96];
            sprintf_s(msg, sizeof(msg), "composite:FAILED line=%d hr=0x%08lX", line, static_cast<unsigned long>(hr));
            Log(msg);
        }
    };

    check(setDepth(d, nullptr));
    check(d->SetVertexShader(nullptr));
    check(d->SetPixelShader(legacyShaders.volume.Get()));
    check(d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
    check(d->SetTexture(0, nullptr));
    check(d->SetTexture(1, depth.Get()));
    check(d->SetTexture(2, resources.noise.Get()));

    for (DWORD s = 0; s < 3; ++s) {
        check(d->SetSamplerState(s, D3DSAMP_MINFILTER, s == 2 ? D3DTEXF_LINEAR : D3DTEXF_POINT));
        check(d->SetSamplerState(s, D3DSAMP_MAGFILTER, s == 2 ? D3DTEXF_LINEAR : D3DTEXF_POINT));
        check(d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        check(d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE));
        check(d->SetSamplerState(s, D3DSAMP_ADDRESSU, s == 2 ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(s, D3DSAMP_ADDRESSV, s == 2 ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP));
    }

    for (D3DRENDERSTATETYPE s : {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
        D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_LIGHTING, D3DRS_SRGBWRITEENABLE })
    {
        check(d->SetRenderState(s, FALSE));
    }
    check(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 7));
    check(d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
    check(d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0));
    check(d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD));
    check(d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE));

    struct V { float x, y, z, w, u, v; };
    auto drawQuad = [&](UINT width, UINT height) {
        float w = float(width) - .5f, h = float(height) - .5f;
        V quad[] = { {-.5f, -.5f, 0, 1, 0, 0}, {w, -.5f, 0, 1, 1, 0}, {-.5f, h, 0, 1, 0, 1}, {w, h, 0, 1, 1, 1} };
        check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V)));
    };

    D3DVIEWPORT9 fullVp{ 0, 0, desc.Width, desc.Height, 0, 1 };
    check(d->SetViewport(&fullVp));
    check(d->SetPixelShaderConstantF(0, constants[0], 14));

    // Screen-space ambient occlusion. First effect on the scene, so it
    // darkens ONLY world geometry: clouds, local light, fog and glare all
    // composite over the result afterwards and are never darkened by it.
    // The pass multiplies into `target` in place; the scene copy every
    // downstream pass samples is republished right after.
    if (!shaftDebug && renderer::AmbientOcclusion::Instance().IsActive() &&
        renderer::AmbientOcclusion::Instance().Render(d, renderer::FrameContext::Current(),
                                                      target.Get(), depth.Get())) {
        check(d->StretchRect(target.Get(), nullptr, legacyTargets.sceneSurface.Get(), nullptr, D3DTEXF_NONE));
        // AmbientOcclusion restores the state it found (its own block
        // stack level); the legacy passes below expect their own
        // bindings to be live again.
        check(d->SetPixelShader(legacyShaders.volume.Get()));
        check(d->SetTexture(0, nullptr));
        check(d->SetTexture(1, depth.Get()));
        check(d->SetTexture(2, resources.noise.Get()));
        check(d->SetPixelShaderConstantF(0, constants[0], 14));
    }

    // Volumetric cloud deck, integrated into the SCENE rather than painted
    // over the finished frame: it runs before local lighting, the
    // atmosphere and the glare pass, and refreshes the scene copy those
    // passes read. Aerial perspective, the sun halo and the god-ray source
    // mask therefore all see the clouds - a deck in front of the sun dims
    // its rays instead of glowing through them.
    frameCtx.cloudTexture = nullptr;
    frameCtx.cloudShadowTexture = nullptr;
    frameCtx.cloudRayStrength = 0.f;
    if (!shaftDebug && cloudsEffectEnabled &&
        renderer::CanRenderWorldClouds(frameCtx.worldScene.state,
            frameCtx.worldTerrainSeenThisFrame||frameCtx.worldWaterSeenThisFrame)) {
        auto& clouds = renderer::VolumetricClouds::Instance();
        clouds.Settings().weather = cloudWeather;
        // Already resolved once at the frame boundary, alongside wind/water.
        clouds.Settings().weatherStrength = cloudWeatherStrength;
        if (clouds.Render(d, frameCtx, target.Get())) {
            frameCtx.cloudTexture = clouds.GetCloudTexture();
            frameCtx.cloudShadowTexture = clouds.GetShadowTexture();
            frameCtx.cloudShadowArea = clouds.ShadowArea();
            frameCtx.cloudLightProjection = clouds.LightProjection();
            frameCtx.cloudRayStrength = shaftsEffectEnabled && clouds.Settings().sunRays
                ? clouds.Settings().sunRayStrength : 0.f;
            // Everything downstream samples legacyTargets.scene, so the
            // copy has to carry the clouds too.
            check(d->StretchRect(target.Get(), nullptr, legacyTargets.sceneSurface.Get(), nullptr, D3DTEXF_NONE));
            // VolumetricClouds restores the render target, viewport,
            // shaders and constants it found, but the legacy passes below
            // expect their own texture bindings to still be live.
            check(d->SetPixelShader(legacyShaders.volume.Get()));
            check(d->SetTexture(0, nullptr));
            check(d->SetTexture(1, depth.Get()));
            check(d->SetTexture(2, resources.noise.Get()));
            check(d->SetPixelShaderConstantF(0, constants[0], 14));
        }
    }

    // Local environment lights are accumulated before atmosphere so their
    // surface contribution is scattered by the same medium and never lands
    // on UI. Water is explicitly masked by LocalLightingRenderer.
    const bool localLightingRendered =
        renderer::LocalLightingRenderer::Instance().Render(d, frameCtx);
    // The atmosphere halo follows the same body as the glare: the moon has its
    // own glow slider, so a moonlit sky is not lit by the sun's setting.
    renderer::DirectionalVolumetricLighting::Instance().Settings().sunGlowStrength =
        float(renderer::FrameContext::Current().celestialIsMoon ? moonGlowPercent : sunGlowPercent) * .01f;
    // Dedicated low-resolution atmosphere. This replaces the old full-res
    // analytic fog/wash and owns haze, height fog, mist and celestial scatter.
    bool atmosphereRendered = false;
    const auto& atmosphereSettings = renderer::DirectionalVolumetricLighting::Instance().Settings();
    if (!shaftDebug && playableWorldEvidence &&
        (fogEffectEnabled || atmosphereSettings.localFogEnabled ||
         atmosphereSettings.edgeFogEnabled || atmosphereSettings.debugMode == renderer::VolumetricDebugMode::LocalLightScattering))
        atmosphereRendered = renderer::DirectionalVolumetricLighting::Instance().Render(
            d, frameCtx, target.Get(), fogEffectEnabled);
    if (localLightingRendered && !atmosphereRendered)
        check(d->StretchRect(frameCtx.sceneSurface, nullptr,
                             target.Get(), nullptr, D3DTEXF_NONE));

    // Secondary Sun Radial Glare pass
    if (sunGlareEnabled && shaftsEffectEnabled && (constants[2][2] > 0.001f || shaftDebug)) {
        renderer::ScopedCpuTimer glareTimer(renderer::PerfStage::SunRadialGlare);

        check(d->SetRenderTarget(0, legacyTargets.rayASurface.Get()));
        D3DVIEWPORT9 rayVp{ 0, 0, rayWidth, rayHeight, 0, 1 };
        check(d->SetViewport(&rayVp));
        check(d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
        check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        check(d->SetPixelShader(legacyShaders.volume.Get()));
        check(d->SetTexture(0, legacyTargets.scene.Get()));
        check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
        check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
        check(d->SetPixelShaderConstantF(0, constants[0], 14));
        // y is the SUN GLOW slider as a clean 0..3 scale (100% -> 1.0). Used
        // to be pre-multiplied by sunGlareStrength (0..~0.45 in practice),
        // which squashed the whole slider into a barely-perceptible range -
        // see the amount formula in volumePixelSource for the other half of
        // that fix.
        const int glowPercent = renderer::FrameContext::Current().celestialIsMoon ? moonGlowPercent : sunGlowPercent;
        float shaftMode[4] = { shaftDebug ? 7.f : 6.f, float(glowPercent) * 0.01f, shaftDebug ? 1.f : 0.f, sunSourceThreshold };
        check(d->SetPixelShaderConstantF(8, shaftMode, 1));
        check(d->SetTexture(3, frameCtx.cloudTexture));
        check(d->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
        check(d->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
        check(d->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        check(d->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(3, D3DSAMP_SRGBTEXTURE, FALSE));
        float cloudMask[4] = {frameCtx.cloudTexture ? 1.f : 0.f, 0, 0, 0};
        check(d->SetPixelShaderConstantF(14, cloudMask, 1));
        // Ray look (see rayLook in volumePixelSource): sunward glow spread,
        // glow floor, and gain. Gain 0.35 is the calibrated level of the new
        // whole-frame mask (equal to the GodRayPreview "0.45" over the old 1.28).
        // Gain 0.6 is the look calibrated in tools/GodRayPreview (0.35 at full
        // visibility) divided by the 0.6 clear-air floor applied in the composite.
        float rayLook[4] = { 6.f, 0.f, 0.6f, 0.f };
        check(d->SetPixelShaderConstantF(15, rayLook, 1));
        if (ok) drawQuad(rayWidth, rayHeight);
        check(d->SetTexture(3, nullptr));

        check(d->SetTexture(1, nullptr));
        check(d->SetTexture(2, nullptr));
        check(d->SetPixelShader(legacyShaders.radial.Get()));
        check(d->SetTexture(0, legacyTargets.rayA.Get()));
        check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
        check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
        check(d->SetRenderTarget(0, legacyTargets.rayBSurface.Get()));
        check(d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
        // Reach/decay were hardcoded (.78/.958) - ShaftFalloffPercent (the
        // "RAY LENGTH" knob, documented as "higher = shorter rays") was
        // read into rayFalloff but never actually consumed by this pass.
        // Wired up now: higher rayFalloff -> faster per-sample decay ->
        // visibly shorter rays, matching what the slider already claimed.
        // Gather the whole path to the source (GodRayCore.h RadialLook).
        // The old .88 reach stopped short of the source and left the rays
        // confined to the disc.
        float radialReach = 1.f;
        float radialDecay = pow(.975f, std::max(rayFalloff, .3f));
        float radial[4] = { constants[2][0], constants[2][1], radialReach, radialDecay };
        check(d->SetPixelShaderConstantF(0, radial, 1));
        if (ok) drawQuad(rayWidth, rayHeight);

        check(d->SetTexture(0, nullptr));
        check(d->SetPixelShader(legacyShaders.blur.Get()));
        check(d->SetRenderTarget(0, legacyTargets.rayASurface.Get()));
        check(d->SetTexture(0, legacyTargets.rayB.Get()));
        check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
        check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
        float aaRadius = .65f + raySoftness * .12f;
        check(d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
        float blur[4] = { aaRadius / rayWidth, 0, 0, 0 };
        check(d->SetPixelShaderConstantF(0, blur, 1));
        check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        if (ok) drawQuad(rayWidth, rayHeight);

        check(d->SetTexture(0, nullptr));
        check(d->SetRenderTarget(0, legacyTargets.rayBSurface.Get()));
        check(d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
        check(d->SetTexture(0, legacyTargets.rayA.Get()));
        blur[0] = 0; blur[1] = aaRadius / rayHeight;
        check(d->SetPixelShaderConstantF(0, blur, 1));
        if (ok) drawQuad(rayWidth, rayHeight);

        IDirect3DTexture9* finalRays = legacyTargets.rayB.Get();
        if (!shaftDebug && temporalShaftsEnabled && legacyTargets.rayHistory && legacyTargets.rayHistorySurface) {
            auto& tempCtx = renderer::FrameContext::Current();
            check(d->SetRenderTarget(0, legacyTargets.rayASurface.Get()));
            check(d->SetPixelShader(legacyShaders.temporal.Get()));
            check(d->SetTexture(0, legacyTargets.rayB.Get()));
            check(d->SetTexture(1, legacyTargets.rayHistory.Get()));
            check(d->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
            check(d->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
            check(d->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
            check(d->SetTexture(2, depth.Get()));
            check(d->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_POINT));
            check(d->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
            check(d->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
            float temporal[4] = { 1.f / rayWidth, 1.f / rayHeight, .72f, legacyTargets.historyValid ? 1.f : 0.f };
            check(d->SetPixelShaderConstantF(0, temporal, 1));
            // Reuse this frame's already-captured projection/inverse-view/
            // camera-position (constants[0], constants[4..7]) for world-
            // position reconstruction, plus LAST frame's raw view matrix for
            // the actual reprojection - see volumeTemporalPixelSource.
            check(d->SetPixelShaderConstantF(1, constants[0], 1));
            check(d->SetPixelShaderConstantF(2, constants[4], 4));
            float prevView[4][4];
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    prevView[row][col] = tempCtx.previousViewRaw.m[row][col];
            prevView[0][3] = tempCtx.previousViewValid ? 1.f : 0.f;
            check(d->SetPixelShaderConstantF(6, prevView[0], 4));
            const float depthRange[4]={tempCtx.viewport.MinZ,
                                       tempCtx.viewport.MaxZ-tempCtx.viewport.MinZ,
                                       tempCtx.viewport.MaxZ,0};
            check(d->SetPixelShaderConstantF(10,depthRange,1));
            if (ok) drawQuad(rayWidth, rayHeight);

            check(d->SetTexture(0, nullptr));
            check(d->SetTexture(1, nullptr));
            check(d->SetTexture(2, nullptr));
            check(d->StretchRect(legacyTargets.rayASurface.Get(), nullptr, legacyTargets.rayHistorySurface.Get(), nullptr, D3DTEXF_NONE));
            legacyTargets.historyValid = ok;
            finalRays = legacyTargets.rayA.Get();
        }

        check(d->SetTexture(0, nullptr));
        check(d->SetTexture(1, nullptr));
        check(d->SetTexture(2, nullptr));
        check(d->SetRenderTarget(0, target.Get()));
        check(d->SetViewport(&fullVp));
        check(d->SetPixelShader(legacyShaders.copy.Get()));
        check(d->SetTexture(0, finalRays));
        auto* linkedFog=atmosphereRendered?renderer::DirectionalVolumetricLighting::Instance().GetResolvedFogTexture():nullptr;
        if(legacyShaders.rayComposite){
            check(d->SetPixelShader(legacyShaders.rayComposite.Get()));
            check(d->SetTexture(1,linkedFog));
            check(d->SetSamplerState(1,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));
            check(d->SetSamplerState(1,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR));
            check(d->SetSamplerState(1,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(1,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(1,D3DSAMP_SRGBTEXTURE,FALSE));
            check(d->SetTexture(2,depth.Get()));
            check(d->SetSamplerState(2,D3DSAMP_MINFILTER,D3DTEXF_POINT));
            check(d->SetSamplerState(2,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
            check(d->SetSamplerState(2,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
            check(d->SetSamplerState(2,D3DSAMP_SRGBTEXTURE,FALSE));
            check(d->SetSamplerState(2,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(2,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
            float link[4]={linkedFog?1.f:0.f,shaftDebug?1.f:0.f,0,0};
            // y = clear-air ray floor (see rayCompositePixelSource). Rays are not
            // gated to zero in clear sky; banks and thick fog still extinguish them.
            float depthInfo[4]={constants[2][3],0.6f,0,0};
            check(d->SetPixelShaderConstantF(0,link,1));
            check(d->SetPixelShaderConstantF(1,depthInfo,1));
        }
        check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
        check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
        check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, shaftDebug ? FALSE : TRUE));
        if (!shaftDebug) {
            check(d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE));
            check(d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE));
        }
        if (ok) drawQuad(desc.Width, desc.Height);
        check(d->SetTexture(0, nullptr));
        check(d->SetTexture(1, nullptr));
        check(d->SetTexture(2, nullptr));
    }

    // The world the camera is inside. Last of the light terms and before the grade, which is
    // where the medium physically is: a diver looks at everything this compositor built - the
    // occluded world, the deck, the local lights, the air, the glare - through water, so the
    // water goes on after all of them and before the frame is graded. It reads the scene copy
    // and writes the mixed frame back into `target` with blending off, leaves the liquid's own
    // pixels to the water stage that marched that column with the same coefficient, and above
    // water it does not run at all.
    if (underwaterActive) {
        renderer::UnderwaterInputs inputs;
        inputs.medium = watereffect::underwaterFrame.medium;
        // The wave field is built HERE, for this frame context's camera - the one
        // the pass reconstructs world positions against from the captured view's
        // inverse. A field carried over from a draw or from the previous frame's
        // Present would be anchored to another camera, and the caustic net, the
        // shaft pattern and the sediment patches would crawl with every move.
        inputs.field = watereffect::UnderwaterFieldAt(frameCtx.cameraPosition.x,
            frameCtx.cameraPosition.y, frameCtx.cameraValid);
        for (int axis = 0; axis < 3; ++axis) {
            inputs.up[axis] = watereffect::underwaterFrame.up[axis];
            inputs.lightDir[axis] = watereffect::underwaterFrame.lightDir[axis];
            inputs.lightColour[axis] = watereffect::underwaterFrame.lightColour[axis];
        }
        inputs.sunElevation = watereffect::underwaterFrame.sunElevation;
        // The place's underwater defaults ride the same resolved environment the
        // medium was built from, gated by the same ZoneWaves switch that gates
        // the zone sea state: ZoneWaves=0 pins every shore to the global dials.
        if (watereffect::zoneWavesEnabled) {
            inputs.turbidityMultiplier = frameCtx.environment[renderer::UnderwaterTurbidity];
            inputs.causticMultiplier = frameCtx.environment[renderer::UnderwaterCaustics];
            inputs.shaftMultiplier = frameCtx.environment[renderer::UnderwaterShafts];
            inputs.glowMultiplier = frameCtx.environment[renderer::UnderwaterGlow];
        }
        if (renderer::Underwater::Instance().Render(d, frameCtx, target.Get(), depth.Get(), inputs))
        {
            // The pass wrote the whole frame, so the copy every downstream pass samples - the
            // grade and the anti-aliasing both read it as their scratch source - has to carry
            // the medium too.
            check(d->StretchRect(target.Get(), nullptr, legacyTargets.sceneSurface.Get(), nullptr, D3DTEXF_NONE));
        }
    }

    // Independent post pass: optional glare failures must not suppress grading. Anti-aliasing rides
    // the same pass and the same scratch copy of the backbuffer - one blit for both - but it can put
    // the pass on the screen by itself, and when it does the grade is forced to identity so that a
    // player who has the master off cannot get their brightness applied through the back door of an
    // AA pass they turned on for their edges.
    const bool gradeWanted = postProcessEffectEnabled &&
        (brightnessPercent != 0 || contrastPercent != 100 || gammaPercent != 100 || sharpnessPercent > 0);
    if (legacyShaders.postProcess && (gradeWanted || antiAliasingEnabled)) {
        renderer::ScopedCpuTimer postTimer(renderer::PerfStage::PostProcess);
        const float postParams[4]={gradeWanted?brightnessPercent*.01f:0.f,gradeWanted?contrastPercent*.01f:1.f,
            gradeWanted?gammaPercent*.01f:1.f,gradeWanted?sharpnessPercent*.01f:0.f};
        const float aaParams[4]={antiAliasingEnabled?1.f:0.f,0.f,0.f,0.f};
        const HRESULT postResult=renderer::ApplyImagePostProcess(d,target.Get(),legacyTargets.scene.Get(),legacyTargets.sceneSurface.Get(),legacyShaders.postProcess.Get(),postParams,aaParams);
        if(FAILED(postResult)){static HRESULT lastPostError=S_OK;if(lastPostError!=postResult){char msg[100];sprintf_s(msg, sizeof(msg), "PostProcess failed hr=0x%08lX",postResult);Log(msg);lastPostError=postResult;}}
    }

    // Debug: crosshair(s) at the screen position(s) various sun/moon source
    // hypotheses land on. Confirms (or disproves) that a given source
    // tracking method sits on the real sun/moon disc.
    if ((celestialMarkerDebug || celestialMemoryProbeDebug) && legacyShaders.celestialMarker) {
        check(d->SetViewport(&fullVp));
        check(d->SetPixelShader(legacyShaders.celestialMarker.Get()));
        check(d->SetTexture(0, nullptr));
        float markerSize[4] = { 1.f / desc.Width, 1.f / desc.Height, 0, 0 };
        check(d->SetPixelShaderConstantF(2, markerSize, 1));
        check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
        check(d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
        check(d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));

        auto drawMarker = [&](float x, float y, bool valid, float r, float g, float b) {
            float markerTarget[4] = { x, y, valid ? 1.f : 0.f, 0 };
            float markerColor[4] = { r, g, b, 1.f };
            check(d->SetPixelShaderConstantF(0, markerTarget, 1));
            check(d->SetPixelShaderConstantF(1, markerColor, 1));
            if (ok) drawQuad(desc.Width, desc.Height);
        };

        // RED (or the original yellow/blue when the memory probe overlay is
        // off): the current v[24]-projection marker used by rays/glare.
        bool legacyValid = constants[2][0] > -0.5f && constants[2][1] > -0.5f;
        if (celestialMemoryProbeDebug) {
            drawMarker(constants[2][0], constants[2][1], legacyValid, 1.f, .15f, .15f);
        } else {
            bool moonDominant = celestialMoonlight > celestialDaylight;
            if (moonDominant) drawMarker(constants[2][0], constants[2][1], legacyValid, .55f, .70f, 1.f);
            else drawMarker(constants[2][0], constants[2][1], legacyValid, 1.f, .85f, .25f);
        }

        // GREEN / CYAN: candidates read from CelestialMemoryProbe (see that
        // file for exactly what is and isn't verified). Projected with the
        // same true-perspective formula as the legacy path, no offset/scale.
        if (celestialMemoryProbeDebug) {
            renderer::CelestialProbeSample probe = renderer::CelestialMemoryProbe::Instance().Sample();

            auto projectAndDraw = [&](bool valid, const renderer::Vec3& toLightWorld, float r, float g, float b) {
                if (!valid) { drawMarker(-2.f, -2.f, false, r, g, b); return; }
                float vx = toLightWorld.x * constants[4][0] + toLightWorld.y * constants[5][0] + toLightWorld.z * constants[6][0];
                float vy = toLightWorld.x * constants[4][1] + toLightWorld.y * constants[5][1] + toLightWorld.z * constants[6][1];
                float vz = toLightWorld.x * constants[4][2] + toLightWorld.y * constants[5][2] + toLightWorld.z * constants[6][2];
                bool inFront = vz > 0.02f;
                float sx = -2.f, sy = -2.f;
                if (inFront) {
                    float invZ = 1.f / vz;
                    sx = 0.5f + 0.5f * vx * invZ * constants[0][2];
                    sy = 0.5f - 0.5f * vy * invZ * constants[0][3];
                }
                drawMarker(sx, sy, inFront, r, g, b);
            };

            projectAndDraw(probe.sunCandidateValid, probe.sunToLightWorld, .25f, 1.f, .35f);
            projectAndDraw(probe.moonCandidateValid, probe.moonToLightWorld, .25f, 1.f, 1.f);

            if (++celestialProbeLogCounter >= 60) {
                celestialProbeLogCounter = 0;
                char msg[512];
                if (!probe.addressesReadable) {
                    sprintf_s(msg, sizeof(msg), "celestial-probe: addresses not readable (module layout may not match)");
                } else if (!probe.valuesPlausible) {
                    sprintf_s(msg, sizeof(msg), "celestial-probe: read ok but neither candidate direction was plausible; day=%.4f", probe.day);
                } else {
                    sprintf_s(msg, sizeof(msg),
                        "celestial-probe: day=%.4f sunRaw=(%.2f %.2f %.2f) moonRaw=(%.2f %.2f %.2f) ref=(%.2f %.2f %.2f) "
                        "sunToLight=(%.3f %.3f %.3f) valid=%d moonToLight=(%.3f %.3f %.3f) valid=%d legacy_v24_screen=(%.3f %.3f)",
                        probe.day,
                        probe.sunRaw.x, probe.sunRaw.y, probe.sunRaw.z,
                        probe.moonRaw.x, probe.moonRaw.y, probe.moonRaw.z,
                        probe.referenceRaw.x, probe.referenceRaw.y, probe.referenceRaw.z,
                        probe.sunToLightWorld.x, probe.sunToLightWorld.y, probe.sunToLightWorld.z, probe.sunCandidateValid ? 1 : 0,
                        probe.moonToLightWorld.x, probe.moonToLightWorld.y, probe.moonToLightWorld.z, probe.moonCandidateValid ? 1 : 0,
                        constants[2][0], constants[2][1]);
                }
                std::ofstream(std::filesystem::path(celestialProbeLogPath), std::ios::app) << msg << '\n';
            }
        }
    }

    check(d->SetTexture(0, nullptr));
    check(d->SetTexture(1, nullptr));
    check(d->SetTexture(2, nullptr));
    internal = false;
    return ok;
}


bool PostProcessWithoutCamera(IDirect3DDevice9* d) {
    // Same two independent reasons to run the pass as the composed path above, and the same rule
    // that anti-aliasing alone is enough to put it on screen with an identity grade.
    const bool gradeWanted = postProcessEffectEnabled &&
        (brightnessPercent!=0 || contrastPercent!=100 || gammaPercent!=100 || sharpnessPercent!=0);
    if (!target || !surface || (!gradeWanted && !antiAliasingEnabled)) return false;
    ComPtr<IDirect3DSurface9> currentTarget;
    if(FAILED(d->GetRenderTarget(0,currentTarget.GetAddressOf())) || currentTarget.Get()!=target.Get())return false;
    D3DSURFACE_DESC desc{};
    if(FAILED(target->GetDesc(&desc)) || !legacyShaders.Ensure(d) ||
        !legacyTargets.Ensure(d,desc.Width,desc.Height,desc.Format,std::max<UINT>(1,desc.Width/2),std::max<UINT>(1,desc.Height/2)))return false;
    const float params[4]={gradeWanted?brightnessPercent*.01f:0.f,gradeWanted?contrastPercent*.01f:1.f,
        gradeWanted?gammaPercent*.01f:1.f,gradeWanted?sharpnessPercent*.01f:0.f};
    const float aaParams[4]={antiAliasingEnabled?1.f:0.f,0.f,0.f,0.f};
    const auto tracked=renderer::g_trackedState;
    internal=true;
    const HRESULT hr=renderer::ApplyImagePostProcess(d,target.Get(),legacyTargets.scene.Get(),legacyTargets.sceneSurface.Get(),legacyShaders.postProcess.Get(),params,aaParams);
    internal=false;
    renderer::g_trackedState=tracked;
    if(SUCCEEDED(hr)){static bool logged=false;if(!logged){Log("PostProcess applied independently of camera capture");logged=true;}return true;}
    return false;
}

void BeforeDraw(IDirect3DDevice9* d) {
    // `owner` is the device whose depth capture this frame started. A frame that
    // captured no depth at all has no owner, and refusing it here was the second half
    // of the same one-way door the Clear hook had: with no depth, no camera attempt;
    // with no camera, nothing at all. The capture attempt below needs no depth, so a
    // null owner means "this device's frame, with no capture yet" - not a foreign one.
    if (internal || composed || (owner != nullptr && owner != d) ||
        (!(enabled && active) && !watereffect::NeedsScene())) return;

    bool isUi = (renderer::g_trackedState.vsHash == 0xd9e7756460af6296ull ||
                 renderer::g_trackedState.psHash == 0xc29c7060b723c0c6ull);

    if (ready && !isUi) return;

    // Try camera capture on every draw where:
    //   - a vertex shader is bound (not fixed-function / UI pass)
    //   - the draw itself survives Capture()'s gates (full viewport, rigid basis,
    //     perspective projection) - note that a captured depth surface is NOT one
    //     of them, see below
    // No attempt-count limit: WoW's early draws (skybox, shadow, pre-pass) don't carry
    // the 27 perspective constants; world terrain arrives at draw ~20-50+ and must be reached.
    if (!ready && !isUi) {
        if (renderer::g_trackedState.currentVS != nullptr) {
            // The camera is not a depth resource: its 27 shader constants, the
            // viewport and the projection all come from the draw. This attempt used
            // to require a captured INTZ surface first - which made the frame's whole
            // camera, and with it every cloud, shaft, local light and ambient
            // occlusion in the frame, depend on a depth format the device can refuse
            // (multisampling, an unusual backbuffer depth). The depth surface is what
            // the effect passes consume, not what the camera needs, so the capture
            // gate is the draw's own: Capture() already refuses anything that is not
            // a full-viewport draw with a rigid basis and a perspective projection.
            ready = CaptureCamera(d);
            if (ready) {
                // Publish the shader only for a draw that ACTUALLY yielded
                // the camera. The hash used to be written before the
                // attempt, so a failed one - a sky, moon or portrait draw,
                // exactly the ones that fail the full-frame/rigid gates -
                // was remembered as "the terrain shader" and every later
                // WMO draw with it was classified as terrain for the rest
                // of the frame.
                cameraCaptureShaderHash = renderer::g_trackedState.vsHash;
                ++cameraFrames;
            }
        }
        if (!ready) {
            renderer::CameraCapture::Instance().ObserveSkyFallback(d);
        }
    }

    // Water can borrow depth/camera capture with the atmosphere switched off.
    // Do not accidentally enable atmosphere/postprocessing in that mode.
    if (!enabled || !active) return;
    // The camera and the scene copy are different requirements. A pose captured on a
    // frame whose depth capture was refused (multisampling, an exotic backbuffer depth)
    // has already served every pose-only pass in BeforeDraw; what it cannot do is make
    // this frame's full-screen composite, which paints onto the copy the clear hook
    // publishes. Hand the frame to the image pass instead of compositing onto nothing -
    // that pass is grade and anti-aliasing, and it is exactly what a player running
    // multisampling is relying on.
    if (!target || !depth) { composed = PostProcessWithoutCamera(d); return; }
    if (!ready && isUi && surface != nullptr) {
        ready = CaptureSkyFallback();
        if (ready) {
            ++cameraFrames;
        }
    }
    if (!ready && isUi) { composed=PostProcessWithoutCamera(d); return; }
    if (!ready || !isUi) return;

    composed = true;
    if (Composite(d)) {
        ++applied;
        renderer::RendererDiagnostics::Instance().RecordComposite();
        if (applied == 1) Log("composited before captured UI shader; height fog + screen-space shafts");
    }
    else {
        ready = false; composed = false;
        Log("composite failed; frame skipped");
    }
}

void Present(IDirect3DDevice9* d) {
    if (enabled && active && !composed && !ready && surface != nullptr) {
        ready = CaptureSkyFallback();
        if (ready) {
            ++cameraFrames;
        }
    }
    // Same split as BeforeDraw: a pose without a scene copy still cannot composite, so
    // the image pass takes the frame (and a frame with neither gets no pass at all,
    // which is what this branch already did).
    if(enabled && active && !composed && (!ready || !target || !depth))composed=PostProcessWithoutCamera(d);
    if (enabled && active && !composed && ready && target && depth) {
        composed = true;
        if (Composite(d)) {
            ++applied;
            renderer::RendererDiagnostics::Instance().RecordComposite();
        }
    }
    renderer::PerformanceProfiler::Instance().OnFrameEnd(d);
    if (enabled && active && ++frames % 600 == 120) {
        std::ofstream(std::filesystem::path(logPath), std::ios::app) << "frames=" << frames << " depth=" << depthFrames << " camera=" << cameraFrames << " applied=" << applied << " shadowDraws=" << shadowDraws << '\n';
    }
    Finish(d);
}
}
