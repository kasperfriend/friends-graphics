#include "LocalLightingRenderer.h"

#include <algorithm>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include "../D3D9/DepthCapture.h"
#include "../D3D9/ScopedPassState.h"
#include "../D3D9/ShaderBytecodeCache.h"
#include "../Lighting/LocalLightManager.h"

#include <windows.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
namespace
{
const char* kSurfaceSource = R"HLSL(
sampler2D depthMap:register(s0);
// s1 is read through a LINEAR filter: the water gate must not inherit the
// 1-2 px jag of a point-sampled low-res tap (the edge of the excluded
// region follows the mask's .25 iso-line instead of a texel grid). s0 stays
// POINT - the stored alpha is the receiver's own depth and the composite's
// silhouette mask depends on it being exact; INTZ also must not be linear-
// filtered, which is driver-dependent.
sampler2D waterMask:register(s1);
float4 cameraPos:register(c0);
float4 projection:register(c1); // x=A,y=B,z=P00,w=P11
float4 inv0:register(c2); float4 inv1:register(c3); float4 inv2:register(c4);
float4 texel:register(c5);       // xy=full-res texel, z=count, w=occluded count
float4 tuning:register(c6);      // x=intensity, y=debug
float4 lightPosRadius[8]:register(c8);
float4 lightColorPower[8]:register(c16);
float4 lightDirectionCone[8]:register(c24);

float Linear(float raw) { return projection.y/(raw-projection.x); }
float3 World(float2 uv,float raw) {
 float z=Linear(raw);
 float3 v=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*z;
 return cameraPos.xyz+v.x*inv0.xyz+v.y*inv1.xyz+v.z*inv2.xyz;
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float raw=tex2D(depthMap,uv).r;
 if(raw>=.99990||tex2D(waterMask,uv).r>.25) return float4(0,0,0,0);
 float3 p=World(uv,raw);
 float dl=tex2D(depthMap,uv-float2(texel.x,0)).r,dr=tex2D(depthMap,uv+float2(texel.x,0)).r;
 float du=tex2D(depthMap,uv-float2(0,texel.y)).r,dd=tex2D(depthMap,uv+float2(0,texel.y)).r;
 float3 x0=World(uv-float2(texel.x,0),dl),x1=World(uv+float2(texel.x,0),dr);
 float3 y0=World(uv-float2(0,texel.y),du),y1=World(uv+float2(0,texel.y),dd);
 // Central differences average both sides of the tap, so the pool no longer
 // dapples and shimmers on textured ground the way a one-sided gradient
 // does. A tap window that straddles a depth edge must fall back to the
 // near side or the cliff face lights up from the wrong direction. The
 // fallback kicks in only once one side is already a clear step away, so
 // it fires at the cliff line itself - where the geometry actually turns
 // - instead of mid-pool, where the old dL==dR switch landed.
 float z=Linear(raw);
 float edge=max(.12,z*.008);
 float dL=abs(Linear(dl)-z),dR=abs(Linear(dr)-z);
 float3 px=max(dL,dR)<edge?(x0+x1)*.5:(dL<dR?x0:x1);
 float dU=abs(Linear(du)-z),dD=abs(Linear(dd)-z);
 float3 py=max(dU,dD)<edge?(y0+y1)*.5:(dU<dD?y0:y1);
 float3 n=normalize(cross(px-p,py-p));
 float3 toEye=normalize(cameraPos.xyz-p); if(dot(n,toEye)<0)n=-n;
 float3 sum=0,sourceDebug=0; float visibilityDebug=0; int count=(int)texel.z; int occ=(int)texel.w;
 [loop] for(int i=0;i<8;++i) {
  if(i>=count) break;
  float3 toL=lightPosRadius[i].xyz-p; float dist=length(toL); float radius=lightPosRadius[i].w;
  float3 l=toL/max(dist,.001); float fall=saturate(1-dist/max(radius,.01));
  float attenuation=fall*fall/(1+dist*dist*.035);
  float cone=lightDirectionCone[i].w;
  if(cone>-0.5) attenuation*=smoothstep(cone,min(1.0,cone+.12),dot(-l,normalize(lightDirectionCone[i].xyz)));
  float diffuse=saturate(dot(n,l));
  float spec=pow(saturate(dot(reflect(-l,n),toEye)),24)*.055;
  float visibility=1; // Preserve native shadows; no duplicate screen-depth shadows.
  sum+=lightColorPower[i].rgb*lightColorPower[i].w*(diffuse+spec)*attenuation*visibility;
  sourceDebug+=lightColorPower[i].rgb*lightColorPower[i].w*fall*fall;
  visibilityDebug+=attenuation*(1-visibility);
 }
 sum*=tuning.x;
 if(tuning.y>.5&&tuning.y<1.5) return float4(sourceDebug,Linear(raw));
 if(tuning.y>1.5&&tuning.y<2.5) return float4(sum,Linear(raw));
 if(tuning.y>2.5&&tuning.y<3.5) return float4(saturate(visibilityDebug).xxx,Linear(raw));
 return float4(min(sum,3.0),Linear(raw));
})HLSL";

