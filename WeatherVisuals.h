#pragma once
#include "src/Environment/LocationTuning.h"
#include "src/Effects/PrecipitationMotion.h"
#include "src/Effects/RainField.h"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "src/D3D9/ShaderBytecodeCache.h"
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
#include "src/D3D9/ScopedPassState.h"
#include "src/D3D9/TrackedRenderState.h"
#include "src/Core/FrameContext.h"

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
inline float intensity = 1.0f;
inline float speed = 1.2f; // snow/sand continuity only
inline renderer::rainfield::Motion rainMotion;
inline float rainFallSpeed = 1.f, rainDensity = 1.f, rainLength = 1.f;
inline float rainWidth = 1.f, rainOpacity = 1.f, rainWindInfluence = 1.f;
inline int rainDebugView = 0;
inline renderer::PrecipitationMotion precipitationMotion;
inline bool depthOcclusion = true;
inline bool lensDroplets = true;
inline float lensStrength = 0.60f;
inline float atmosphereHaze = 1.20f;

inline std::wstring mainIni, tuningIni, logPath, basePath;
inline IDirect3DDevice9* currentDevice = nullptr;

// GPU Resources
inline ComPtr<IDirect3DPixelShader9> precipitationPS;
inline ComPtr<IDirect3DPixelShader9> rainPS;
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
inline bool rainCompositeReady = false;
inline bool weatherSuspended = false;
// Native particle batches can be sparse even in steady rain (or disappear
// under cover). A missed draw is not a clear-weather event.
inline constexpr DWORD nativeWeatherHoldMs = 30000;
inline std::unordered_set<uint64_t> loggedParticleCandidates;
inline unsigned loggedParticleCandidateCount = 0;

inline const char* precipitationHLSL = R"HLSL(
sampler2D sceneTex : register(s0);
sampler2D depthMap : register(s1);
sampler2D weatherTex : register(s2);
sampler2D mistTex : register(s3);

// c0: x=projA, y=projB, z=scaleX, w=scaleY
float4 projection : register(c0);
// c1: x=raw fall phase (diagnostics only), y=intensity,
//     z=mode (2=Snow, 3=Sandstorm)
float4 weatherParams : register(c1);
// c2: xy=raw wind offset (diagnostics only), z=haze, w=indoorFactor
float4 windParams : register(c2);
// c3: x=resX, y=resY, z=invResX, w=invResY
float4 screenParams : register(c3);
// c4: light color / tint (rgb), w=depthOcclusionEnabled
float4 lightColor : register(c4);
// c5: x=viewport MinZ, y=reciprocal viewport depth range
float4 depthRange : register(c5);
// c6/c7/c8: bounded snow/sand phase coordinates, split on the CPU.
float4 layerPhase0 : register(c6);
float4 layerPhase1 : register(c7);
float4 layerPhase2 : register(c8);
// c9: x = snow wobble sin argument (2pi-wrapped), yzw = sand gust sin
//     arguments (2pi-wrapped)
float4 wobbles : register(c9);
// c10: xy = mist drift (decimal parts; the mist samples with frac)
float4 mistDrift : register(c10);

