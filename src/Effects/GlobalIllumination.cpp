#include "GlobalIllumination.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include "../D3D9/DepthCapture.h"
#include "../D3D9/ScopedRenderState.h"
#include "../Diagnostics/PerformanceProfiler.h"
#include "../Environment/LocationTuning.h"
#include "../Lighting/LocalLightManager.h"
#include "EnvironmentFogCapture.h"
#include "VolumetricClouds.h"

#include <windows.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
namespace
{
// Quarter-resolution bounce integration. Depth-derived normals (the same
// neighbour-pick the direct lamp pass uses), one shared Vogel spiral that
// feeds both the cavity occlusion and the hemisphere-weighted bounce, a
// hemispheric sky/ground ambient, and a soft diffuse-only re-evaluation of
// the selected dynamic lights so lamp pools bleed onto nearby surfaces.
//
// The radiance buffer this samples is the scene AFTER the cloud composite
// (cloud shadows included), the client's own native shadows, and the direct
// lamp pools - so the bounce respects all three without any extra lookup.
// The one explicit shadow sample below dims only the sky ambient under the
// moving deck, through the same projection the atmosphere uses.
const char* kGlobalIlluminationIntegrateSource = R"HLSL(
sampler2D depthMap:register(s0);
sampler2D radianceMap:register(s1);
sampler2D waterMask:register(s2);
sampler2D cloudShadow:register(s3);
float4 cameraPos:register(c0);
float4 projection:register(c1); // x=A,y=B (viewport-fused),z=P00,w=P11
float4 inv0:register(c2); float4 inv1:register(c3); float4 inv2:register(c4);
float4 texel:register(c5);      // xy=full-res texel, z=radius world units, w=tap count
float4 tuning:register(c6);     // x=master, y=debug, z=bounce scale, w=sky scale
float4 skyColor:register(c7);   // rgb=sky ambient, w=occlusion strength
float4 groundColor:register(c8);// rgb=ground bounce, w=cloud ambient shadow strength
float4 sunWorld:register(c9);   // xyz=world direction to light, w=sun-lean strength
float4 cloudArea:register(c10); // xy=origin, z=1/extent, w=elevation fade (0 without a map)
float4 cloudProjection:register(c11); // xy=light slope, z=cloud top, w=1/thickness
float4 depthInfo:register(c12);// x=viewport MaxZ, y=lamp scale, z=light count
float4 lightPosRadius[8]:register(c17);
float4 lightColorPower[8]:register(c25);
float4 lightDirectionCone[8]:register(c33);

float Linear(float raw) { return projection.y/(raw-projection.x); }
float3 World(float2 uv,float raw) {
 float z=Linear(raw);
 float3 v=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*z;
 return cameraPos.xyz+v.x*inv0.xyz+v.y*inv1.xyz+v.z*inv2.xyz;
}
float CloudTransmission(float3 p) {
 float2 uv=(p.xy-p.z*cloudProjection.xy-cloudArea.xy)*cloudArea.z;
 float edge=max(abs(uv.x-.5),abs(uv.y-.5));
 float2 fade=saturate(float2((.49-edge)*(1/.09),(cloudProjection.z-p.z)*cloudProjection.w));
 fade=fade*fade*(3-2*fade);
 return lerp(1,tex2Dlod(cloudShadow,float4(uv,0,0)).r,fade.x*fade.y*cloudArea.w);
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float raw=tex2D(depthMap,uv).r;
 // Geometry is bounded by viewport MaxZ while the D3D clear stays 1: the
 // midpoint keeps sky classification exact for every viewport range.
 float sky=step((depthInfo.x+1)*.5,raw);
 float water=tex2D(waterMask,uv).r;
 if(sky>.5||water>.35) return float4(0,0,0,0);
 float centerZ=Linear(raw);
 float3 p=World(uv,raw);
 float dl=tex2D(depthMap,uv-float2(texel.x,0)).r,dr=tex2D(depthMap,uv+float2(texel.x,0)).r;
 float du=tex2D(depthMap,uv-float2(0,texel.y)).r,dd=tex2D(depthMap,uv+float2(0,texel.y)).r;
 float3 px=abs(Linear(dl)-centerZ)<abs(Linear(dr)-centerZ)?World(uv-float2(texel.x,0),dl):World(uv+float2(texel.x,0),dr);
 float3 py=abs(Linear(du)-centerZ)<abs(Linear(dd)-centerZ)?World(uv-float2(0,texel.y),du):World(uv+float2(0,texel.y),dd);
 float3 n=normalize(cross(px-p,py-p));
 float3 toEye=normalize(cameraPos.xyz-p); if(dot(n,toEye)<0)n=-n;
 // World-sized footprint that shrinks with distance, clamped so a surface
 // against the camera cannot smear the whole buffer across one pixel.
 float radiusWorld=max(texel.z,.25);
 float2 radiusUv=min((radiusWorld/max(centerZ,.5))*projection.zw*.5,.045);
 float tapCount=max(texel.w,1);
 float occlusion=0,bounceW=0; float3 bounce=0;
 // Fixed golden-angle spiral: the phase never animates, so the result is
 // stable grain rather than flicker, and the bilateral upsample resolves it.
 [loop] for(int i=0;i<8;++i) {
  if(i>=int(tapCount+.5)) break;
  float fi=float(i)+.5;
  float r=sqrt(fi*.125);
  float a=fi*2.399963+.7;
  float2 tuv=uv+float2(cos(a),sin(a))*r*radiusUv;
  float tRaw=tex2Dlod(depthMap,float4(tuv,0,0)).r;
  float valid=(1-step((depthInfo.x+1)*.5,tRaw))*(1-step(.35,tex2Dlod(waterMask,float4(tuv,0,0)).r));
  float tz=Linear(tRaw);
  float dz=tz-centerZ;
  // Surroundings closer than the pixel hide the sky above it: creases and
  // corners darken while open ground keeps its ambient.
  occlusion+=valid*saturate((-dz-(.06+centerZ*.002))/radiusWorld);
  float3 toTap=World(tuv,tRaw)-p;
  float facing=saturate(dot(n,toTap/max(length(toTap),.001))*2);
  float bilateral=exp(-abs(dz)/max(max(.2,centerZ*.03),.05));
  float w=valid*facing*bilateral;
  bounce+=tex2Dlod(radianceMap,float4(tuv,0,0)).rgb*w;
  bounceW+=w;
 }
 float occ=saturate(occlusion/tapCount)*skyColor.w;
 float occFactor=1-occ;
 // WoW world-up is +Z: upward faces take the sky, downward faces the ground.
 float hemi=n.z*.5+.5; hemi=hemi*hemi*(3-2*hemi);
 float3 ambient=lerp(groundColor.rgb,skyColor.rgb,hemi);
 // The sky side toward the light is brighter; overcast flattens it on CPU.
 float lean=pow(saturate(dot(-toEye,sunWorld.xyz)*.5+.5),3)*sunWorld.w;
 ambient*=1+lean;
 // Under a cloud the whole sky dome dims, not just the sun disc.
 ambient*=lerp(1,CloudTransmission(p),groundColor.w);
 float3 bounced=bounce/max(bounceW,.001)*tuning.z*lerp(1,occFactor,.8);
 ambient*=occFactor*tuning.w;
 // Lamp bleed is diffuse-only and deliberately wider/softer than the direct
 // pass: it is the light that left the pool, not the pool itself.
 float3 lamps=0; int lights=int(depthInfo.z+.5);
 [loop] for(int li=0;li<8;++li) {
  if(li>=lights) break;
  float3 toL=lightPosRadius[li].xyz-p; float dist=length(toL);
  float radius=max(lightPosRadius[li].w*1.6,.5);
  float3 l=toL/max(dist,.001);
  float edge=saturate(1-dist/radius);
  float attenuation=edge*edge/(1+dist*dist*.02);
  float cone=lightDirectionCone[li].w;
  if(cone>-0.5) attenuation*=smoothstep(cone,min(1.0,cone+.12),dot(-l,normalize(lightDirectionCone[li].xyz)));
  lamps+=lightColorPower[li].rgb*lightColorPower[li].w*saturate(dot(n,l))*attenuation;
 }
 lamps*=depthInfo.y*lerp(1,occFactor,.35);
 float3 total=min((ambient+bounced+lamps)*tuning.x,2.5);
 if(tuning.y>1.5&&tuning.y<2.5) return float4(occ.xxx,centerZ);
 if(tuning.y>2.5) return float4(min(bounced*tuning.x,2.5),centerZ);
 return float4(total,centerZ);
})HLSL";