const char* kCompositeSource = R"HLSL(
sampler2D sceneMap:register(s0); sampler2D lightMap:register(s1);
sampler2D depthMap:register(s2); sampler2D waterMask:register(s3);
float4 texel:register(c0); // xy=light texel, zw unused
float4 tuning:register(c1); // x=debug mode
float4 projection:register(c2);
float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
 float4 scene=tex2D(sceneMap,uv); float raw=tex2D(depthMap,uv).r;
 if(raw>=.99990||tex2D(waterMask,uv).r>.25)return scene;
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
 float3 light=0; float sum=0;
 [unroll]for(int i=0;i<4;++i){
  float2 q=(base+o[i]+.5)*texel.xy; float4 s=tex2D(lightMap,q);
  // The depth mask must keep a bilinear floor: without it a tap window that
  // straddles a depth edge goes winner-takes-all and the pool snaps in
  // 1-2 px steps along the light-map grid (the "lines" at sub-1.0 quality).
  // The floor keeps the blend continuous everywhere; the masked-out side of
  // a silhouette still only contributes its (tiny) bilinear share.
  float d=abs(s.a-receiverZ); float2 aw=1-abs(o[i]-f);
  float w=max(aw.x*aw.y*exp(-d/max(tolerance,.01)*3),aw.x*aw.y*0.02);
  light+=s.rgb*w;sum+=w;
 }
 // sum is >= .02 by construction of the floor, so this is always a bounded
 // weighted blend - no argmin fallback, hence no hard switch line anywhere.
 light=light/max(sum,.02);
 if(tuning.x>.5)return float4(light,1);
 // Half a level of fixed dither on the way into the 8-bit scene: a lamp
 // pool's smooth falloff otherwise quantizes into concentric rings on the
 // ground around the light, one ring per output level. The pattern is
 // static per pixel, so it reads as texture, not flicker.
 return float4(scene.rgb+light+(frac(52.9829189*frac(dot(vpos,float2(.06711056,.00583715))))-.5)/255.0,scene.a);
})HLSL";

bool Compile(IDirect3DDevice9* device, const char* source, const std::wstring& cacheDir, IDirect3DPixelShader9** shader, std::string& error)
{
    // On-disk bytecode cache first (renderer::shaderbytecode): a repeat
    // launch skips fxc entirely, and a cached blob the driver refuses
    // falls back to one recompile, keeping the error text as before.
    ComPtr<ID3DBlob> blob;
    return renderer::shaderbytecode::GetShader(cacheDir, source, "main", "ps_3_0", blob,
        [device, shader](ID3DBlob* b)
        { return SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(b->GetBufferPointer()), shader)); },
        &error);
}
}

LocalLightingRenderer& LocalLightingRenderer::Instance()
{
    static LocalLightingRenderer renderer;
    return renderer;
}

void LocalLightingRenderer::Configure(const std::wstring& basePath)
{
    m_logPath = basePath + L"LocalLighting.log";
    m_shaderCacheDir = renderer::shaderbytecode::Dir(basePath);
    m_retry = RetryGate{};
}

void LocalLightingRenderer::Log(const std::string& message) const
{
    if (m_logPath.empty()) return;
    std::ofstream(std::filesystem::path(m_logPath), std::ios::app) << message << '\n';
}

void LocalLightingRenderer::Reset(IDirect3DDevice9* device)
{
    if (m_owner && device && m_owner != device) return;
    m_lightSurface.Reset(); m_lightTexture.Reset();
    m_litSurface.Reset(); m_litTexture.Reset();
    m_surfaceShader.Reset(); m_compositeShader.Reset();
    m_owner = nullptr; m_width = m_height = m_lightWidth = m_lightHeight = 0;
    m_format = D3DFMT_UNKNOWN; m_quality = -1; m_retry = RetryGate{};
    m_shaderErrorLogged = false; m_resourceErrorLogged = false;
}

