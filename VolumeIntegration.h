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
#include "src/D3D9/ScopedRenderState.h"
#include "src/Scene/DrawCallClassifier.h"
#include "src/Effects/DirectionalVolumetricLighting.h"
#include "src/Effects/VolumetricClouds.h"
#include "src/Effects/GroundSurfaceCapture.h"
#include "src/Effects/LocalLightingRenderer.h"
#include "src/Diagnostics/CelestialMemoryProbe.h"
#include "src/D3D9/CelestialTracker.h"
#include "src/D3D9/CelestialStability.h"

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
bool fogEffectEnabled=true,shaftsEffectEnabled=true,shadowsEffectEnabled=true,shadowMapEnabled=false,cloudShadowsEffectEnabled=true,temporalShaftsEnabled=false;
bool postProcessEffectEnabled=true;
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
    ComPtr<IDirect3DPixelShader9> contactShadow;
    ComPtr<IDirect3DPixelShader9> postProcess;
    ComPtr<IDirect3DPixelShader9> rayComposite;
    ComPtr<IDirect3DPixelShader9> localLight;
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

    ComPtr<IDirect3DTexture9> localA;
    ComPtr<IDirect3DSurface9> localASurface;

    ComPtr<IDirect3DTexture9> localB;
    ComPtr<IDirect3DSurface9> localBSurface;

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

ComPtr<ID3DBlob> bytecode, blurBytecode, copyBytecode, temporalBytecode, localLightBytecode, radialBytecode, contactShadowBytecode, postProcessBytecode, rayCompositeBytecode, celestialMarkerBytecode;
float constants[14][4]{};
uint64_t cameraCaptureShaderHash = 0;
float capturedViewTranslation[3]{};
bool capturedViewValid = false;
float baseHeight = 60, density = .004f, falloff = .07f, strength = 2.6f, moonStrength = .35f, variation = 1.35f, lowLayer = .35f, raySoftness = 5.f, rayFalloff = 2.f, fogWash = .45f, sunVerticalScale = .4f, sunOffsetX = 0, sunOffsetY = 0, localLightStrength = .8f, localLightThreshold = .25f, sunSourceThreshold = .60f;
// Second, independent height-fog layer: low, dense ground mist hugging the
// terrain, on top of the broader distance haze (`density`/`falloff`/
// `baseHeight` above). Combined by summing optical depth - physically the
// right way to stack two independent scattering media along the same ray.
float groundMistHeight = 4.f, groundMistFalloff = .35f, groundMistDensity = .008f;
float contactShadowStrength = .28f, contactShadowRadius = 10.f, contactShadowMaxDistance = 6.f, directionalShadowStrength = .16f, shadowMapDistance = 140.f, shadowMapBias = .0008f, shadowSoftness = 1.6f, cloudShadowStrength = .06f, localLightRadius = 140.f;
UINT configuredShadowMapSize = 1024;
int brightnessPercent = 0, contrastPercent = 100, gammaPercent = 100, sharpnessPercent = 35;
int sunGlowPercent = 150;
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
bool localLightDebug = false;
bool celestialMarkerDebug = false;
bool celestialMemoryProbeDebug = false;
std::wstring celestialProbeLogPath;
unsigned celestialProbeLogCounter = 0;
unsigned frames = 0, applied = 0, depthFrames = 0, cameraFrames = 0, shadowDraws = 0, shadowFrameDraws = 0;
std::wstring logPath;
std::wstring mainIni, tuningIni;

void Log(const char* s) { std::ofstream(std::filesystem::path(logPath), std::ios::app) << s << '\n'; }
int ReadTuning(const wchar_t* key, int fallback, const wchar_t* section = L"Atmosphere") {
    wchar_t value[64]{};
    renderer::locationtuning::ReadString(section, key, L"", value, std::size(value), tuningIni.c_str());
    return value[0] ? int(wcstol(value, nullptr, 10)) : renderer::locationtuning::ReadInt(section, key, fallback, mainIni.c_str());
}

