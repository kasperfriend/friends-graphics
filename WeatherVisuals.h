#include "src/Environment/LocationTuning.h"
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>

#include "src/D3D9/DepthCapture.h"
#include "src/D3D9/CameraCapture.h"
#include "src/Materials/WicTextureLoader.h"
#include "src/Materials/TextureHashLookup.h"
#include "src/D3D9/CelestialTracker.h"
#include "src/D3D9/ScopedRenderState.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace weathervisuals {
using Microsoft::WRL::ComPtr;

inline bool enabled = true;
inline bool active = true;
inline bool effectEnabled = true;
inline bool hotkey = true;
inline bool keyDown = false;
inline bool showStatus = false;
inline bool internal = false;

// Configuration
inline int mode = 0; // 0=Auto/native, 1=Rain texture hint, 2=Snow texture hint
inline float intensity = 1.0f;
inline float speed = 1.2f;
inline float windStrength = 0.35f;
inline float windAngle = 15.0f;
inline bool depthOcclusion = true;
inline bool lensDroplets = true;
inline float lensStrength = 0.60f;
inline bool surfaceSplashes = true;
inline float atmosphereHaze = 1.20f;

inline std::wstring mainIni, tuningIni, logPath, basePath;
inline IDirect3DDevice9* currentDevice = nullptr;

// GPU Resources
inline ComPtr<IDirect3DPixelShader9> precipitationPS;
inline ComPtr<IDirect3DPixelShader9> lensDropletsPS;
inline ComPtr<IDirect3DPixelShader9> nativeParticlePS;
inline ComPtr<IDirect3DTexture9> rainDropTex;
inline ComPtr<IDirect3DTexture9> splashTex;
inline ComPtr<IDirect3DTexture9> snowFlakeTex;
inline ComPtr<IDirect3DTexture9> snowMistTex;
inline ComPtr<IDirect3DTexture9> sandParticleTex;
inline ComPtr<IDirect3DTexture9> neutralDepthTex;
inline ComPtr<IDirect3DTexture9> sceneCopyTex;
inline ComPtr<IDirect3DSurface9> sceneCopySurface;
inline UINT copyWidth = 0, copyHeight = 0;

inline DWORD lastTick = 0;
inline float elapsedTime = 0.0f;
inline float smoothedIndoor = 0.0f;
inline bool worldGeometrySeenThisFrame = false;
inline int nativeWeatherSeenThisFrame = 0;
inline int nativeWeatherMode = 0;
inline UINT nativeWeatherPrimitives = 0;
inline DWORD lastNativeWeatherTick = 0;
inline bool composedThisFrame = false;
inline bool weatherSuspended = false;
inline constexpr DWORD nativeWeatherHoldMs = 5000;
inline std::unordered_set<uint64_t> loggedParticleCandidates;
inline unsigned loggedParticleCandidateCount = 0;

inline const char* precipitationHLSL = R"HLSL(
sampler2D sceneTex : register(s0);
sampler2D depthMap : register(s1);
sampler2D weatherTex : register(s2);
sampler2D mistTex : register(s3);

// c0: x=projA, y=projB, z=scaleX, w=scaleY
float4 projection : register(c0);
// c1: x=time, y=intensity, z=mode (1=Rain, 2=Snow, 3=Sandstorm), w=speed
float4 weatherParams : register(c1);
// c2: x=windX, y=windY, z=maxDist, w=indoorFactor
float4 windParams : register(c2);
// c3: x=resX, y=resY, z=invResX, w=invResY
float4 screenParams : register(c3);
// c4: light color / tint (rgb), w=depthOcclusionEnabled
float4 lightColor : register(c4);