bool LocalLightingRenderer::EnsureShaders(IDirect3DDevice9* device)
{
    if (m_surfaceShader && m_compositeShader) return true;
    std::string error;
    if (!Compile(device, kSurfaceSource, m_shaderCacheDir, m_surfaceShader.GetAddressOf(), error) ||
        !Compile(device, kCompositeSource, m_shaderCacheDir, m_compositeShader.GetAddressOf(), error))
    {
        m_surfaceShader.Reset(); m_compositeShader.Reset();
        if (!m_shaderErrorLogged) {
            m_shaderErrorLogged = true;
            Log("Dynamic lighting shaders unavailable, will retry: " + error);
        }
        return false;
    }
    m_shaderErrorLogged = false;
    return true;
}

void LocalLightingRenderer::NoteFailure(const std::string& reason)
{
    // Logged once per failure streak; the retry gate handles the rest.
    if (m_retry.Fail(GetTickCount64(), kOptionalPassRetryMs) && !m_resourceErrorLogged) {
        m_resourceErrorLogged = true;
        Log(reason + "; feature skipped and retried without affecting the scene");
    }
}

bool LocalLightingRenderer::EnsureResources(IDirect3DDevice9* device, uint32_t width, uint32_t height, D3DFORMAT format, int quality)
{
    const float scale = quality <= 0 ? 0.5f : (quality == 1 ? 0.75f : 1.f);
    const uint32_t lw = std::max(1u, uint32_t(width * scale));
    const uint32_t lh = std::max(1u, uint32_t(height * scale));
    if (m_owner == device && m_width == width && m_height == height && m_lightWidth == lw &&
        m_lightHeight == lh && m_format == format && m_quality == quality && m_lightTexture && m_litTexture) return true;

    m_lightSurface.Reset(); m_lightTexture.Reset(); m_litSurface.Reset(); m_litTexture.Reset();
    m_owner = device; m_width = width; m_height = height; m_lightWidth = lw; m_lightHeight = lh;
    m_format = format; m_quality = quality;
    D3DFORMAT lightFormat = D3DFMT_A16B16G16R16F;
    // Alpha now stores linear receiver depth, which cannot be represented in
    // an 8-bit normalized fallback. Fail this optional pass instead of leaking
    // another surface's lighting across silhouettes on unsupported hardware.
    if (FAILED(device->CreateTexture(lw, lh, 1, D3DUSAGE_RENDERTARGET, lightFormat, D3DPOOL_DEFAULT, m_lightTexture.GetAddressOf(), nullptr))) return false;
    if (FAILED(m_lightTexture->GetSurfaceLevel(0, m_lightSurface.GetAddressOf())) ||
        FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, m_litTexture.GetAddressOf(), nullptr)) ||
        FAILED(m_litTexture->GetSurfaceLevel(0, m_litSurface.GetAddressOf()))) return false;
    return true;
}