const char* kGlobalIlluminationCompositeSource = R"HLSL(
sampler2D sceneMap:register(s0); sampler2D giMap:register(s1);
sampler2D depthMap:register(s2); sampler2D waterMask:register(s3);
float4 texel:register(c0); // xy=low-res texel, zw=full-res texel
float4 tuning:register(c1); // x=debug passthrough, y=viewport MaxZ
float4 projection:register(c2); // x=A,y=B (viewport-fused)
float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
 float4 scene=tex2D(sceneMap,uv); float raw=tex2D(depthMap,uv).r;
 if(step((tuning.y+1)*.5,raw)>.5||tex2D(waterMask,uv).r>.25)return scene;
 float receiverZ=projection.y/(raw-projection.x);
 // Account for a continuous depth slope across a low-resolution texel.
 // Use the smaller one-sided differences so silhouettes do not inflate it.
 float2 fullTexel=texel.zw;
 float zl=projection.y/(tex2D(depthMap,uv-float2(fullTexel.x,0)).r-projection.x);
 float zr=projection.y/(tex2D(depthMap,uv+float2(fullTexel.x,0)).r-projection.x);
 float zu=projection.y/(tex2D(depthMap,uv-float2(0,fullTexel.y)).r-projection.x);
 float zd=projection.y/(tex2D(depthMap,uv+float2(0,fullTexel.y)).r-projection.x);
 float slope=min(abs(zl-receiverZ),abs(zr-receiverZ))+min(abs(zu-receiverZ),abs(zd-receiverZ));
 float tolerance=max(max(.12,receiverZ*.008),slope*max(texel.x/fullTexel.x,texel.y/fullTexel.y)*1.5);
 float2 size=1/texel.xy; float2 pixel=uv*size-.5; float2 base=floor(pixel); float2 f=frac(pixel);
 float2 o[4]={float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
 float3 gi=0; float sum=0; float best=1e9; float3 nearest=0;
 [unroll]for(int i=0;i<4;++i){
  float2 q=(base+o[i]+.5)*texel.xy; float4 s=tex2D(giMap,q);
  float d=abs(s.a-receiverZ); float2 aw=1-abs(o[i]-f); float w=max(aw.x*aw.y,.001)*exp(-d/max(tolerance,.01)*3);
  gi+=s.rgb*w;sum+=w;if(d<best){best=d;nearest=s.rgb;}
 }
 gi=sum>.02?gi/sum:(best<tolerance?nearest:float3(0,0,0));
 if(tuning.x>.5)return float4(gi,1);
 // Half a level of fixed dither on the way into the 8-bit scene: the smooth
 // ambient falloff would otherwise quantize into rings on flat ground. The
 // pattern is static per pixel, so it reads as texture, not flicker.
 return float4(scene.rgb+gi+(frac(52.9829189*frac(dot(vpos,float2(.06711056,.00583715))))-.5)/255.0,scene.a);
})HLSL";