float GetLinearDepth(float raw)
{
    float d = clamp(raw, 0.00001, 0.99999);
    return projection.y / (d - projection.x);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float rawDepth = tex2D(depthMap, uv).r;
    float sceneZ = GetLinearDepth(rawDepth);
    float4 baseColor = tex2D(sceneTex, uv);
    
    float intensity = weatherParams.y * (1.0 - windParams.w * 0.96);
    if (intensity <= 0.001)
        return baseColor;
        
    float t = weatherParams.x * weatherParams.w;
    float2 aspect = float2(screenParams.x * screenParams.w, 1.0);
    float3 accum = float3(0, 0, 0);
    float totalAlpha = 0.0;
    
    // Multi-layer depth-tested precipitation
    float layerDepths[3] = { 3.5, 11.0, 26.0 };
    float layerScales[3] = { 1.6, 3.8, 8.5 };
    float layerSpeeds[3] = { 1.25, 1.0, 0.75 };
    float layerWeights[3] = { 0.45, 0.35, 0.20 };
    
    float isSnow = step(1.5, weatherParams.z) * step(weatherParams.z, 2.5);
    float isSand = step(2.5, weatherParams.z);
    
    [unroll] for (int i = 0; i < 3; ++i)
    {
        float lz = layerDepths[i];
        // Depth occlusion: if scene is closer than this layer, precipitation behind is occluded
        float occlusion = (lightColor.w > 0.5) ? smoothstep(lz * 0.75, lz * 1.15, sceneZ) : 1.0;
        if (occlusion <= 0.001) continue;
        
        float2 windOffset = float2(windParams.x, windParams.y) * (t * layerSpeeds[i]);
        float2 layerUV = uv * aspect * layerScales[i];
        
        if (isSnow > 0.5)
        {
            // Snow: sinusoidal drift
            layerUV.x += sin(t * 1.6 + layerUV.y * 3.5) * 0.12 + windOffset.x * 0.5;
            layerUV.y += t * layerSpeeds[i] * 0.6;
            float4 snowSamp = tex2D(weatherTex, frac(layerUV));
            float flake = snowSamp.a * snowSamp.r;
            accum += float3(0.92, 0.95, 1.0) * flake * layerWeights[i] * occlusion;
            totalAlpha += flake * layerWeights[i] * occlusion;
        }
        else if (isSand > 0.5)
        {
            // Sandstorm: rapid dusty drift
            layerUV.x += windOffset.x * 3.0 + t * 2.0;
            layerUV.y += sin(layerUV.x * 2.0) * 0.05 + windOffset.y;
            float4 sandSamp = tex2D(mistTex, frac(layerUV * 0.5));
            float dust = sandSamp.a * 0.6;
            accum += float3(0.85, 0.75, 0.55) * dust * layerWeights[i] * occlusion;
            totalAlpha += dust * layerWeights[i] * occlusion;
        }
        else
        {
            // Rain: vertical streaks along wind vector
            layerUV.x += windOffset.x;
            layerUV.y += t * layerSpeeds[i] * 4.5 + windOffset.y;
            float2 streakUV = float2(layerUV.x, layerUV.y * 0.22);
            float4 dropSamp = tex2D(weatherTex, frac(streakUV));
            // The extracted Classic texture deliberately stores very low
            // alpha (its maximum is about 0.15). Bring it into a useful
            // screen-space range before applying the user's intensity.
            float drop = saturate(dropSamp.a * 4.5);
            accum += float3(0.75, 0.82, 0.92) * drop * layerWeights[i] * occlusion;
            totalAlpha += drop * layerWeights[i] * occlusion;
        }
    }
    
    // Add subtle ambient mist during heavy rain/snow at mid-distances
    if (isSand < 0.5 && sceneZ > 6.0)
    {
        float2 mistUV = uv * aspect * 1.5 + float2(windParams.x * 0.2, t * 0.15);
        float mistSamp = tex2D(mistTex, frac(mistUV)).a;
        float mistDepthFade = smoothstep(6.0, 40.0, sceneZ);
        float mistAmount = mistSamp * mistDepthFade * 0.20 * intensity * windParams.z;
        accum += float3(0.8, 0.85, 0.9) * mistAmount;
        totalAlpha += mistAmount;
    }
    
    accum *= intensity;
    totalAlpha = saturate(totalAlpha * intensity * 1.4);
    
    // Lighting modulation
    float3 litColor = accum * (lightColor.rgb * 0.65 + 0.35);
    
    return float4(baseColor.rgb * (1.0 - totalAlpha * 0.35) + litColor, baseColor.a);
}
)HLSL";

inline const char* lensDropletsHLSL = R"HLSL(
sampler2D sceneTex : register(s0);
sampler2D depthMap : register(s1);

// c0: x=time, y=strength, z=indoorFactor, w=unused
float4 params : register(c0);
// c1: x=resX, y=resY, z=invResX, w=invResY
float4 screenParams : register(c1);

float Hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 col = tex2D(sceneTex, uv);
    float strength = params.y * (1.0 - params.z);
    if (strength <= 0.001)
        return col;

    float t = params.x * 0.22;
    float2 aspect = float2(screenParams.x * screenParams.w, 1.0);
    float2 gridUV = uv * aspect * 13.0;
    float2 id = floor(gridUV);
    float2 gv = frac(gridUV) - 0.5;

    float n = Hash21(id);
    float dropTime = frac(t + n);
    float yOffset = dropTime * dropTime * 1.6;
    float2 dropPos = float2((Hash21(id + 1.23) - 0.5) * 0.55, -0.45 + yOffset);
    
    float d = length(gv - dropPos);
    float radius = 0.075 * (0.8 + 0.4 * sin(n * 6.28));
    
    float dropMask = smoothstep(radius, radius * 0.35, d);
    if (dropMask > 0.01)
    {
        float2 normal = normalize(gv - dropPos) * dropMask;
        float2 distortUV = uv - normal * 0.022 * strength;
        float4 refracted = tex2D(sceneTex, distortUV);
        float highlight = pow(saturate(dot(normal, float2(0.5, -0.7))), 4.0) * 0.4;
        return refracted + highlight * strength;
    }

    return col;
}
)HLSL";