float GetLinearDepth(float raw)
{
    // INTZ contains viewport-scaled depth (WoW commonly uses MaxZ=.94).
    // Cleared sky must stay far away, including the no-depth fallback.
    if (raw >= .99999) return 100000.0;
    float d = saturate((raw-depthRange.x)*depthRange.y);
    return projection.y / min(d - projection.x, -.000001);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float rawDepth = tex2D(depthMap, uv).r;
    float sceneZ = GetLinearDepth(rawDepth);
    float4 baseColor = tex2D(sceneTex, uv);
    
    float intensity = weatherParams.y * (1.0 - windParams.w * 0.96);
    if (intensity <= 0.001)
        return baseColor;
        
    float3 accum = float3(0, 0, 0);
    float totalAlpha = 0.0;
    
    // Snow/sand continuity (rain uses the separate analytical pass below)
    float layerDepths[3] = { 3.5, 11.0, 26.0 };
    float layerScales[3] = { 1.6, 3.8, 8.5 };
    float layerSpeeds[3] = { 1.25, 1.0, 0.75 };
    float layerWeights[3] = { 0.45, 0.35, 0.20 };
    
    float isSnow = step(1.5, weatherParams.z) * step(weatherParams.z, 2.5);
    float isSand = step(2.5, weatherParams.z);
    
    float sandWobble[3] = { wobbles.y, wobbles.z, wobbles.w };
    // Snow and sand use their existing bounded phase coordinates.
    float phaseX[3] = { layerPhase0.x, layerPhase1.x, layerPhase2.x };
    float phaseY[3] = { layerPhase0.z, layerPhase1.z, layerPhase2.z };
    
    [unroll] for (int i = 0; i < 3; ++i)
    {
        float lz = layerDepths[i];
        // Depth occlusion: if scene is closer than this layer, precipitation behind is occluded
        float occlusion = (lightColor.w > 0.5) ? smoothstep(lz * 0.75, lz * 1.15, sceneZ) : 1.0;
        if (occlusion <= 0.001) continue;
        
        // Snow/sand sample only the bounded fractional phase.
        if (isSnow > 0.5)
        {
            // Snow: sinusoidal drift. Screen V grows downwards, so
            // subtracting travel from the sampled UV moves the visible
            // flake down, not up. (frac() is 1-periodic, so the integer
            // phase part never needs to enter the sample.)
            float2 gridDec = uv * (12.0 * layerScales[i])
                           - float2(phaseX[i], phaseY[i]);
            gridDec.x += sin(wobbles.x + uv.y * layerScales[i] * 3.5) * 1.44;
            float4 snowSamp = tex2D(weatherTex, frac(gridDec));
            float flake = snowSamp.a * snowSamp.r;
            accum += float3(0.92, 0.95, 1.0) * flake * layerWeights[i] * occlusion;
            totalAlpha += flake * layerWeights[i] * occlusion;
        }
        else if (isSand > 0.5)
        {
            // Sandstorm: rapid dusty drift. The wobble arguments are
            // 2pi-wrapped on the CPU, so the gusts stay exact for a whole
            // session.
            float2 gridDec = uv * (0.5 * layerScales[i])
                           - float2(phaseX[i], phaseY[i]);
            gridDec.y += sin(2.0 * layerScales[i] * uv.x - sandWobble[i]) * 0.025;
            float4 sandSamp = tex2D(mistTex, frac(gridDec));
            float dust = sandSamp.a * 0.6;
            accum += float3(0.85, 0.75, 0.55) * dust * layerWeights[i] * occlusion;
            totalAlpha += dust * layerWeights[i] * occlusion;
        }

    }
    
    // Add subtle ambient mist during snow at mid-distances
    if (isSand < 0.5 && sceneZ > 6.0)
    {
        // The drift phases' decimal parts (c10); the soft haze samples
        // with frac(), so only the [0,1) parts matter.
        float2 mistUV = uv * 1.5 - float2(mistDrift.x, mistDrift.y);
        float mistSamp = tex2D(mistTex, frac(mistUV)).a;
        float mistDepthFade = smoothstep(6.0, 40.0, sceneZ);
        // Apply intensity once, below, just like the drops. Squaring it here
        // made high-intensity rain emit a disproportionately bright wash.
        float mistAmount = mistSamp * mistDepthFade * 0.08 * windParams.z;
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

// Dedicated analytical rain renderer. No reused rain atlas or tiled UV streaks:
// every occupied cell contains one independently seeded, subpixel-antialiased
// falling line. Cell-local bounds keep it inside its cell as the lattice moves.
inline const char* rainHLSL = R"HLSL(
sampler2D sceneTex : register(s0);
sampler2D depthMap : register(s1);
float4 projection : register(c0); // depth unpack A/B
float4 screen : register(c1); // width, height, invWidth, invHeight
float4 light : register(c2); // neutral lighting RGB, depth available
float4 shape : register(c3); // density, length, width, opacity
float4 control : register(c4); // intensity, indoor, occlusion, debug (0/1/2)
float4 travel : register(c5); // unit screen direction XY, wind X/Y in px/s
float4 phase0 : register(c6);
float4 phase1 : register(c7);
float4 phase2 : register(c8);
float4 depthRange : register(c9);

float Hash(float2 id)
{
    // Wrap the integer phase before hashing: adjacent cells remain distinct
    // even after hours, and the 4096-cell seam repeats without a jump.
    id = id - 4096.0 * floor(id / 4096.0);
    float3 p = frac(float3(id.x, id.y, id.x) * .1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}
float Depth(float raw)
{
    if (raw >= .99999) return 100000.0;
    float d = saturate((raw-depthRange.x)*depthRange.y);
    return projection.y / min(d-projection.x, -.000001);
}
float Particle(float2 pixel, float2 cellSize, float4 phase, float seed,
               float lengthPx, float widthPx, float occupancy, float pixelFootprint)
{
    float2 grid = pixel / cellSize - phase.xz;
    float2 id = floor(grid) - phase.yw;
    float2 local = frac(grid) - .5;
    float a = Hash(id + seed);
    if (a >= occupancy) return 0.0;
    float b = Hash(id + seed + 31.7);
    float c = Hash(id + seed + 73.2);
    // Seeded center is inset so no line is clipped at a cell boundary.
    float2 center = float2((b-.5)*.36, (c-.5)*.22);
    float2 delta = (local-center)*cellSize;
    float2 axis = travel.xy;
    float along = dot(delta,axis);
    float across = abs(dot(delta,float2(axis.y,-axis.x)));
    float width = max(.35,widthPx * lerp(.75,1.25,c));
    float halfLength = min(lengthPx * lerp(.65,1.15,b) * .5,
        min((cellSize.x*.5-abs(center.x*cellSize.x)-width-1.0)/max(abs(axis.x),.01),
            (cellSize.y*.5-abs(center.y*cellSize.y)-width-1.0)/max(abs(axis.y),.01)));
    // The leading (downwind) head catches more light; the trailing end is
    // thinner and less bright. Coverage is integrated across ONE output
    // pixel in reference units, so 720p rain doesn't break into hard dots
    // and 4K rain doesn't turn into blurry ribbons.
    float head = saturate(.5 + .5*along/max(halfLength,1.0));
    float taperedWidth = width * lerp(.58,1.0,head);
    float body = saturate((taperedWidth-across)/pixelFootprint + .5);
    float ends = saturate((halfLength-abs(along))/pixelFootprint + .5);
    return body * ends * lerp(.68,1.08,head) * lerp(.55,1.0,a/max(occupancy,.001));
}
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 scene = tex2D(sceneTex,uv);
    float z = Depth(tex2D(depthMap,uv).r);
    float scale = screen.y / 1080.0;
    float2 pixel = uv * screen.xy / scale; // reference 1080p pixels
    float pixelFootprint = max(.25,1.0/scale);
    float coverage = 0.0;
    float visibility = 0.0;
    float3 weights = float3(.44,.33,.23);
    float3 distances = float3(4.0, 11.0, 25.0);
    float4 phases[3] = {phase0,phase1,phase2};
    float2 cells[3] = {float2(38,60),float2(28,48),float2(22,38)};
    float lengths[3] = {18.0,11.0,7.0};
    float widths[3] = {1.15,.95,.75};
    [unroll] for (int i=0;i<3;++i)
    {
        float visible = light.w > .5 && control.z > .5
            ? smoothstep(distances[i]-.8,distances[i]+.8,z) : 1.0;
        float drop = Particle(pixel,cells[i],phases[i],float(i)*127.13,
            lengths[i]*shape.y,widths[i]*shape.z,
            saturate(shape.x * (.30 + float(i)*.025)),pixelFootprint);
        coverage += drop * weights[i] * visible;
        visibility += weights[i] * visible;
    }
    coverage = saturate(coverage * shape.w * control.x * (1.0-control.y*.96));
    if (control.w > 1.5)
    {
        // Direction / occlusion diagnostic: red=rightward drift,
        // blue=leftward drift, green=downward; dark = geometry blocks rain.
        float blocked = visibility;
        float3 direction = float3(saturate(.35+travel.z/400.0),
                                  saturate(.35+travel.y*.35),
                                  saturate(.35-travel.z/400.0));
        return float4(lerp(float3(.08,.015,.015),direction*.35,blocked) + coverage.xxx,scene.a);
    }
    if (control.w > .5) return float4(coverage.xxx,scene.a);
    // A wet streak scatters cool daylight on dark scenery but gently
    // refracts (rather than painting white over) a bright WoW sky. This
    // preserves contrast without a gray rain curtain or neon zone tint.
    float3 scatter = float3(.68,.73,.80) * (.60 + .40*saturate(light.rgb));
    float sceneLuma = dot(scene.rgb,float3(.2126,.7152,.0722));
    float3 refracted = scene.rgb*.55 + scatter*.45;
    float3 rainColor = lerp(scatter,refracted,smoothstep(.25,.85,sceneLuma));
    return float4(lerp(scene.rgb,rainColor,coverage*.70),scene.a);
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
                    float fy = (float(y) + .5f) / 128.0f;
                    // Match the authored mask's ~15% peak alpha, with a soft
                    // tail and head. Full-opacity fallback drops became beams.
                    float taper = std::min(fy * 2.f, (1.f - fy) * 8.f);
                    BYTE a = static_cast<BYTE>(38.f * std::clamp(taper, 0.f, 1.f));
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
    if (precipitationPS && rainPS && lensDropletsPS && nativeParticlePS) return;

    // On-disk bytecode cache first (renderer::shaderbytecode): a repeat
    // launch skips all three fxc passes, and a cached blob the driver
    // refuses falls back to one recompile, keeping the error log as before.
    const std::wstring cacheDir = renderer::shaderbytecode::Dir(basePath);
    ComPtr<ID3DBlob> codeBlob;
    if (!precipitationPS)
    {
        std::string error;
        if (!renderer::shaderbytecode::GetShader(cacheDir, precipitationHLSL, "main", "ps_3_0", codeBlob,
                [device](ID3DBlob* b) { return SUCCEEDED(device->CreatePixelShader(static_cast<DWORD*>(b->GetBufferPointer()), precipitationPS.GetAddressOf())); }, &error))
            Log(error.c_str());
    }

    if (!rainPS)
    {
        codeBlob.Reset();
        std::string error;
        if (!renderer::shaderbytecode::GetShader(cacheDir, rainHLSL, "main", "ps_3_0", codeBlob,
                [device](ID3DBlob* b) { return SUCCEEDED(device->CreatePixelShader(static_cast<DWORD*>(b->GetBufferPointer()), rainPS.GetAddressOf())); }, &error))
            Log(error.c_str());
    }

    if (!lensDropletsPS)
    {
        codeBlob.Reset();
        std::string error;
        if (!renderer::shaderbytecode::GetShader(cacheDir, lensDropletsHLSL, "main", "ps_3_0", codeBlob,
                [device](ID3DBlob* b) { return SUCCEEDED(device->CreatePixelShader(static_cast<DWORD*>(b->GetBufferPointer()), lensDropletsPS.GetAddressOf())); }, &error))
            Log(error.c_str());
    }

    if (!nativeParticlePS)
    {
        codeBlob.Reset();
        std::string error;
        if (!renderer::shaderbytecode::GetShader(cacheDir, nativeParticleHLSL, "main", "ps_2_0", codeBlob,
                [device](ID3DBlob* b) { return SUCCEEDED(device->CreatePixelShader(static_cast<DWORD*>(b->GetBufferPointer()), nativeParticlePS.GetAddressOf())); }, &error))
            Log(error.c_str());
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
    intensity = std::clamp(ReadTuningInt(L"IntensityPercent", 100), 0, 300) * 0.01f;
    rainFallSpeed = std::clamp(ReadTuningInt(L"RainFallSpeedPercent", 100), 10, 400) * .01f;
    rainDensity = std::clamp(ReadTuningInt(L"RainDensityPercent", 100), 0, 250) * .01f;
    rainLength = std::clamp(ReadTuningInt(L"RainLengthPercent", 100), 30, 160) * .01f;
    rainWidth = std::clamp(ReadTuningInt(L"RainWidthPercent", 100), 40, 180) * .01f;
    rainOpacity = std::clamp(ReadTuningInt(L"RainOpacityPercent", 100), 0, 200) * .01f;
    rainWindInfluence = std::clamp(ReadTuningInt(L"RainWindInfluencePercent", 100), 0, 200) * .01f;
    rainDebugView = std::clamp(ReadTuningInt(L"RainDebugView", 0), 0, 2);
    depthOcclusion = ReadTuningInt(L"DepthOcclusion", 1) != 0;
    lensDroplets = ReadTuningInt(L"LensDroplets", 1) != 0;
    lensStrength = std::clamp(ReadTuningInt(L"LensDropletStrength", 60), 0, 200) * 0.01f;
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
    rainPS.Reset();
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
    rainCompositeReady = false;
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

// Texture identity plus the captured weather shader pair is the type signal.
// Batch size is NOT weather intensity: the client varies particle counts and
// can emit small batches while the same game rain continues.
inline int ClassifyNativeWeather(UINT primitives, UINT width, UINT height,
                                 uint64_t textureHash, uint64_t vsHash, uint64_t psHash)
{
    if (!primitives || psHash) return 0;
    if (width == 16 && height == 128 && textureHash == 0xde9331cb560b9814ull &&
        vsHash == 0x8832bcc5ed4ff2a6ull) return 1;
    if (width == 64 && height == 64 && textureHash == 0x50aa504baa78b237ull)
    {
        if (vsHash == 0xec9ee827b8bf5afaull) return 2;
        if (vsHash == 0x8832bcc5ed4ff2a6ull) return 3;
    }
    return 0;
}

// Recognize native weather draws for detection even when rendering is off.
// Snow/sand keep their world-space billboards; rain is hidden only after the
// wind-driven compositor has a working shader and target.
class NativeParticleScope
{
public:
    NativeParticleScope(IDirect3DDevice9* device, UINT primitiveCount)
        : m_device(device)
    {
        if (!device ||
            !renderer::g_trackedState.alphaBlend ||
            renderer::g_trackedState.zWrite || !renderer::g_trackedState.zEnable ||
            primitiveCount == 0)
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

        // Captured directly from this Ascension 3.3.5 client. Dimensions alone
        // also matched UI atlases and spell particles, so they are not safe.
        const bool dimRain = desc.Width == 16 && desc.Height == 128;
        const bool dimSmall = desc.Width == 64 && desc.Height == 64;
        // Only hash the two tiny candidate atlases, and only when the
        // native weather vertex/pixel pair can match (or for bounded logs).
        const uint64_t trackedVs = renderer::g_trackedState.vsHash;
        const uint64_t trackedPs = renderer::g_trackedState.psHash;
        const bool matchPossible = trackedPs == 0 &&
            ((dimRain && trackedVs == 0x8832bcc5ed4ff2a6ull) ||
             (dimSmall && (trackedVs == 0xec9ee827b8bf5afaull || trackedVs == 0x8832bcc5ed4ff2a6ull)));
        const bool logWorthIt = loggedParticleCandidateCount < 128 && (dimRain || dimSmall) && !matchPossible;
        uint64_t textureHash = 0;
        if (matchPossible || logWorthIt)
            textureHash = renderer::TextureHashLookup::Instance().GetTextureHash(base);
        const int detected = ClassifyNativeWeather(primitiveCount, desc.Width, desc.Height,
            textureHash, trackedVs, trackedPs);

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
        // Once the analytical compositor has a valid target, do not mix
        // differently directed native rain with the shared-wind field.
        // Until then the game's billboards remain a visible fallback.
        const bool replaceRain = detected == 1 && rainCompositeReady && rainPS &&
            sceneCopySurface && neutralDepthTex;
        const float alphaScale = replaceRain ? 0.f :
            intensity * renderer::FrameContext::Current().environment[renderer::Weather] *
            (detected == 2 ? 1.55f : (detected == 3 ? .48f : .70f));
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

// Composite rain, or snow/sand continuity, immediately before the first UI
// draw without touching the game UI or F7 overlay.
inline void BeforeDraw(IDirect3DDevice9* device, bool presentFallback = false)
{
    const bool isUi = renderer::g_trackedState.vsHash == 0xd9e7756460af6296ull ||
                      renderer::g_trackedState.psHash == 0xc29c7060b723c0c6ull;
    if ((!isUi && !presentFallback) || composedThisFrame || !enabled || !effectEnabled || !active || !device)
        return;
    // Only the native observation owns the type. The old "Mode" override
    // changed this layer alone while clouds, waves, native particles and
    // the status display kept reporting the real rain/snow/sand type.
    const int renderMode = nativeWeatherMode;
    if (!renderMode) return;

    // Time step calculation
    DWORD now = GetTickCount();
    float dt = (now >= lastTick) ? (now - lastTick) * 0.001f : 0.016f;
    lastTick = now;
    dt = std::clamp(dt, 0.f, 0.1f);
    elapsedTime += dt; // lens animation is independent of rain fall speed

    // All precipitation samples the same published wind snapshot as water
    // and clouds, projected through this frame's view basis.
    // Integrate phases once before any GPU early-out; neither a change in
    // speed/direction nor a device reset can rephase the whole session.
    const renderer::FrameContext& frame = renderer::FrameContext::Current();
    const renderer::WindVector viewRight{frame.viewRaw.m[0][0], frame.viewRaw.m[1][0]};
    const renderer::WindVector viewUp{frame.viewRaw.m[0][1], frame.viewRaw.m[1][1]};
    const auto rainWind = renderer::rainfield::Project(frame.wind, viewRight, viewUp,
        frame.cameraValid && frame.viewRawValid, rainWindInfluence);
    rainMotion.Advance(dt, rainFallSpeed, rainWind);
    precipitationMotion.Advance(dt, speed, frame.wind,
        {frame.viewRaw.m[0][0], frame.viewRaw.m[1][0]},
        {frame.viewRaw.m[0][1], frame.viewRaw.m[1][1]},
        frame.cameraValid && frame.viewRawValid);

    // Depth is optional for the visible precipitation/lens layers. If INTZ
    // capture is unavailable, only roof occlusion is disabled; returning here
    // used to make the menu say RAIN ON while drawing absolutely nothing.
    const bool hasCapturedDepth = renderer::DepthCapture::Instance().GetOwner() == device &&
        renderer::DepthCapture::Instance().HasDepth() &&
        renderer::DepthCapture::Instance().GetDepthTexture() && frame.cameraValid &&
        frame.viewport.MaxZ > frame.viewport.MinZ;
    CreateNeutralDepthFallback(device);
    IDirect3DTexture9* depthTex = hasCapturedDepth
        ? renderer::DepthCapture::Instance().GetDepthTexture()
        : neutralDepthTex.Get();
    if (!depthTex) { if (renderMode == 1) rainCompositeReady = false; return; }

    ComPtr<IDirect3DSurface9> backBuffer;
    if (FAILED(device->GetRenderTarget(0, backBuffer.GetAddressOf())) || !backBuffer)
    { if (renderMode == 1) rainCompositeReady = false; return; }

    D3DSURFACE_DESC desc{};
    backBuffer->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0)
    { if (renderMode == 1) rainCompositeReady = false; return; }

    EnsureShaders(device);
    LoadTextures(device);

    if (renderMode == 1 ? !rainPS : !precipitationPS)
    { if (renderMode == 1) rainCompositeReady = false; return; }

    // Precipitation itself composites over sceneTex, so the scene copy is
    // mandatory even when lens droplets are disabled.
    if (!sceneCopyTex || !sceneCopySurface || copyWidth != desc.Width || copyHeight != desc.Height)
    {
        sceneCopyTex.Reset(); sceneCopySurface.Reset();
        copyWidth = desc.Width; copyHeight = desc.Height;
        device->CreateTexture(copyWidth, copyHeight, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, sceneCopyTex.GetAddressOf(), nullptr);
        if (sceneCopyTex)
            sceneCopyTex->GetSurfaceLevel(0, sceneCopySurface.GetAddressOf());
    }

    if (!sceneCopySurface || FAILED(device->StretchRect(backBuffer.Get(), nullptr, sceneCopySurface.Get(), nullptr, D3DTEXF_NONE)))
    { if (renderMode == 1) rainCompositeReady = false; return; }

    const auto trackedState = renderer::g_trackedState;
    internal = true;
    bool drewPrecipitation = false;
    {
    renderer::ScopedPassState savedState(device);

    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    // This pass replaces scene color; inherited UI/world fog, stencil,
    // scissor or sRGB state must not turn drops into bright/clipped bands.
    for (auto state : {D3DRS_FOGENABLE, D3DRS_LIGHTING, D3DRS_STENCILENABLE,
                       D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE})
        device->SetRenderState(state, FALSE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetVertexShader(nullptr);
    device->SetVertexDeclaration(nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

    const auto motion = precipitationMotion.Phase();
    // c6/c7/c8: the same phases, wrapped per layer into their own lattice
    // periods (see ComputeWrappedPhases). These are what the pixel stage's
    // grids actually use; motion.fall/windX/windY stay raw for c1/c2.
    const auto layerPhases = renderer::ComputeWrappedPhases(
        precipitationMotion.Fall(), precipitationMotion.WindX(),
        precipitationMotion.WindY(), renderMode);

    // Setup shader constants
    // c0: Projection params from CameraCapture / frame constants
    float projConstants[4] = {
        hasCapturedDepth ? frame.projUnpack[0] : 1.001f,
        hasCapturedDepth ? frame.projUnpack[1] : -.1f,
        frame.cameraValid ? frame.projUnpack[2] : float(desc.Width) / float(desc.Height),
        frame.cameraValid ? frame.projUnpack[3] : 1.0f };
    // c1: Weather params
    // Snow/sand continuity supplements their native particles.
    float weatherConst[4] = { motion.fall, intensity * frame.environment[renderer::Weather] * .45f, float(renderMode), 0.f };
    // c2: Integrated screen-space displacement, not current wind * session age.
    float windConst[4] = { motion.windX, motion.windY, atmosphereHaze, smoothedIndoor };
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
    const float depthConst[4] = {hasCapturedDepth ? frame.viewport.MinZ : 0.f,
        hasCapturedDepth ? 1.f/(frame.viewport.MaxZ-frame.viewport.MinZ) : 1.f, 0, 0};
    device->SetPixelShaderConstantF(5, depthConst, 1);
    device->SetPixelShaderConstantF(6, layerPhases.c6, 1);
    device->SetPixelShaderConstantF(7, layerPhases.c7, 1);
    device->SetPixelShaderConstantF(8, layerPhases.c8, 1);
    device->SetPixelShaderConstantF(9, layerPhases.c9, 1);
    device->SetPixelShaderConstantF(10, layerPhases.c10, 1);

    struct QuadVertex { float x, y, z, rhw, u, v; };
    float w = float(desc.Width) - 0.5f, h = float(desc.Height) - 0.5f;
    QuadVertex quad[] = {
        { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
        { w,     -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
        { -0.5f, h,     0.0f, 1.0f, 0.0f, 1.0f },
        { w,     h,     0.0f, 1.0f, 1.0f, 1.0f }
    };

    // Render precipitation pass
    if (renderMode == 1)
    {
        float phases[3][4]{};
        for (unsigned i = 0; i < 3; ++i) rainMotion.Layer(i, phases[i]);
        const float verticalSpeed = renderer::rainfield::VerticalSpeed(rainFallSpeed, rainWind);
        const float magnitude = std::hypot(rainWind.x, verticalSpeed);
        const float rainTravel[4] = {rainWind.x / std::max(magnitude,1.f),
            verticalSpeed / std::max(magnitude,1.f),rainWind.x,rainWind.y};
        const float rainShape[4] = {rainDensity,rainLength,rainWidth,rainOpacity};
        const float rainControl[4] = {intensity * frame.environment[renderer::Weather],
            smoothedIndoor,depthOcclusion ? 1.f : 0.f,float(rainDebugView)};
        device->SetPixelShaderConstantF(0, projConstants, 1);
        device->SetPixelShaderConstantF(1, screenConst, 1);
        device->SetPixelShaderConstantF(2, lightConst, 1);
        device->SetPixelShaderConstantF(3, rainShape, 1);
        device->SetPixelShaderConstantF(4, rainControl, 1);
        device->SetPixelShaderConstantF(5, rainTravel, 1);
        device->SetPixelShaderConstantF(6, phases[0], 3);
        device->SetPixelShaderConstantF(9, depthConst, 1);
    }
    device->SetPixelShader(renderMode == 1 ? rainPS.Get() : precipitationPS.Get());
    device->SetTexture(0, sceneCopyTex ? sceneCopyTex.Get() : nullptr);
    device->SetTexture(1, depthTex);
    device->SetTexture(2, renderMode == 2 ? snowFlakeTex.Get() :
        (renderMode == 3 ? rainDropTex.Get() : nullptr));
    device->SetTexture(3, snowMistTex ? snowMistTex.Get() : nullptr);

    for (DWORD slot=0; slot<4; ++slot)
    {
        const DWORD filter = slot == 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
        const DWORD address = slot < 2 ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP;
        device->SetSamplerState(slot, D3DSAMP_MINFILTER, filter);
        device->SetSamplerState(slot, D3DSAMP_MAGFILTER, filter);
        device->SetSamplerState(slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(slot, D3DSAMP_ADDRESSU, address);
        device->SetSamplerState(slot, D3DSAMP_ADDRESSV, address);
        device->SetSamplerState(slot, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    static bool loggedFirstRender = false;
    if (!loggedFirstRender)
    {
        loggedFirstRender = true;
        Log(hasCapturedDepth
            ? "WeatherVisuals render path active: captured depth occlusion enabled"
            : "WeatherVisuals render path active: precipitation fallback without captured depth");
    }

    drewPrecipitation = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex)));

    // Render camera lens droplets pass (if rain and enabled)
    if (drewPrecipitation && lensDroplets && rainDebugView == 0 && renderMode == 1 && lensDropletsPS && sceneCopyTex && lensStrength > 0.01f)
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
    renderer::g_trackedState = trackedState;
    // Commit only a real draw. Failure leaves native rain available on the
    // next frame and allows the Present fallback to retry this frame.
    if (drewPrecipitation) composedThisFrame = true;
    if (renderMode == 1) rainCompositeReady = drewPrecipitation;
}

// Native draws are observations, not a per-frame boolean weather API. Missing
// draws in steady light rain must not repeatedly switch the shader off.
inline void UpdateWeatherObservation(DWORD now, bool focused, bool preserveDetection)
{
    if (nativeWeatherSeenThisFrame)
    {
        nativeWeatherMode = nativeWeatherSeenThisFrame;
        lastNativeWeatherTick = now;
        weatherSuspended = false;
    }
    else if ((!focused || preserveDetection) && nativeWeatherMode)
    {
        // Loading, Alt+Tab and F7 can throttle native particle emission.
        weatherSuspended = true;
    }
    else if (weatherSuspended && nativeWeatherMode)
    {
        // Restart the grace window after returning to the game.
        lastNativeWeatherTick = now;
        weatherSuspended = false;
    }
    else if (lastNativeWeatherTick && now - lastNativeWeatherTick > nativeWeatherHoldMs)
    {
        nativeWeatherMode = 0;
        lastNativeWeatherTick = 0;
    }
}

// Called after the rest of the world effects, before Present resets the frame.
// The normal pre-UI path is preferred; this is only for frames with no UI
// trigger (or a late native weather batch).
inline bool NeedsPresentFallback() noexcept
{
    return !composedThisFrame && enabled && effectEnabled && active && nativeWeatherMode != 0 &&
        (worldGeometrySeenThisFrame || nativeWeatherSeenThisFrame != 0);
}

inline void Present(IDirect3DDevice9*, bool preserveDetection = false)
{
    // F9 belongs to celestial diagnostics. Weather uses F10 so the two
    // independent modules cannot toggle each other in the same key press.
    bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    const bool focused = renderer::FrameContext::Current().wowFocused;
    if (hotkey && down && !keyDown && focused)
    {
        active = !active;
        Log(active ? "WeatherVisuals activated (F10)" : "WeatherVisuals paused (F10)");
    }
    keyDown = down;

    UpdateWeatherObservation(GetTickCount(), focused, preserveDetection);

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