void LocalLightingRenderer::DrawQuad(IDirect3DDevice9* device, uint32_t width, uint32_t height)
{
    struct V { float x, y, z, rhw, u, v; };
    const float w = float(width) - .5f, h = float(height) - .5f;
    V q[] = { {-.5f,-.5f,0,1,0,0},{w,-.5f,0,1,1,0},{-.5f,h,0,1,0,1},{w,h,0,1,1,1} };
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

bool LocalLightingRenderer::Render(IDirect3DDevice9* device, FrameContext& f)
{
    auto& manager = LocalLightManager::Instance();
    manager.SelectForFrame(f.cameraPosition, { f.inverseView.m[2][0], f.inverseView.m[2][1], f.inverseView.m[2][2] });
    const auto& lights = manager.Selected();
    if (!manager.Enabled() || lights.empty() || !device || !f.cameraValid || !f.depthAvailable || !f.sceneColor) return false;
    if (!m_retry.Open(GetTickCount64())) return false;
    D3DSURFACE_DESC desc{};
    if (!f.sceneSurface || FAILED(f.sceneSurface->GetDesc(&desc)) || !EnsureShaders(device) ||
        !EnsureResources(device, desc.Width, desc.Height, desc.Format, manager.Quality()))
    {
        NoteFailure("Dynamic lighting render targets unavailable");
        return false;
    }
    m_retry.Succeed();
    m_resourceErrorLogged = false;

    ScopedPassState state(device);
    DepthCapture::Instance().RawSetDepth(device, nullptr);
    device->SetVertexShader(nullptr);
    for (auto rs : { D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_SRGBWRITEENABLE,D3DRS_ALPHABLENDENABLE })
        device->SetRenderState(rs, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    for (DWORD s = 0; s < 4; ++s)
    {
        device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    // The surface pass reads the water gate through a LINEAR filter so its
    // edge is not a 0.75x texel grid; the composite pass below needs s1 back
    // on POINT.
    device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

    D3DVIEWPORT9 low{ 0,0,m_lightWidth,m_lightHeight,0,1 };
    device->SetViewport(&low); device->SetRenderTarget(0, m_lightSurface.Get());
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
    device->SetPixelShader(m_surfaceShader.Get()); device->SetTexture(0, f.depthTexture); device->SetTexture(1, f.waterMaskTexture);
    const float camera[4] = { f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,0 };
    const float inv[3][4] = { {f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},{f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},{f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0} };
    const int maxLights = manager.Quality() <= 0 ? 4 : 8;
    const int count = std::min<int>(maxLights, int(lights.size()));
    const int occluded = std::min(count, manager.Quality() <= 0 ? 2 : 4);
    const float texel[4] = { 1.f / desc.Width,1.f / desc.Height,float(count),float(occluded) };
    const float tuning[4] = { manager.IntensityScale()*f.environment[Lighting],float(manager.DebugMode()),0,0 };
    // c8..c31 are contiguous in the shader (pos c8, color c16, direction
    // c24), so the whole light table is one constant call, not three.
    float lightData[24][4]{};
    for (int i = 0; i < count; ++i)
    {
        lightData[i][0] = lights[i].position.x; lightData[i][1] = lights[i].position.y; lightData[i][2] = lights[i].position.z; lightData[i][3] = lights[i].radius;
        lightData[8+i][0] = lights[i].color.x; lightData[8+i][1] = lights[i].color.y; lightData[8+i][2] = lights[i].color.z; lightData[8+i][3] = lights[i].intensity;
        lightData[16+i][0] = lights[i].direction.x; lightData[16+i][1] = lights[i].direction.y; lightData[16+i][2] = lights[i].direction.z;
        lightData[16+i][3] = (lights[i].flags & LocalLightSpot) ? lights[i].outerCone : -1.f;
    }
    // This pass samples raw INTZ, unlike atmosphere's normalized boundary.
    // Fold the viewport transform into A/B so both World and Visibility use
    // exactly the depth convention that WoW wrote (including MaxZ < 1).
    const float depthRange = std::max(f.depthMaxZ - f.viewport.MinZ, .001f);
    const float projection[4] = { f.viewport.MinZ + f.projUnpack[0] * depthRange,
        f.projUnpack[1] * depthRange, f.projUnpack[2], f.projUnpack[3] };
    device->SetPixelShaderConstantF(0, camera, 1); device->SetPixelShaderConstantF(1, projection, 1);
    device->SetPixelShaderConstantF(2, inv[0], 3); device->SetPixelShaderConstantF(5, texel, 1); device->SetPixelShaderConstantF(6, tuning, 1);
    device->SetPixelShaderConstantF(8, lightData[0], 24);
    DrawQuad(device, m_lightWidth, m_lightHeight);
    device->SetTexture(0, nullptr); device->SetTexture(1, nullptr);
    device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

    D3DVIEWPORT9 full{ 0,0,desc.Width,desc.Height,0,1 };
    device->SetViewport(&full); device->SetRenderTarget(0, m_litSurface.Get()); device->SetPixelShader(m_compositeShader.Get());
    device->SetTexture(0, f.sceneColor); device->SetTexture(1, m_lightTexture.Get()); device->SetTexture(2, f.depthTexture); device->SetTexture(3, f.waterMaskTexture);
    // c0..c2 contiguous: one call.
    const float compositeData[3][4] = {
        { 1.f / m_lightWidth,1.f / m_lightHeight,1.f / m_width,1.f / m_height },
        { (manager.DebugMode() >= 1 && manager.DebugMode() <= 3) ? 1.f : 0.f,0,0,0 },
        { projection[0], projection[1], projection[2], projection[3] } };
    device->SetPixelShaderConstantF(0, compositeData[0], 3);
    DrawQuad(device, desc.Width, desc.Height);
    for (DWORD s = 0; s < 4; ++s) device->SetTexture(s, nullptr);
    f.sceneColor = m_litTexture.Get(); f.sceneSurface = m_litSurface.Get();
    return true;
}
}