inline const char* nativeParticleHLSL = R"HLSL(
sampler2D particleTex : register(s0);
// rgb = precipitation tint, a = alpha multiplier
float4 particleParams : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 vertexColor : COLOR0) : COLOR0
{
    float4 particle = tex2D(particleTex, uv);
    // Retain some native lighting without inheriting strong green/purple zone
    // tints that make rain look radioactive.
    // Native rain colour contains a strong zone tint. Preserve only its
    // luminance, otherwise neutral drops turn cyan/green in rainy zones.
    float nativeLuma = dot(vertexColor.rgb, float3(0.299, 0.587, 0.114));
    float3 lighting = lerp(float3(1.0, 1.0, 1.0), nativeLuma.xxx, 0.22);
    float alpha = saturate(particle.a * vertexColor.a * particleParams.a);
    return float4(particle.rgb * particleParams.rgb * lighting, alpha);
}
)HLSL";

inline void Log(const char* s)
{
    if (!logPath.empty())
        std::ofstream(std::filesystem::path(logPath), std::ios::app) << s << '\n';
}

inline int ReadTuningInt(const wchar_t* key, int fallback)
{
    wchar_t value[64]{};
    renderer::locationtuning::ReadString(L"WeatherVisuals", key, L"", value, std::size(value), tuningIni.c_str());
    return value[0] ? int(wcstol(value, nullptr, 10)) : renderer::locationtuning::ReadInt(L"WeatherVisuals", key, fallback, mainIni.c_str());
}

inline float ReadTuningFloat(const wchar_t* key, float fallback)
{
    wchar_t value[64]{};
    renderer::locationtuning::ReadString(L"WeatherVisuals", key, L"", value, std::size(value), tuningIni.c_str());
    return value[0] ? float(wcstod(value, nullptr)) : fallback;
}

inline void CreateProceduralFallback(IDirect3DDevice9* device)
{
    if (!rainDropTex)
    {
        device->CreateTexture(16, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, rainDropTex.GetAddressOf(), nullptr);
        if (rainDropTex)
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(rainDropTex->LockRect(0, &lr, nullptr, 0)))
            {
                DWORD* pixels = static_cast<DWORD*>(lr.pBits);
                for (int y = 0; y < 128; ++y)
                {
                    float fy = float(y) / 128.0f;
                    BYTE a = static_cast<BYTE>(std::clamp(fy * 255.0f, 0.0f, 255.0f));
                    for (int x = 0; x < 16; ++x)
                    {
                        float fx = (float(x) - 7.5f) / 7.5f;
                        float w = std::max(0.0f, 1.0f - fx * fx);
                        BYTE alpha = static_cast<BYTE>(a * w);
                        pixels[y * (lr.Pitch / 4) + x] = (alpha << 24) | 0x00FFFFFF;
                    }
                }
                rainDropTex->UnlockRect(0);
            }
        }
    }

    // Sandstorms use the same native billboard machinery as precipitation,
    // but their stock atlas is an opaque square on this client. A soft,
    // irregular dust puff keeps the particles world-space and depth-tested
    // while removing the visible quad boundary.
    if (!sandParticleTex)
    {
        device->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            sandParticleTex.GetAddressOf(), nullptr);
        if (sandParticleTex)
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(sandParticleTex->LockRect(0, &lr, nullptr, 0)))
            {
                for (int y = 0; y < 64; ++y)
                {
                    DWORD* row = reinterpret_cast<DWORD*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch);
                    for (int x = 0; x < 64; ++x)
                    {
                        const float fx = (float(x) + .5f) / 32.f - 1.f;
                        const float fy = (float(y) + .5f) / 32.f - 1.f;
                        const float r2 = fx * fx + fy * fy;
                        const float edge = std::clamp(1.f - r2, 0.f, 1.f);
                        const float grain = .72f + .18f * std::sin(float(x * 17 + y * 29)) +
                            .10f * std::sin(float(x * 7 - y * 13));
                        const BYTE a = static_cast<BYTE>(255.f * std::clamp(edge * edge * grain, 0.f, 1.f));
                        row[x] = (DWORD(a) << 24) | 0x00E8D2A8u;
                    }
                }
                sandParticleTex->UnlockRect(0);
            }
        }
    }
}