bool Compile(IDirect3DDevice9* device, const char* source, IDirect3DPixelShader9** shader, std::string& error)
{
    ComPtr<ID3DBlob> blob, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "ps_3_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) || !blob)
    {
        if (errors) error.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        return false;
    }
    if (FAILED(device->CreatePixelShader(static_cast<const DWORD*>(blob->GetBufferPointer()), shader)))
    {
        error = "CreatePixelShader failed";
        return false;
    }
    return true;
}
}

GlobalIllumination& GlobalIllumination::Instance()
{
    static GlobalIllumination effect;
    return effect;
}

void GlobalIllumination::Configure(const std::wstring& basePath)
{
    m_iniPath = basePath + L"GraphicsEffects.ini";
    m_logPath = basePath + L"GlobalIllumination.log";
    m_retry = RetryGate{};
    ReloadTuning();
}

void GlobalIllumination::ReloadTuning()
{
    if (m_iniPath.empty()) return;
    auto read = [&](const wchar_t* key, int fallback) {
        return static_cast<int>(renderer::locationtuning::ReadInt(L"GlobalIllumination", key, fallback, m_iniPath.c_str()));
    };
    m_settings.enabled = read(L"Enabled", 1) != 0;
    m_settings.intensity = std::clamp(read(L"IntensityPercent", 100), 0, 200) * 0.01f;
    m_settings.skyAmbient = std::clamp(read(L"SkyAmbientPercent", 80), 0, 200) * 0.01f;
    m_settings.bounce = std::clamp(read(L"BouncePercent", 70), 0, 200) * 0.01f;
    m_settings.occlusion = std::clamp(read(L"OcclusionPercent", 70), 0, 100) * 0.01f;
    m_settings.lampBounce = std::clamp(read(L"LampBouncePercent", 60), 0, 200) * 0.01f;
    m_settings.radius = std::clamp(read(L"RadiusPercent", 100), 25, 300) * 0.01f;
    m_settings.followCloudShadows = read(L"FollowCloudShadows", 1) != 0;
    m_settings.followWeather = read(L"FollowWeather", 1) != 0;
    m_settings.quality = std::clamp(read(L"Quality", 1), 0, 2);
    m_settings.debugMode = std::clamp(read(L"DebugMode", 0), 0, 3);
}