bool LegacyVolumeShaders::Ensure(IDirect3DDevice9* d)
{
    if (owner == d && volume && blur && copy && temporal && radial && contactShadow)
        return true;

    Reset();
    owner = d;
    if (!d) return false;

    ComPtr<ID3DBlob> errors;
    if (!bytecode) D3DCompile(volumePixelSource, strlen(volumePixelSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (!blurBytecode) D3DCompile(volumeBlurPixelSource, strlen(volumeBlurPixelSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blurBytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (!copyBytecode) D3DCompile(volumeCopyPixelSource, strlen(volumeCopyPixelSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, copyBytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (!temporalBytecode) D3DCompile(volumeTemporalPixelSource, strlen(volumeTemporalPixelSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, temporalBytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (!radialBytecode) D3DCompile(solarRadialPixelSource, strlen(solarRadialPixelSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, radialBytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (!contactShadowBytecode) D3DCompile(contactShadowPixelSource, strlen(contactShadowPixelSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, contactShadowBytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (!postProcessBytecode) D3DCompile(postProcessPixelSource, strlen(postProcessPixelSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, postProcessBytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (!rayCompositeBytecode) D3DCompile(rayCompositePixelSource, strlen(rayCompositePixelSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, rayCompositeBytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (!localLightBytecode) D3DCompile(localLightPixelSource, strlen(localLightPixelSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, localLightBytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (!celestialMarkerBytecode) D3DCompile(celestialMarkerPixelSource, strlen(celestialMarkerPixelSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, celestialMarkerBytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());

    if (!bytecode || !blurBytecode || !copyBytecode || !temporalBytecode || !radialBytecode || !contactShadowBytecode)
        return false;

    if (FAILED(d->CreatePixelShader(static_cast<DWORD*>(bytecode->GetBufferPointer()), volume.GetAddressOf())) ||
        FAILED(d->CreatePixelShader(static_cast<DWORD*>(blurBytecode->GetBufferPointer()), blur.GetAddressOf())) ||
        FAILED(d->CreatePixelShader(static_cast<DWORD*>(copyBytecode->GetBufferPointer()), copy.GetAddressOf())) ||
        FAILED(d->CreatePixelShader(static_cast<DWORD*>(temporalBytecode->GetBufferPointer()), temporal.GetAddressOf())) ||
        FAILED(d->CreatePixelShader(static_cast<DWORD*>(radialBytecode->GetBufferPointer()), radial.GetAddressOf())) ||
        FAILED(d->CreatePixelShader(static_cast<DWORD*>(contactShadowBytecode->GetBufferPointer()), contactShadow.GetAddressOf())))
        return false;

    if (postProcessBytecode) d->CreatePixelShader(static_cast<DWORD*>(postProcessBytecode->GetBufferPointer()), postProcess.GetAddressOf());
    if (rayCompositeBytecode) d->CreatePixelShader(static_cast<DWORD*>(rayCompositeBytecode->GetBufferPointer()), rayComposite.GetAddressOf());
    if (localLightBytecode) d->CreatePixelShader(static_cast<DWORD*>(localLightBytecode->GetBufferPointer()), localLight.GetAddressOf());
    if (celestialMarkerBytecode) d->CreatePixelShader(static_cast<DWORD*>(celestialMarkerBytecode->GetBufferPointer()), celestialMarker.GetAddressOf());

    return true;
}

void LegacyVolumeShaders::Reset()
{
    volume.Reset();
    blur.Reset();
    copy.Reset();
    temporal.Reset();
    radial.Reset();
    contactShadow.Reset();
    postProcess.Reset();
    rayComposite.Reset();
    localLight.Reset();
    celestialMarker.Reset();
    owner = nullptr;
}

bool LegacyFrameTargets::Ensure(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT fmt, UINT rW, UINT rH)
{
    if (owner == d && width == w && height == h && format == fmt &&
        rayWidth == rW && rayHeight == rH &&
        scene && rayA && rayB && localA && localB)
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
        FAILED(rayB->GetSurfaceLevel(0, rayBSurface.GetAddressOf())) ||
        FAILED(d->CreateTexture(rW, rH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, localA.GetAddressOf(), nullptr)) ||
        FAILED(localA->GetSurfaceLevel(0, localASurface.GetAddressOf())) ||
        FAILED(d->CreateTexture(rW, rH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, localB.GetAddressOf(), nullptr)) ||
        FAILED(localB->GetSurfaceLevel(0, localBSurface.GetAddressOf())))
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
    localASurface.Reset();
    localA.Reset();
    localBSurface.Reset();
    localB.Reset();
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
    cloudShadowsEffectEnabled = ReadTuning(L"CloudShadowsEnabled", 1) != 0;
    temporalShaftsEnabled = ReadTuning(L"TemporalShaftsEnabled", 0) != 0;
    baseHeight = float(ReadTuning(L"BaseHeight", 60));
    density = std::clamp(ReadTuning(L"DensityPermille", 4), 0, 20) * .001f;
    strength = std::clamp(ReadTuning(L"ShaftPercent", 160), 0, 500) * .01f;
    variation = std::clamp(ReadTuning(L"VariationPercent", 135), 0, 250) * .01f;
    moonStrength = std::clamp(ReadTuning(L"MoonShaftPercent", 35), 0, 100) * .01f;
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
    localLightStrength = std::clamp(ReadTuning(L"LocalLightPercent", 80), 0, 300) * .01f;
    localLightThreshold = std::clamp(ReadTuning(L"LocalLightThresholdPercent", 25), 5, 95) * .01f;
    localLightRadius = float(std::clamp(ReadTuning(L"LocalLightRadiusPixels", 140), 40, 260));
    sunSourceThreshold = std::clamp(ReadTuning(L"SunSourceThresholdPercent", 60), 10, 95) * .01f;
    contactShadowStrength = std::clamp(ReadTuning(L"ContactShadowPercent", 25), 0, 70) * .01f;
    contactShadowRadius = float(std::clamp(ReadTuning(L"ContactShadowRadiusPixels", 10), 2, 24));
    // Genuinely local: a "contact" shadow grounds objects and shades creases.
    // Long-range occlusion belongs to the light-space directional shadow map,
    // never to this screen-space depth raymarch (off-screen occluders don't
    // exist here, which is what produced ghosting/disocclusion at long range).
    contactShadowMaxDistance = float(std::clamp(ReadTuning(L"ContactShadowRangeUnits", 6), 2, 20));
    directionalShadowStrength = std::clamp(ReadTuning(L"DirectionalShadowPercent", 45), 0, 100) * .01f;
    shadowMapDistance = float(std::clamp(ReadTuning(L"ShadowMapDistance", ReadTuning(L"ShadowReach", 180)), 50, 300));
    configuredShadowMapSize = UINT(std::clamp(ReadTuning(L"ShadowMapSize", 1024), 256, 4096));
    brightnessPercent = std::clamp(ReadTuning(L"BrightnessPercent", 0, L"PostProcess"), -50, 50);
    contrastPercent = std::clamp(ReadTuning(L"ContrastPercent", 100, L"PostProcess"), 50, 180);
    gammaPercent = std::clamp(ReadTuning(L"GammaPercent", 100, L"PostProcess"), 50, 180);
    sharpnessPercent = std::clamp(ReadTuning(L"SharpnessPercent", 35, L"PostProcess"), 0, 100);
    postProcessEffectEnabled = ReadTuning(L"Enabled", 1, L"PostProcess") != 0;
    {char message[160];sprintf_s(message,"PostProcess effective enabled=%d brightness=%d contrast=%d gamma=%d sharpness=%d",postProcessEffectEnabled,brightnessPercent,contrastPercent,gammaPercent,sharpnessPercent);if(!logPath.empty())Log(message);}
    sunGlowPercent = std::clamp(ReadTuning(L"SunGlowPercent", 80), 0, 300);
    sunGlareEnabled = ReadTuning(L"SunGlareEnabled", 1) != 0;
    sunGlareStrength = float(ReadTuning(L"SunGlareStrengthPercent", 15)) * 0.01f;
    shaftDebug = ReadTuning(L"ShaftDebugMask", 0) != 0;
    localLightDebug = ReadTuning(L"LocalLightDebugMask", 0) != 0;
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
    enabled = renderer::locationtuning::ReadInt(L"Volume", L"Enabled", 0, mainIni.c_str()) != 0;
    ReloadTuning();
}

// Frame-scoped DEFAULT resources: persistent textures STAY ALIVE across frames!
void Finish(IDirect3DDevice9* d) {
    if (owner && owner != d) return;
    renderer::DepthCapture::Instance().OnFrameEnd(d);
    surface.Reset(); depth.Reset(); originalDepth.Reset(); target.Reset();
    owner = nullptr; ready = false; composed = false;
    watereffect::waterTargetSurface = watereffect::waterMaskSurface = watereffect::waterDepthSurface = nullptr;
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

inline bool HasActiveEffects() {
    bool hasContact = (!shaftDebug && shadowsEffectEnabled && contactShadowStrength > 0);
    bool hasFog = (!shaftDebug && (fogEffectEnabled ||
        (renderer::DirectionalVolumetricLighting::Instance().Settings().localFogEnabled || renderer::DirectionalVolumetricLighting::Instance().Settings().edgeFogEnabled)));
    bool hasVolumetric = shaftsEffectEnabled && renderer::DirectionalVolumetricLighting::Instance().Settings().enabled;
    bool hasGlare = (sunGlareEnabled && shaftsEffectEnabled && (constants[2][2] > 0.001f || shaftDebug));
    bool hasClouds = (!shaftDebug && cloudsEffectEnabled && renderer::VolumetricClouds::Instance().Settings().enabled);
    bool hasPost = postProcessEffectEnabled &&
        (brightnessPercent != 0 || contrastPercent != 100 || gammaPercent != 100 || sharpnessPercent > 0);
    return hasContact || hasFog || hasVolumetric || hasGlare || hasClouds || hasPost;
}

void BeforeClear(IDirect3DDevice9* d, DWORD count, const D3DRECT* rects, DWORD flags, float z) {
    if (internal || (!(enabled && active) && !watereffect::NeedsScene())) return;
    if (!renderer::DepthCapture::Instance().BeforeClear(d, count, rects, flags, z)) return;
    renderer::PerformanceProfiler::Instance().OnFrameBegin(d);
    renderer::RendererDiagnostics::Instance().OnFrameBegin();
    depth = renderer::DepthCapture::Instance().GetDepthTexture();
    surface = renderer::DepthCapture::Instance().GetDepthSurface();
    originalDepth = renderer::DepthCapture::Instance().GetOriginalDepth();
    target = renderer::DepthCapture::Instance().GetRenderTarget();
    owner = renderer::DepthCapture::Instance().GetOwner();
    depthFrames = renderer::DepthCapture::Instance().GetDepthFrames();
    if (renderer::DepthCapture::Instance().HasDepth()) {
        auto& frameCtx = renderer::FrameContext::Current();
        ++frameCtx.frameIndex;
        frameCtx.device = d;
        shadowFrameStarted = false; shadowFrameValid = false; shadowFrameDraws = 0;
        renderer::CelestialTracker::Instance().BeginFrame(frameCtx.frameIndex);

        // Per-frame water mask: watereffect::Scope binds this as a second
        // render target during every actual water draw (see WaterEffect.h)
        // and marks its pixels, blended by the GPU exactly like water's own
        // colour output. The atmosphere composite reads it back to exclude
        // fog from water entirely - the low-res, depth-aware fog upsample
        // was producing a flat, wrong, hard-edged patch of fog colour on
        // open water (confirmed via AtmosphereDebugMode=9), because water's
        // constantly animated/refracting/reflecting surface makes that
        // depth-based reconstruction unreliable right at the water plane.
        // Sized to the real render target, cleared fresh every frame before
        // any water draws happen.
        watereffect::waterTargetSurface = watereffect::waterMaskSurface = watereffect::waterDepthSurface = nullptr;
        frameCtx.waterMaskTexture = frameCtx.waterDepthTexture = nullptr;
        D3DSURFACE_DESC targetDesc{};
        if (target && SUCCEEDED(target->GetDesc(&targetDesc))) {
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
            if (resources.waterMaskSurface && resources.waterDepthSurface &&
                SUCCEEDED(d->ColorFill(resources.waterMaskSurface.Get(), nullptr, 0)) &&
                SUCCEEDED(d->ColorFill(resources.waterDepthSurface.Get(), nullptr, 0))) {
                watereffect::waterTargetSurface = target.Get();
                watereffect::waterMaskSurface = resources.waterMaskSurface.Get();
                watereffect::waterDepthSurface = resources.waterDepthSurface.Get();
                frameCtx.waterMaskTexture = resources.waterMask.Get();
                frameCtx.waterDepthTexture = resources.waterDepth.Get();
            }
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
        d, frameCtx, cfg, constants, capturedViewTranslation, capturedViewValid, cameraCaptureShaderHash);
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
        BuildShadowCamera();
        shadowFrameStarted = true;
        internal = true;
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
        internal = false;
        shadowFrameValid = true;
    }
    if (!shadowFrameValid) return;

    D3DVIEWPORT9 oldVp{}; d->GetViewport(&oldVp);
    DWORD colorWrite = 0, zEnable = 0, zWrite = 0, zFunc = 0, fog = 0;
    DWORD alphaBlend = 0, alphaTest = 0, oldDepthBias = 0, oldSlopeBias = 0, oldCull = 0;
    d->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
    d->GetRenderState(D3DRS_ZENABLE, &zEnable);
    d->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
    d->GetRenderState(D3DRS_ZFUNC, &zFunc);
    d->GetRenderState(D3DRS_FOGENABLE, &fog);
    d->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
    d->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
    d->GetRenderState(D3DRS_DEPTHBIAS, &oldDepthBias);
    d->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &oldSlopeBias);
    d->GetRenderState(D3DRS_CULLMODE, &oldCull);

    ComPtr<IDirect3DPixelShader9> originalPixelShader;
    d->GetPixelShader(originalPixelShader.GetAddressOf());

    float originalConsts[8][4]{};
    d->GetVertexShaderConstantF(0, originalConsts[0], 8);

    internal = true;
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

    d->SetPixelShader(originalPixelShader.Get());
    d->SetVertexShaderConstantF(0, originalConsts[0], 8);
    setDepth(d, nullptr);
    d->SetRenderTarget(0, target.Get());
    setDepth(d, surface.Get());
    d->SetViewport(&oldVp);
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
    internal = false;
}

bool Composite(IDirect3DDevice9* d) {
    if (!HasActiveEffects()) return true;

    renderer::ScopedCpuTimer totalTimer(renderer::PerfStage::TotalInjectedFrame);
    renderer::ScopedCpuTimer compTimer(renderer::PerfStage::LegacyComposite);

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
        const renderer::CelestialBody* body = useSun ? &sun : (useMoon ? &moon : nullptr);
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
        if (body) {
            frameCtx.sunScreenX = body->screenX;
            frameCtx.sunScreenY = body->screenY;
            frameCtx.sunStrength = constants[2][2];
            frameCtx.sunDirectionView = body->viewSpaceDirection;
            frameCtx.sunDirectionWorld = body->worldSpaceDirection;
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

        // Stable direct light, separate from the disc signal above. The
        // disc fields keep answering "is there a god-ray origin on screen
        // right now" (constants[2]/sunScreen/celestialIntensity), which is
        // genuinely a screen-space question. Cloud and atmosphere lighting
        // must instead answer "is the sun/moon up", which does not change
        // when the camera pans the disc out of frame, when WoW culls its
        // billboard at the screen edge, or when our own cloud deck crosses
        // it - all three used to toggle celestialIntensity between frames
        // and blink the whole sky (clouds lit/unlit, ground shadowed/
        // unshadowed). Stabilize the world direction too: retain the
        // confirmed body's direction when its billboard is culled instead
        // of switching back to potentially camera-dependent v[24] data.
        // Only use that fallback until a body has actually been tracked.
        {
            const float lightDt = celestialLightClock.Observe(
                renderer::PerformanceCounter(), renderer::PerformanceFrequency());
            renderer::CelestialLightInput lightInput;
            lightInput.discVisible = body != nullptr;
            lightInput.discIsMoon = useMoon;
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
            const renderer::Vec3* confirmed = body && useMoon == stable.isMoon
                ? &body->worldSpaceDirection : nullptr;
            frameCtx.directLightDirectionWorld = celestialDirectionStabilizer.Update(
                frameCtx.sunDirectionWorld, confirmed, stable.isMoon, lightDt);
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

    // Lightweight scoped render state backup (NO D3DSBT_ALL)
    renderer::ScopedRenderState scopedState(d);

    internal = true;
    bool ok = true;
    auto check = [&](HRESULT hr, int line = __LINE__) {
        if (FAILED(hr) && ok) {
            ok = false;
            char msg[96];
            sprintf_s(msg, "composite:FAILED line=%d hr=0x%08lX", line, static_cast<unsigned long>(hr));
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

    // Screen-space contact shadows.
    // Rendered DIRECTLY onto the bound scene target (`target`, already the
    // active render target here) with a ZERO/SRCCOLOR modulate blend - no
    // intermediate render target at all. The previous version wrote this
    // pass into `legacyTargets.rayA`, a texture allocated at HALF resolution
    // (rayWidth/rayHeight), while setting a FULL-resolution viewport and
    // drawing a full-resolution quad into it: the rasterizer clips to the
    // actual half-size surface, so only a quarter of the intended shadow
    // data was ever written, then sampled back over the full screen. That
    // size mismatch - not the blur - is what produced the large moving
    // ghost/projection artifacts. Writing straight to the real full-res
    // target removes the mismatch entirely, and also removes two full-screen
    // passes (the old copy+blend) that this used to cost.
    // The trace range is also now genuinely local: this is a *contact*
    // shadow (grounding/creases), not a stand-in for directional shadows -
    // long-range occlusion belongs to the light-space shadow map below.
    // SAFETY GATE - disabled from production regardless of ini. In-game
    // testing reported the shadow crawling/changing shape with camera
    // movement and jagged silhouettes even after the render-target fix.
    // Reconstructing anything beyond a small ground-contact shadow from the
    // screen-space depth buffer is the wrong tool (no off-screen occluder
    // data), and this must not stand in for real directional shadows. Kept
    // as diagnostic-only until re-scoped to ~0.5-2 unit contact range per
    // the Phase 5 plan, layered on top of WoW's native shadows rather than
    // replacing them.
    constexpr bool kAllowScreenSpaceContactShadow = false;
    if (kAllowScreenSpaceContactShadow && !shaftDebug && shadowsEffectEnabled && contactShadowStrength > 0) {
        renderer::ScopedCpuTimer contactTimer(renderer::PerfStage::ContactShadows);
        check(d->SetPixelShader(legacyShaders.contactShadow.Get()));
        check(d->SetTexture(0, depth.Get()));
        check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
        check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
        float shadowTuning[4] = { 1.f / desc.Width, 1.f / desc.Height, contactShadowStrength,
                                  contactShadowMaxDistance };
        float contactShadowProjection[4] = { constants[0][0], constants[0][1], constants[0][2], constants[0][3] };
        float contactShadowLight[4] = { constants[10][0], constants[10][1], constants[10][2], constants[2][3] };
        check(d->SetPixelShaderConstantF(0, shadowTuning, 1));
        check(d->SetPixelShaderConstantF(1, contactShadowProjection, 1));
        check(d->SetPixelShaderConstantF(2, contactShadowLight, 1));
        check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
        check(d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO));
        check(d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR));
        if (ok) drawQuad(desc.Width, desc.Height);

        check(d->SetTexture(0, nullptr));
        check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        // This pass wrote straight into `target`, so the scene copy the
        // passes below sample is now stale. The cloud composite re-publishes
        // that copy across the whole screen, so refreshing it here is what
        // keeps a re-enabled contact shadow from being erased again.
        check(d->StretchRect(target.Get(), nullptr, legacyTargets.sceneSurface.Get(), nullptr, D3DTEXF_NONE));
        check(d->SetPixelShader(legacyShaders.volume.Get()));
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
    if (!shaftDebug && cloudsEffectEnabled) {
        auto& clouds = renderer::VolumetricClouds::Instance();
        clouds.Settings().weather = cloudWeather;
        clouds.Settings().weatherStrength =
            std::clamp(cloudWeatherStrength * frameCtx.environment[renderer::Weather], 0.f, 1.f);
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
    // Dedicated low-resolution atmosphere. This replaces the old full-res
    // analytic fog/wash and owns haze, height fog, mist and celestial scatter.
    bool atmosphereRendered = false;
    if (!shaftDebug && (fogEffectEnabled ||
        (renderer::DirectionalVolumetricLighting::Instance().Settings().localFogEnabled || renderer::DirectionalVolumetricLighting::Instance().Settings().edgeFogEnabled)))
        atmosphereRendered = renderer::DirectionalVolumetricLighting::Instance().Render(
            d, renderer::FrameContext::Current(), target.Get(), fogEffectEnabled);
    if (localLightingRendered && !atmosphereRendered)
        check(d->StretchRect(renderer::LocalLightingRenderer::Instance().GetLitSurface(), nullptr,
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
        float shaftMode[4] = { shaftDebug ? 7.f : 6.f, float(sunGlowPercent) * 0.01f, shaftDebug ? 1.f : 0.f, sunSourceThreshold };
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
        float radialReach = .88f;
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
            float depthInfo[4]={constants[2][3],0,0,0};
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

    // Independent post pass: optional glare failures must not suppress grading.
    if (postProcessEffectEnabled && legacyShaders.postProcess &&
        (brightnessPercent != 0 || contrastPercent != 100 || gammaPercent != 100 || sharpnessPercent > 0)) {
        renderer::ScopedCpuTimer postTimer(renderer::PerfStage::PostProcess);
        const float postParams[4]={brightnessPercent*.01f,contrastPercent*.01f,gammaPercent*.01f,sharpnessPercent*.01f};
        const HRESULT postResult=renderer::ApplyImagePostProcess(d,target.Get(),legacyTargets.scene.Get(),legacyTargets.sceneSurface.Get(),legacyShaders.postProcess.Get(),postParams);
        if(FAILED(postResult)){static HRESULT lastPostError=S_OK;if(lastPostError!=postResult){char msg[100];sprintf_s(msg,"PostProcess failed hr=0x%08lX",postResult);Log(msg);lastPostError=postResult;}}
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
                    sprintf_s(msg, "celestial-probe: addresses not readable (module layout may not match)");
                } else if (!probe.valuesPlausible) {
                    sprintf_s(msg, "celestial-probe: read ok but neither candidate direction was plausible; day=%.4f", probe.day);
                } else {
                    sprintf_s(msg,
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

bool CompositeLateLocalLights(IDirect3DDevice9* d) {
    if (!enabled || !active || internal || owner != d || !composed || localLightStrength <= 0 ||
        !target || !depth || !legacyTargets.sceneSurface || !legacyTargets.localASurface || !legacyTargets.localBSurface)
        return false;

    if (!legacyShaders.Ensure(d)) return false;

    D3DSURFACE_DESC desc{}, localDesc{};
    if (FAILED(target->GetDesc(&desc)) || FAILED(legacyTargets.localASurface->GetDesc(&localDesc)) ||
        FAILED(d->StretchRect(target.Get(), nullptr, legacyTargets.sceneSurface.Get(), nullptr, D3DTEXF_NONE)))
        return false;

    renderer::ScopedRenderState scopedState(d);
    internal = true;
    bool ok = true;
    auto check = [&](HRESULT hr, int line = __LINE__) {
        if (FAILED(hr) && ok) {
            ok = false;
            char msg[96];
            sprintf_s(msg, "localLights:FAILED line=%d hr=0x%08lX", line, static_cast<unsigned long>(hr));
            Log(msg);
        }
    };

    check(setDepth(d, nullptr));
    check(d->SetVertexShader(nullptr));
    check(d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
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

    for (DWORD s = 0; s < 2; ++s) {
        check(d->SetSamplerState(s, D3DSAMP_MINFILTER, s ? D3DTEXF_POINT : D3DTEXF_LINEAR));
        check(d->SetSamplerState(s, D3DSAMP_MAGFILTER, s ? D3DTEXF_POINT : D3DTEXF_LINEAR));
        check(d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        check(d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE));
        check(d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    }

    struct V { float x, y, z, w, u, v; };
    auto drawQuad = [&](UINT width, UINT height) {
        float w = float(width) - .5f, h = float(height) - .5f;
        V q[] = { {-.5f, -.5f, 0, 1, 0, 0}, {w, -.5f, 0, 1, 1, 0}, {-.5f, h, 0, 1, 0, 1}, {w, h, 0, 1, 1, 1} };
        check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
    };

    D3DVIEWPORT9 localVp{ 0, 0, localDesc.Width, localDesc.Height, 0, 1 };
    check(d->SetViewport(&localVp));
    check(d->SetRenderTarget(0, legacyTargets.localASurface.Get()));
    check(d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
    check(d->SetPixelShader(legacyShaders.localLight.Get()));
    check(d->SetTexture(0, legacyTargets.scene.Get()));
    check(d->SetTexture(1, depth.Get()));
    check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));

    float local[4] = { 1.f / desc.Width, 1.f / desc.Height, localLightStrength, localLightThreshold };
    float localMode[4] = { localLightDebug ? 1.f : 0.f, localLightRadius, 0, 0 };
    float localDepth[4] = { constants[2][3], 0, 0, 0 };
    check(d->SetPixelShaderConstantF(0, local, 1));
    check(d->SetPixelShaderConstantF(1, localMode, 1));
    check(d->SetPixelShaderConstantF(2, localDepth, 1));
    if (ok) drawQuad(localDesc.Width, localDesc.Height);

    check(d->SetTexture(0, nullptr));
    check(d->SetTexture(1, nullptr));
    check(d->SetPixelShader(legacyShaders.blur.Get()));
    float localBlur[4] = {};
    for (int pass = 0; pass < 3; ++pass) {
        check(d->SetTexture(0, legacyTargets.localA.Get()));
        check(d->SetRenderTarget(0, legacyTargets.localBSurface.Get()));
        check(d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
        localBlur[0] = localLightRadius / (16.f * desc.Width); localBlur[1] = 0;
        check(d->SetPixelShaderConstantF(0, localBlur, 1));
        if (ok) drawQuad(localDesc.Width, localDesc.Height);
        check(d->SetTexture(0, nullptr));
        check(d->SetTexture(0, legacyTargets.localB.Get()));
        check(d->SetRenderTarget(0, legacyTargets.localASurface.Get()));
        check(d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
        localBlur[0] = 0; localBlur[1] = localLightRadius / (16.f * desc.Height);
        check(d->SetPixelShaderConstantF(0, localBlur, 1));
        if (ok) drawQuad(localDesc.Width, localDesc.Height);
        check(d->SetTexture(0, nullptr));
    }

    D3DVIEWPORT9 fullVp{ 0, 0, desc.Width, desc.Height, 0, 1 };
    check(d->SetViewport(&fullVp));
    check(d->SetRenderTarget(0, target.Get()));
    check(d->SetPixelShader(legacyShaders.copy.Get()));
    check(d->SetTexture(0, legacyTargets.localA.Get()));
    check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
    check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
    check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
    check(d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE));
    check(d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE));
    if (ok) drawQuad(desc.Width, desc.Height);

    check(d->SetTexture(0, nullptr));
    check(d->SetTexture(1, nullptr));
    internal = false;
    return ok;
}

bool PostProcessWithoutCamera(IDirect3DDevice9* d) {
    if (!target || !surface || !postProcessEffectEnabled ||
        (brightnessPercent==0 && contrastPercent==100 && gammaPercent==100 && sharpnessPercent==0)) return false;
    ComPtr<IDirect3DSurface9> currentTarget;
    if(FAILED(d->GetRenderTarget(0,currentTarget.GetAddressOf())) || currentTarget.Get()!=target.Get())return false;
    D3DSURFACE_DESC desc{};
    if(FAILED(target->GetDesc(&desc)) || !legacyShaders.Ensure(d) ||
        !legacyTargets.Ensure(d,desc.Width,desc.Height,desc.Format,std::max<UINT>(1,desc.Width/2),std::max<UINT>(1,desc.Height/2)))return false;
    const float params[4]={brightnessPercent*.01f,contrastPercent*.01f,gammaPercent*.01f,sharpnessPercent*.01f};
    const auto tracked=renderer::g_trackedState;
    internal=true;
    const HRESULT hr=renderer::ApplyImagePostProcess(d,target.Get(),legacyTargets.scene.Get(),legacyTargets.sceneSurface.Get(),legacyShaders.postProcess.Get(),params);
    internal=false;
    renderer::g_trackedState=tracked;
    if(SUCCEEDED(hr)){static bool logged=false;if(!logged){Log("PostProcess applied independently of camera capture");logged=true;}return true;}
    return false;
}

void BeforeDraw(IDirect3DDevice9* d) {
    if (internal || owner != d || composed || (!(enabled && active) && !watereffect::NeedsScene())) return;

    bool isUi = (renderer::g_trackedState.vsHash == 0xd9e7756460af6296ull ||
                 renderer::g_trackedState.psHash == 0xc29c7060b723c0c6ull);

    if (ready && !isUi) return;

    // Try camera capture on every draw where:
    //   - depth capture is active this frame (surface != nullptr)
    //   - a vertex shader is bound (not fixed-function / UI pass)
    //   - current depth buffer matches our INTZ surface
    // No attempt-count limit: WoW's early draws (skybox, shadow, pre-pass) don't carry
    // the 27 perspective constants; world terrain arrives at draw ~20-50+ and must be reached.
    if (!ready && surface != nullptr && renderer::g_trackedState.currentVS != nullptr) {
        ComPtr<IDirect3DSurface9> ds;
        if (SUCCEEDED(getDepth(d, ds.GetAddressOf())) && ds.Get() == surface.Get()) {
            cameraCaptureShaderHash = renderer::g_trackedState.vsHash;
            ready = CaptureCamera(d);
            if (ready) {
                ++cameraFrames;
            }
        }
    }

    // Water can borrow depth/camera capture with the atmosphere switched off.
    // Do not accidentally enable atmosphere/postprocessing in that mode.
    if (!enabled || !active) return;
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
    if(enabled && active && !composed && !ready)composed=PostProcessWithoutCamera(d);
    if (enabled && active && !composed && ready) {
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