inline void CreateNeutralDepthFallback(IDirect3DDevice9* device)
{
    if (!device || neutralDepthTex) return;
    if (FAILED(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, neutralDepthTex.GetAddressOf(), nullptr)) || !neutralDepthTex)
        return;
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(neutralDepthTex->LockRect(0, &locked, nullptr, 0)))
    {
        *static_cast<DWORD*>(locked.pBits) = 0xFFFFFFFFu;
        neutralDepthTex->UnlockRect(0);
    }
}

inline void EnsureShaders(IDirect3DDevice9* device)
{
    if (!device) return;
    if (precipitationPS && lensDropletsPS && nativeParticlePS) return;

    ComPtr<ID3DBlob> codeBlob, errBlob;
    if (!precipitationPS)
    {
        HRESULT hr = D3DCompile(precipitationHLSL, strlen(precipitationHLSL), nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0, codeBlob.GetAddressOf(), errBlob.GetAddressOf());
        if (SUCCEEDED(hr) && codeBlob)
            device->CreatePixelShader(static_cast<DWORD*>(codeBlob->GetBufferPointer()), precipitationPS.GetAddressOf());
        else if (errBlob)
            Log(static_cast<const char*>(errBlob->GetBufferPointer()));
    }

    if (!lensDropletsPS)
    {
        codeBlob.Reset(); errBlob.Reset();
        HRESULT hr = D3DCompile(lensDropletsHLSL, strlen(lensDropletsHLSL), nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0, codeBlob.GetAddressOf(), errBlob.GetAddressOf());
        if (SUCCEEDED(hr) && codeBlob)
            device->CreatePixelShader(static_cast<DWORD*>(codeBlob->GetBufferPointer()), lensDropletsPS.GetAddressOf());
        else if (errBlob)
            Log(static_cast<const char*>(errBlob->GetBufferPointer()));
    }

    if (!nativeParticlePS)
    {
        codeBlob.Reset(); errBlob.Reset();
        HRESULT hr = D3DCompile(nativeParticleHLSL, strlen(nativeParticleHLSL),
            nullptr, nullptr, nullptr, "main", "ps_2_0", 0, 0,
            codeBlob.GetAddressOf(), errBlob.GetAddressOf());
        if (SUCCEEDED(hr) && codeBlob)
            device->CreatePixelShader(static_cast<DWORD*>(codeBlob->GetBufferPointer()), nativeParticlePS.GetAddressOf());
        else if (errBlob)
            Log(static_cast<const char*>(errBlob->GetBufferPointer()));
    }
}

inline void LoadTextures(IDirect3DDevice9* device)
{
    if (!device) return;
    std::wstring texDir = basePath + L"textures\\weather\\";

    if (!rainDropTex)
    {
        IDirect3DTexture9* raw = nullptr;
        if (SUCCEEDED(renderer::LoadTextureFromFileWic(device, texDir + L"raindrop01.png", &raw, renderer::WicTextureUsage::LinearData)) && raw)
        {
            rainDropTex.Attach(raw);
            // raindrop01 stores its streak primarily as a very dark mask.
            // WoW 3.3.5's native fixed-function weather pass multiplies RGB,
            // so using the file verbatim creates opaque-looking black rain.
            // Convert it to a neutral-white colour mask while retaining the
            // authored coverage in alpha for every mip level.
            for (UINT level = 0; level < rainDropTex->GetLevelCount(); ++level)
            {
                D3DSURFACE_DESC desc{};
                D3DLOCKED_RECT locked{};
                if (FAILED(rainDropTex->GetLevelDesc(level, &desc)) ||
                    FAILED(rainDropTex->LockRect(level, &locked, nullptr, 0)))
                    continue;
                for (UINT y = 0; y < desc.Height; ++y)
                {
                    BYTE* row = static_cast<BYTE*>(locked.pBits) + y * locked.Pitch;
                    for (UINT x = 0; x < desc.Width; ++x)
                    {
                        BYTE* pixel = row + x * 4; // BGRA
                        const BYTE coverage = std::max(
                            pixel[3], std::max(pixel[0], std::max(pixel[1], pixel[2])));
                        pixel[0] = 235;
                        pixel[1] = 235;
                        pixel[2] = 235;
                        pixel[3] = coverage;
                    }
                }
                rainDropTex->UnlockRect(level);
            }
        }
    }
    if (!splashTex)
    {
        IDirect3DTexture9* raw = nullptr;
        if (SUCCEEDED(renderer::LoadTextureFromFileWic(device, texDir + L"raindropsplash01.png", &raw, renderer::WicTextureUsage::LinearData)) && raw)
            splashTex.Attach(raw);
    }
    if (!snowFlakeTex)
    {
        IDirect3DTexture9* raw = nullptr;
        if (SUCCEEDED(renderer::LoadTextureFromFileWic(device, texDir + L"snowflake01.png", &raw, renderer::WicTextureUsage::LinearData)) && raw)
            snowFlakeTex.Attach(raw);
    }
    if (!snowMistTex)
    {
        IDirect3DTexture9* raw = nullptr;
        if (SUCCEEDED(renderer::LoadTextureFromFileWic(device, texDir + L"snowmist01.png", &raw, renderer::WicTextureUsage::LinearData)) && raw)
            snowMistTex.Attach(raw);
    }

    CreateProceduralFallback(device);
}