void GlobalIllumination::Log(const std::string& message) const
{
    if (m_logPath.empty()) return;
    std::ofstream(std::filesystem::path(m_logPath), std::ios::app) << message << '\n';
}

void GlobalIllumination::Reset(IDirect3DDevice9* device)
{
    if (m_owner && device && m_owner != device) return;
    m_radianceSurface.Reset(); m_radianceTexture.Reset();
    m_giSurface.Reset(); m_giTexture.Reset();
    m_litSurface.Reset(); m_litTexture.Reset();
    m_integrateShader.Reset(); m_compositeShader.Reset();
    m_owner = nullptr; m_width = m_height = m_lowWidth = m_lowHeight = 0;
    m_format = D3DFMT_UNKNOWN; m_quality = -1; m_retry = RetryGate{};
    m_shaderErrorLogged = false; m_resourceErrorLogged = false;
}

bool GlobalIllumination::EnsureShaders(IDirect3DDevice9* device)
{
    if (m_integrateShader && m_compositeShader) return true;
    std::string error;
    if (!Compile(device, kGlobalIlluminationIntegrateSource, m_integrateShader.GetAddressOf(), error) ||
        !Compile(device, kGlobalIlluminationCompositeSource, m_compositeShader.GetAddressOf(), error))
    {
        m_integrateShader.Reset(); m_compositeShader.Reset();
        if (!m_shaderErrorLogged) {
            m_shaderErrorLogged = true;
            Log("Global illumination shaders unavailable, will retry: " + error);
        }
        return false;
    }
    m_shaderErrorLogged = false;
    return true;
}

void GlobalIllumination::NoteFailure(const std::string& reason)
{
    // Logged once per failure streak; the retry gate handles the rest.
    if (m_retry.Fail(GetTickCount64(), kOptionalPassRetryMs) && !m_resourceErrorLogged) {
        m_resourceErrorLogged = true;
        Log(reason + "; feature skipped and retried without affecting the scene");
    }
}

bool GlobalIllumination::EnsureResources(IDirect3DDevice9* device, uint32_t width, uint32_t height, D3DFORMAT format, int quality)
{
    const float scale = quality >= 2 ? 0.5f : 0.25f;
    const uint32_t lw = std::max(1u, uint32_t(width * scale));
    const uint32_t lh = std::max(1u, uint32_t(height * scale));
    if (m_owner == device && m_width == width && m_height == height && m_lowWidth == lw &&
        m_lowHeight == lh && m_format == format && m_quality == quality &&
        m_radianceTexture && m_giTexture && m_litTexture) return true;

    m_radianceSurface.Reset(); m_radianceTexture.Reset();
    m_giSurface.Reset(); m_giTexture.Reset();
    m_litSurface.Reset(); m_litTexture.Reset();
    m_owner = device; m_width = width; m_height = height; m_lowWidth = lw; m_lowHeight = lh;
    m_format = format; m_quality = quality;
    // The radiance buffer is a StretchRect copy of the scene, so it must be
    // the scene's own format; the GI buffer needs float alpha for linear
    // receiver depth, exactly like the direct lamp pass.
    if (FAILED(device->CreateTexture(lw, lh, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, m_radianceTexture.GetAddressOf(), nullptr)) ||
        FAILED(m_radianceTexture->GetSurfaceLevel(0, m_radianceSurface.GetAddressOf())))
        return false;
    if (FAILED(device->CreateTexture(lw, lh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, m_giTexture.GetAddressOf(), nullptr)))
        return false;
    if (FAILED(m_giTexture->GetSurfaceLevel(0, m_giSurface.GetAddressOf())) ||
        FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, m_litTexture.GetAddressOf(), nullptr)) ||
        FAILED(m_litTexture->GetSurfaceLevel(0, m_litSurface.GetAddressOf()))) return false;
    return true;
}