inline void ReloadTuning()
{
    effectEnabled = ReadTuningInt(L"Enabled", 1) != 0;
    mode = std::clamp(ReadTuningInt(L"Mode", 0), 0, 2);
    intensity = std::clamp(ReadTuningInt(L"IntensityPercent", 100), 0, 300) * 0.01f;
    speed = std::clamp(ReadTuningInt(L"RainSpeedPercent", 120), 10, 400) * 0.01f;
    windStrength = std::clamp(ReadTuningInt(L"WindStrengthPercent", 35), 0, 300) * 0.01f;
    windAngle = float(ReadTuningInt(L"WindAngle", 15));
    depthOcclusion = ReadTuningInt(L"DepthOcclusion", 1) != 0;
    lensDroplets = ReadTuningInt(L"LensDroplets", 1) != 0;
    lensStrength = std::clamp(ReadTuningInt(L"LensDropletStrength", 60), 0, 200) * 0.01f;
    surfaceSplashes = ReadTuningInt(L"SurfaceSplashes", 1) != 0;
    atmosphereHaze = std::clamp(ReadTuningInt(L"AtmosphereHazePercent", 120), 0, 300) * 0.01f;
    showStatus = ReadTuningInt(L"ShowStatus", 0) != 0;
}

inline void Configure(const std::wstring& base)
{
    basePath = base;
    mainIni = base + L"ModernWoWRenderer.ini";
    tuningIni = base + L"GraphicsEffects.ini";
    logPath = base + L"WeatherVisuals.log";
    enabled = renderer::locationtuning::ReadInt(L"WeatherVisuals", L"Enabled", 1, mainIni.c_str()) != 0;
    ReloadTuning();
    lastTick = GetTickCount();
}

inline void Reset(IDirect3DDevice9* device)
{
    precipitationPS.Reset();
    lensDropletsPS.Reset();
    nativeParticlePS.Reset();
    sceneCopyTex.Reset();
    sceneCopySurface.Reset();
    copyWidth = 0; copyHeight = 0;
    currentDevice = device;
    worldGeometrySeenThisFrame = false;
    nativeWeatherSeenThisFrame = 0;
    nativeWeatherPrimitives = 0;
    composedThisFrame = false;
    weatherSuspended = nativeWeatherMode != 0;
    lastTick = GetTickCount();
}

inline bool IsTerrainShader(uint64_t psHash)
{
    switch (psHash)
    {
    case 0x1259eadbfaafb43eull:
    case 0x8f5f50576e4cf36aull:
    case 0x36df746731dcdefdull:
    case 0x250ee869fc4be850ull:
        return true;
    default:
        return false;
    }
}

inline void ObserveWorldDraw()
{
    if (IsTerrainShader(renderer::g_trackedState.psHash))
        worldGeometrySeenThisFrame = true;
}

// WoW already creates world-space, depth-tested rain/snow billboards. The
// Classic/Forever assets belong on those draws, not on a screen-sized quad in
// Present(). This scope recognizes only plausible native precipitation draws,
// swaps their stage-0 texture, and restores the game's texture immediately.
class NativeParticleScope
{
public:
    NativeParticleScope(IDirect3DDevice9* device, UINT primitiveCount)
        : m_device(device)
    {
        if (!device ||
            !renderer::g_trackedState.alphaBlend ||
            renderer::g_trackedState.zWrite || !renderer::g_trackedState.zEnable ||
            primitiveCount < 16)
            return;

        IDirect3DBaseTexture9* base = nullptr;
        if (FAILED(device->GetTexture(0, &base)) || !base || base->GetType() != D3DRTYPE_TEXTURE)
        {
            if (base) base->Release();
            return;
        }
        m_original.Attach(base); // GetTexture already AddRef'd it.

        auto* texture = static_cast<IDirect3DTexture9*>(base);
        D3DSURFACE_DESC desc{};
        if (FAILED(texture->GetLevelDesc(0, &desc)) || (desc.Usage & D3DUSAGE_RENDERTARGET))
            return;

        const uint64_t textureHash = renderer::TextureHashLookup::Instance().GetTextureHash(base);
        // Captured directly from this Ascension 3.3.5 client. Dimensions alone
        // also matched UI atlases and spell particles, so they are not safe.
        const bool exactRain = desc.Width == 16 && desc.Height == 128 &&
            primitiveCount >= 6000 && textureHash == 0xde9331cb560b9814ull &&
            renderer::g_trackedState.vsHash == 0x8832bcc5ed4ff2a6ull &&
            renderer::g_trackedState.psHash == 0;
        const bool exactSnow = desc.Width == 64 && desc.Height == 64 &&
            primitiveCount >= 6000 && textureHash == 0x50aa504baa78b237ull &&
            renderer::g_trackedState.vsHash == 0xec9ee827b8bf5afaull &&
            renderer::g_trackedState.psHash == 0;
        // Type 3 (sandstorm) reuses the 64x64 precipitation atlas but selects
        // the rain-family vertex shader. This exact pairing was captured from
        // the client's native draw and is distinct from snow above.
        const bool exactSand = desc.Width == 64 && desc.Height == 64 &&
            primitiveCount >= 6000 && textureHash == 0x50aa504baa78b237ull &&
            renderer::g_trackedState.vsHash == 0x8832bcc5ed4ff2a6ull &&
            renderer::g_trackedState.psHash == 0;

        const int detected = exactRain ? 1 : (exactSnow ? 2 : (exactSand ? 3 : 0));

        // Record a bounded set of native transparent candidates for exact
        // rain/clear/snow comparison without flooding the log.
        if (loggedParticleCandidateCount < 128)
        {
            uint64_t key = textureHash ^ (uint64_t(desc.Width) << 48) ^
                (uint64_t(desc.Height) << 32) ^ renderer::g_trackedState.psHash;
            if (loggedParticleCandidates.insert(key).second)
            {
                std::ostringstream line;
                line << "Native particle candidate: " << desc.Width << 'x' << desc.Height
                     << " prims=" << primitiveCount
                     << " tex=0x" << std::hex << textureHash
                     << " vs=0x" << renderer::g_trackedState.vsHash
                     << " ps=0x" << renderer::g_trackedState.psHash << std::dec
                     << " detected=" << detected;
                Log(line.str().c_str());
                ++loggedParticleCandidateCount;
            }
        }

        if (!detected)
            return;

        // Detection is a world signal, not a side effect of successful
        // texture/shader replacement. Clouds still follow native weather
        // with WeatherVisuals disabled or optional assets unavailable.
        nativeWeatherSeenThisFrame = detected;
        nativeWeatherMode = detected;
        lastNativeWeatherTick = GetTickCount();
        nativeWeatherPrimitives += primitiveCount;
        if (!enabled || !effectEnabled || !active) return;

        EnsureShaders(device);
        LoadTextures(device);
        IDirect3DBaseTexture9* replacement = detected == 2
            ? static_cast<IDirect3DBaseTexture9*>(snowFlakeTex.Get())
            : (detected == 3 ? static_cast<IDirect3DBaseTexture9*>(sandParticleTex.Get())
                             : static_cast<IDirect3DBaseTexture9*>(rainDropTex.Get()));
        if (!replacement || !nativeParticlePS)
            return;

        internal = true;
        device->GetPixelShader(m_originalPS.GetAddressOf());
        device->GetPixelShaderConstantF(0, m_originalC0, 1);
        const float alphaScale = intensity * renderer::FrameContext::Current().environment[renderer::Weather] * (detected == 2 ? 1.55f : (detected == 3 ? 0.48f : 0.70f));
        const float params[4] = {
            detected == 2 ? 0.96f : (detected == 3 ? 0.78f : 0.90f),
            detected == 2 ? 0.98f : (detected == 3 ? 0.67f : 0.90f),
            detected == 2 ? 1.00f : (detected == 3 ? 0.48f : 0.90f),
            alphaScale };
        device->SetPixelShader(nativeParticlePS.Get());
        device->SetPixelShaderConstantF(0, params, 1);
        m_modified = true;
        if (SUCCEEDED(device->SetTexture(0, replacement)))
            m_replaced = true;
        internal = false;
    }

    ~NativeParticleScope()
    {
        if (!m_modified || !m_device) return;
        internal = true;
        m_device->SetTexture(0, m_original.Get());
        m_device->SetPixelShader(m_originalPS.Get());
        m_device->SetPixelShaderConstantF(0, m_originalC0, 1);
        internal = false;
    }

private:
    IDirect3DDevice9* m_device = nullptr;
    ComPtr<IDirect3DBaseTexture9> m_original;
    ComPtr<IDirect3DPixelShader9> m_originalPS;
    float m_originalC0[4]{};
    bool m_replaced = false;
    bool m_modified = false;
};