void GlobalIllumination::DrawQuad(IDirect3DDevice9* device, uint32_t width, uint32_t height)
{
    struct V { float x, y, z, rhw, u, v; };
    const float w = float(width) - .5f, h = float(height) - .5f;
    V q[] = { {-.5f,-.5f,0,1,0,0},{w,-.5f,0,1,1,0},{-.5f,h,0,1,0,1},{w,h,0,1,1,1} };
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

bool GlobalIllumination::Render(IDirect3DDevice9* device, FrameContext& f)
{
    // The dynamic-lamp selection is populated by LocalLightingRenderer at the
    // top of the same composite; reading it here (and never re-selecting)
    // keeps one smoothing cadence for both passes.
    const auto& manager = LocalLightManager::Instance();
    const auto& lights = manager.Selected();
    if (!m_settings.enabled || !device || !f.cameraValid || !f.depthAvailable || !f.sceneColor) return false;
    if (!m_retry.Open(GetTickCount64())) return false;
    D3DSURFACE_DESC desc{};
    if (!f.sceneSurface || FAILED(f.sceneSurface->GetDesc(&desc)) || !EnsureShaders(device) ||
        !EnsureResources(device, desc.Width, desc.Height, desc.Format, m_settings.quality))
    {
        NoteFailure("Global illumination render targets unavailable");
        return false;
    }

    // Radiance downsample first: a hardware bilinear StretchRect, no shader
    // cost. This is the bounce source - already cloud-shadowed, natively
    // shadowed and lamp-lit by the passes that ran before this one.
    if (FAILED(device->StretchRect(f.sceneSurface, nullptr, m_radianceSurface.Get(), nullptr, D3DTEXF_LINEAR)))
    {
        NoteFailure("Global illumination downsample failed");
        return false;
    }
    m_retry.Succeed();
    m_resourceErrorLogged = false;

    ScopedCpuTimer cpuTimer(PerfStage::GlobalIllumination);
    ScopedGpuTimer gpuTimer(device, GpuPerfStage::GlobalIllumination);
    ScopedRenderState state(device);
    DepthCapture::Instance().RawSetDepth(device, nullptr);
    device->SetVertexShader(nullptr);
    for (auto rs : { D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_SRGBWRITEENABLE,D3DRS_ALPHABLENDENABLE })
        device->SetRenderState(rs, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    for (DWORD s = 0; s < 4; ++s)
    {
        const bool linear = (s == 1 || s == 3);
        device->SetSamplerState(s, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        device->SetSamplerState(s, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    // Sky ambient in the zone's own palette: the captured authored fog
    // colour when the water draws provided one this frame (the same colour
    // the atmosphere and the cloud deck already use), otherwise a day/night
    // blend of the client's directional light over a cool floor.
    const auto& envFog = EnvironmentFogCapture::Instance().State();
    const float daylight = std::clamp(f.daylightFactor, 0.f, 1.f);
    const float moonlight = std::clamp(f.moonlightFactor, 0.f, 1.f);
    float sky[3];
    if (envFog.valid && envFog.colorValid)
    {
        sky[0] = envFog.ambientFogColor[0];
        sky[1] = envFog.ambientFogColor[1];
        sky[2] = envFog.ambientFogColor[2];
    }
    else
    {
        sky[0] = std::clamp(f.directionalLightColor.x * 0.5f * daylight + 0.16f * (0.3f + 0.7f * moonlight) + 0.03f, 0.02f, 1.f);
        sky[1] = std::clamp(f.directionalLightColor.y * 0.5f * daylight + 0.20f * (0.3f + 0.7f * moonlight) + 0.03f, 0.02f, 1.f);
        sky[2] = std::clamp(f.directionalLightColor.z * 0.5f * daylight + 0.28f * (0.3f + 0.7f * moonlight) + 0.04f, 0.02f, 1.f);
    }
    // Live overcast closes the sky down and flattens its colour, but only
    // when the deck actually rendered this frame: with clouds disabled or a
    // genuinely clear sky there is no lid to dim under. With FollowWeather
    // off the ambient stays at the clear-sky value regardless.
    const float overcast = (m_settings.followWeather && f.cloudTexture)
        ? VolumetricClouds::Instance().OvercastFactor() : 0.f;
    const float skyLum = sky[0] * 0.3f + sky[1] * 0.5f + sky[2] * 0.2f;
    for (int i = 0; i < 3; ++i)
        sky[i] = sky[i] + (skyLum - sky[i]) * (overcast * 0.35f);

    const float envLight = f.environment[Lighting];
    // Calibration: the sliders are artistic 0..2 multipliers on top of these
    // base gains, chosen so the shipped defaults lift shadows without
    // washing the client's own direct light out.
    const float master = m_settings.intensity * envLight;
    const float skyScale = 0.22f * m_settings.skyAmbient * (1.f - overcast * 0.5f);
    const float bounceScale = 0.35f * m_settings.bounce * (1.f - overcast * 0.25f);

    Vec3 sunW = f.directLightValid ? f.directLightDirectionWorld : f.sunDirectionWorld;
    const float sunLen = Length(sunW);
    sunW = sunLen > 1e-4f ? sunW * (1.f / sunLen) : Vec3{ 0.f, 0.f, 1.f };
    const float directI = f.directLightValid ? f.directLightIntensity : f.celestialIntensity;
    const bool isMoon = f.directLightValid ? f.directLightIsMoon : f.celestialIsMoon;
    float lean = std::clamp(directI, 0.f, 1.f) * (isMoon ? 0.18f : 0.35f) * (1.f - overcast * 0.7f);

    const auto& cloudSettings = VolumetricClouds::Instance().Settings();
    const float cloudStrength = (m_settings.followCloudShadows && f.cloudShadowTexture && cloudSettings.groundShadows)
        ? cloudSettings.groundShadowStrength : 0.f;

    const int maxLights = m_settings.quality <= 0 ? 4 : 8;
    const int count = std::min<int>(maxLights, int(lights.size()));
    const float lampScale = (manager.Enabled() && count > 0)
        ? 0.35f * m_settings.lampBounce * manager.IntensityScale() * envLight : 0.f;

    D3DVIEWPORT9 low{ 0,0,m_lowWidth,m_lowHeight,0,1 };
    device->SetViewport(&low); device->SetRenderTarget(0, m_giSurface.Get());
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
    device->SetPixelShader(m_integrateShader.Get());
    device->SetTexture(0, f.depthTexture); device->SetTexture(1, m_radianceTexture.Get());
    device->SetTexture(2, f.waterMaskTexture); device->SetTexture(3, f.cloudShadowTexture);

    const float camera[4] = { f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,0 };
    const float inv[3][4] = { {f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},{f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},{f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0} };
    // This pass samples raw INTZ, like the direct lamp pass. Fold the
    // viewport transform into A/B so reconstruction uses exactly the depth
    // convention WoW wrote (including MaxZ < 1).
    const float depthRange = std::max(f.depthMaxZ - f.viewport.MinZ, .001f);
    const float projection[4] = { f.viewport.MinZ + f.projUnpack[0] * depthRange,
        f.projUnpack[1] * depthRange, f.projUnpack[2], f.projUnpack[3] };
    const float radiusWorld = 2.5f * m_settings.radius * (m_settings.quality >= 2 ? 1.3f : 1.f);
    const float texel[4] = { 1.f / desc.Width, 1.f / desc.Height, radiusWorld, m_settings.quality <= 0 ? 4.f : 8.f };
    const float tuning[4] = { master, float(m_settings.debugMode), bounceScale, skyScale };
    const float skyAmbient[4] = { sky[0], sky[1], sky[2], m_settings.occlusion };
    const float ground[4] = { sky[0] * 0.42f, sky[1] * 0.36f, sky[2] * 0.30f, cloudStrength };
    const float sun[4] = { sunW.x, sunW.y, sunW.z, lean };
    const float cloudArea[4] = { f.cloudShadowArea.x, f.cloudShadowArea.y, f.cloudShadowArea.z,
        f.cloudShadowArea.w * (f.cloudShadowTexture ? 1.f : 0.f) };
    const float cloudProjection[4] = { f.cloudLightProjection.x, f.cloudLightProjection.y,
        f.cloudLightProjection.z + f.cloudLightProjection.w,
        1.f / std::max(f.cloudLightProjection.w, 1.f) };
    const float depthInfo[4] = { f.depthMaxZ, lampScale, float(count), 0 };
    float pos[8][4]{}, color[8][4]{}, direction[8][4]{};
    for (int i = 0; i < count; ++i)
    {
        pos[i][0] = lights[i].position.x; pos[i][1] = lights[i].position.y; pos[i][2] = lights[i].position.z; pos[i][3] = lights[i].radius;
        color[i][0] = lights[i].color.x; color[i][1] = lights[i].color.y; color[i][2] = lights[i].color.z; color[i][3] = lights[i].intensity;
        direction[i][0] = lights[i].direction.x; direction[i][1] = lights[i].direction.y; direction[i][2] = lights[i].direction.z;
        direction[i][3] = (lights[i].flags & LocalLightSpot) ? lights[i].outerCone : -1.f;
    }
    device->SetPixelShaderConstantF(0, camera, 1); device->SetPixelShaderConstantF(1, projection, 1);
    device->SetPixelShaderConstantF(2, inv[0], 3); device->SetPixelShaderConstantF(5, texel, 1);
    device->SetPixelShaderConstantF(6, tuning, 1); device->SetPixelShaderConstantF(7, skyAmbient, 1);
    device->SetPixelShaderConstantF(8, ground, 1); device->SetPixelShaderConstantF(9, sun, 1);
    device->SetPixelShaderConstantF(10, cloudArea, 1); device->SetPixelShaderConstantF(11, cloudProjection, 1);
    device->SetPixelShaderConstantF(12, depthInfo, 1);
    device->SetPixelShaderConstantF(17, pos[0], 8); device->SetPixelShaderConstantF(25, color[0], 8);
    device->SetPixelShaderConstantF(33, direction[0], 8);
    DrawQuad(device, m_lowWidth, m_lowHeight);
    for (DWORD s = 0; s < 4; ++s) device->SetTexture(s, nullptr);

    D3DVIEWPORT9 full{ 0,0,desc.Width,desc.Height,0,1 };
    device->SetViewport(&full); device->SetRenderTarget(0, m_litSurface.Get()); device->SetPixelShader(m_compositeShader.Get());
    device->SetTexture(0, f.sceneColor); device->SetTexture(1, m_giTexture.Get());
    device->SetTexture(2, f.depthTexture); device->SetTexture(3, f.waterMaskTexture);
    const float giTexel[4] = { 1.f / m_lowWidth,1.f / m_lowHeight,1.f / m_width,1.f / m_height };
    const float compositeTuning[4] = { m_settings.debugMode >= 1 ? 1.f : 0.f, f.depthMaxZ, 0, 0 };
    device->SetPixelShaderConstantF(0, giTexel, 1); device->SetPixelShaderConstantF(1, compositeTuning, 1);
    device->SetPixelShaderConstantF(2, projection, 1);
    DrawQuad(device, desc.Width, desc.Height);
    for (DWORD s = 0; s < 4; ++s) device->SetTexture(s, nullptr);
    f.sceneColor = m_litTexture.Get(); f.sceneSurface = m_litSurface.Get();
    return true;
}
}