// Render the procedural continuity layer immediately before WoW's first UI
// draw. Native particles still provide the world-space precipitation; this
// fills gaps caused by their small player-centred simulation volume without
// touching game UI or the F7 overlay.
inline void BeforeDraw(IDirect3DDevice9* device)
{
    const bool isUi = renderer::g_trackedState.vsHash == 0xd9e7756460af6296ull ||
                      renderer::g_trackedState.psHash == 0xc29c7060b723c0c6ull;
    if (!isUi || composedThisFrame || !enabled || !effectEnabled || !active || !device)
        return;
    const int renderMode = nativeWeatherMode ? (mode ? mode : nativeWeatherMode) : 0;
    if (!renderMode) return;
    composedThisFrame = true;

    // Time step calculation
    DWORD now = GetTickCount();
    float dt = (now >= lastTick) ? (now - lastTick) * 0.001f : 0.016f;
    lastTick = now;
    dt = std::clamp(dt, 0.001f, 0.1f);
    elapsedTime += dt;

    // Depth is optional for the visible precipitation/lens layers. If INTZ
    // capture is unavailable, only roof occlusion is disabled; returning here
    // used to make the menu say RAIN ON while drawing absolutely nothing.
    const bool hasCapturedDepth = renderer::DepthCapture::Instance().HasDepth() &&
        renderer::DepthCapture::Instance().GetDepthTexture();
    CreateNeutralDepthFallback(device);
    IDirect3DTexture9* depthTex = hasCapturedDepth
        ? renderer::DepthCapture::Instance().GetDepthTexture()
        : neutralDepthTex.Get();
    if (!depthTex) return;

    ComPtr<IDirect3DSurface9> backBuffer;
    if (FAILED(device->GetRenderTarget(0, backBuffer.GetAddressOf())) || !backBuffer)
        return;

    D3DSURFACE_DESC desc{};
    backBuffer->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) return;

    EnsureShaders(device);
    LoadTextures(device);

    if (!precipitationPS) return;

    // Precipitation itself composites over sceneTex, so the scene copy is
    // mandatory even when lens droplets are disabled.
    if (!sceneCopyTex || copyWidth != desc.Width || copyHeight != desc.Height)
    {
        sceneCopyTex.Reset(); sceneCopySurface.Reset();
        copyWidth = desc.Width; copyHeight = desc.Height;
        device->CreateTexture(copyWidth, copyHeight, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, sceneCopyTex.GetAddressOf(), nullptr);
        if (sceneCopyTex)
            sceneCopyTex->GetSurfaceLevel(0, sceneCopySurface.GetAddressOf());
    }

    if (!sceneCopySurface || FAILED(device->StretchRect(backBuffer.Get(), nullptr, sceneCopySurface.Get(), nullptr, D3DTEXF_NONE)))
        return;

    internal = true;
    {
    renderer::ScopedRenderState savedState(device);

    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetVertexShader(nullptr);
    device->SetVertexDeclaration(nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

    // Compute wind vector
    float rad = windAngle * 0.0174532925f;
    float windX = std::sin(rad) * windStrength;
    float windY = std::cos(rad) * 0.2f;

    // Setup shader constants
    // c0: Projection params from CameraCapture / frame constants
    const renderer::FrameContext& frame = renderer::FrameContext::Current();
    float projConstants[4] = {
        frame.cameraValid ? frame.projUnpack[0] : 0.001f,
        frame.cameraValid ? frame.projUnpack[1] : 1.0f,
        frame.cameraValid ? frame.projUnpack[2] : float(desc.Width) / float(desc.Height),
        frame.cameraValid ? frame.projUnpack[3] : 1.0f };
    // c1: Weather params
    // A restrained layer supplements native particles rather than doubling
    // their density. It remains camera-relative, so mount speed cannot outrun it.
    float weatherConst[4] = { elapsedTime, intensity * renderer::FrameContext::Current().environment[renderer::Weather] * .45f, float(renderMode), speed };
    // c2: Wind params
    float windConst[4] = { windX, windY, atmosphereHaze, smoothedIndoor };
    // c3: Screen params
    float screenConst[4] = { float(desc.Width), float(desc.Height), 1.0f / float(desc.Width), 1.0f / float(desc.Height) };
    // c4: Light & occlusion toggle
    float lightConst[4] = {
        frame.cameraValid ? std::max(0.15f, frame.directionalLightColor.x) : 1.0f,
        frame.cameraValid ? std::max(0.15f, frame.directionalLightColor.y) : 1.0f,
        frame.cameraValid ? std::max(0.15f, frame.directionalLightColor.z) : 1.0f,
        depthOcclusion && hasCapturedDepth ? 1.0f : 0.0f };

    device->SetPixelShaderConstantF(0, projConstants, 1);
    device->SetPixelShaderConstantF(1, weatherConst, 1);
    device->SetPixelShaderConstantF(2, windConst, 1);
    device->SetPixelShaderConstantF(3, screenConst, 1);
    device->SetPixelShaderConstantF(4, lightConst, 1);

    struct QuadVertex { float x, y, z, rhw, u, v; };
    float w = float(desc.Width) - 0.5f, h = float(desc.Height) - 0.5f;
    QuadVertex quad[] = {
        { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
        { w,     -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
        { -0.5f, h,     0.0f, 1.0f, 0.0f, 1.0f },
        { w,     h,     0.0f, 1.0f, 1.0f, 1.0f }
    };

    // Render precipitation pass
    device->SetPixelShader(precipitationPS.Get());
    device->SetTexture(0, sceneCopyTex ? sceneCopyTex.Get() : nullptr);
    device->SetTexture(1, depthTex);
    device->SetTexture(2, (mode == 2) ? (snowFlakeTex ? snowFlakeTex.Get() : rainDropTex.Get()) : (rainDropTex ? rainDropTex.Get() : nullptr));
    device->SetTexture(3, snowMistTex ? snowMistTex.Get() : nullptr);

    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    static bool loggedFirstRender = false;
    if (!loggedFirstRender)
    {
        loggedFirstRender = true;
        Log(hasCapturedDepth
            ? "WeatherVisuals render path active: captured depth occlusion enabled"
            : "WeatherVisuals render path active: precipitation fallback without captured depth");
    }

    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));

    // Render camera lens droplets pass (if rain and enabled)
    if (lensDroplets && renderMode == 1 && lensDropletsPS && sceneCopyTex && lensStrength > 0.01f)
    {
        // A render-target texture cannot be a StretchRect destination while
        // it is still bound for sampling by the precipitation pass.
        device->SetTexture(0, nullptr);
        const bool lensCopyReady = SUCCEEDED(
            device->StretchRect(backBuffer.Get(), nullptr, sceneCopySurface.Get(), nullptr, D3DTEXF_NONE));
        if (!lensCopyReady)
        {
            static bool loggedLensCopyFailure = false;
            if (!loggedLensCopyFailure)
            {
                loggedLensCopyFailure = true;
                Log("WeatherVisuals: lens scene copy failed");
            }
        }
        else
        {
            float lensConst[4] = { elapsedTime, lensStrength, smoothedIndoor, 0.0f };
            device->SetPixelShader(lensDropletsPS.Get());
            device->SetPixelShaderConstantF(0, lensConst, 1);
            device->SetPixelShaderConstantF(1, screenConst, 1);
            device->SetTexture(0, sceneCopyTex.Get());
            device->SetTexture(1, depthTex);

            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));
        }
    }

    }
    internal = false;
}

inline void Present(IDirect3DDevice9*, bool preserveDetection = false)
{
    // F9 belongs to celestial diagnostics. Weather uses F10 so the two
    // independent modules cannot toggle each other in the same key press.
    bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    const bool focused = foregroundPid == GetCurrentProcessId();
    if (hotkey && down && !keyDown && focused)
    {
        active = !active;
        Log(active ? "WeatherVisuals activated (F10)" : "WeatherVisuals paused (F10)");
    }
    keyDown = down;

    const DWORD now = GetTickCount();
    if (nativeWeatherSeenThisFrame)
    {
        nativeWeatherMode = nativeWeatherSeenThisFrame;
        lastNativeWeatherTick = now;
        weatherSuspended = false;
    }
    else if ((!focused || preserveDetection) && nativeWeatherMode)
    {
        // Alt+Tab and the interactive F7 overlay can pause or throttle native
        // particle emission. Absence of a draw then is not evidence that the
        // weather ended.
        weatherSuspended = true;
    }
    else if (weatherSuspended && nativeWeatherMode)
    {
        // Give the client time to refill its native particle system after
        // focus/device restoration while the continuity layer stays alive.
        lastNativeWeatherTick = now;
        weatherSuspended = false;
    }
    else if (lastNativeWeatherTick && now - lastNativeWeatherTick > nativeWeatherHoldMs)
    {
        nativeWeatherMode = 0;
        lastNativeWeatherTick = 0;
    }

    nativeWeatherSeenThisFrame = 0;
    nativeWeatherPrimitives = 0;
    worldGeometrySeenThisFrame = false;
    composedThisFrame = false;
}

inline const char* GetStatusText()
{
    if (!enabled || !effectEnabled || !active)
        return "WEATHER OFF";
    switch (nativeWeatherMode)
    {
    case 3: return "SANDSTORM ON";
    case 2: return "SNOW ON";
    case 1: return "RAIN ON";
    default: return "WEATHER AUTO";
    }
}
}
