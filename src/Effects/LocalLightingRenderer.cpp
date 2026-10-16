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
#include "LocalLightingCore.h"

#include <windows.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
namespace
{
// Line-by-line mirror of src/Effects/LocalLightingCore.h: Footprint,
// TapWeights, ReconstructNormal and PoolShade. The relative edge band
// (.30/.85) is NormalTuning::relativeEdgeLow/High at their shipped defaults -
// the only part of that struct the shader spells out instead of receiving.
// Change it in both places in the same commit.
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
float4 texel:register(c5);        // xy=full-res texel, zw=THIS buffer's texel
float4 tuning:register(c6);       // x=intensity, y=debug, z=count
float4 normalTuning:register(c7); // x=min window width (world), y=edge floor, z=edge slope, w=growth per yard
float4 lightPosRadius[8]:register(c8);
float4 lightColorPower[8]:register(c16);
float4 lightDirectionCone[8]:register(c24);

float Linear(float raw) { return projection.y/(raw-projection.x); }
float3 World(float2 uv,float raw) {
 float z=Linear(raw);
 float3 v=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*z;
 return cameraPos.xyz+v.x*inv0.xyz+v.y*inv1.xyz+v.z*inv2.xyz;
}
// locallighting::TapWeights, one axis, in inverse depth. .x is the weight of
// the side the offset points away from, .y the other side.
float2 AxisWeights(float dA,float dB,float curv,float absolute,float surfA,float surfB) {
 float edge=smoothstep(absolute,absolute*4,curv)*smoothstep(.30,.85,curv/max(dA+dB,absolute));
 // Normalized by the larger side, not by their sum: the sum saturates, so a
 // side four times further off the surface than its partner kept a third of the
 // weight and dragged the tangent onto the occluder behind it.
 float asym=(dA-dB)/max(max(dA,dB),1e-4);
 // locallighting::TapVetoRange: a side standing this much further from the
 // receiver than its partner is on something else and does not get to steer the
 // tangent. Measured against the partner, because the partner is what proves
 // the window is on a surface - over ground at a grazing angle both sides take
 // in many yards of depth per pixel, and they do it together - while an
 // occluder's edge is exactly the asymmetry this catches.
 float veto=6*min(dA,dB)+absolute;
 float2 w=float2(surfA*(dA<=veto)*(1-edge*smoothstep(0,1,asym)),surfB*(dB<=veto)*(1-edge*smoothstep(0,1,-asym)));
 // A side that measured nothing - off the frame, or the sky beyond it - was
 // zeroed above; left to the edge test it reads as total disagreement and takes
 // the surviving side with it, which collapses the tangent and the normal to
 // zero. One surviving side is the whole axis.
 if(w.x<=0&&w.y>0) w=float2(0,1);
 if(w.y<=0&&w.x>0) w=float2(1,0);
 return w;
}
// locallighting::SnapUv. tex2D is POINT, so a tap returns the texel centre's
// depth; reconstructing that depth along the requested uv puts the point off
// the surface by the depth gradient times up to half a texel, which at grazing
// angles is yards. Every position this pass builds comes from a snapped uv, so
// every tap is a point the depth buffer actually describes.
float2 Snap(float2 uv) {
 float2 i=min(floor(saturate(uv)/texel.xy),1/texel.xy-1);
 return (i+.5)*texel.xy;
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 uv=Snap(uv);
 float raw=tex2D(depthMap,uv).r;
 if(raw>=.99990||tex2D(waterMask,uv).r>.25) return float4(0,0,0,0);
 float z=Linear(raw);
 float3 p=World(uv,raw);
 // locallighting::Footprint. One texel of the buffer being shaded, so the
 // normal is a resolvable estimate for the pixel it lights: a window pinned
 // to the FULL-resolution grid is narrower than a 0.75x texel, so the normal
 // field was undersampled against its own grid and its high-frequency part
 // aliased into grain. The world floor keeps the window off the terrain's
 // micro-relief close to the camera, where one texel is a fraction of an
 // inch of ground and the pool used to dapple with it.
 float2 spread=max(texel.zw*(1+max(z,0)*normalTuning.w),normalTuning.x*.5/max(z,.25)*projection.zw);
 // locallighting::TapOffset: the window is a whole number of depth texels per
 // side, so the two taps mirror the receiver. A window that lands one texel one
 // way and two the other reads as a step in any second difference however flat
 // the ground is - and at a grazing angle one texel is yards of depth.
 spread=max(round(spread/texel.xy),1)*texel.xy;
 // The window's taps are sampled and reconstructed from the same snapped uv, so
 // an off-frame tap shrinks the window to the frame instead of pairing the
 // border texel's depth with a ray that never touched it.
 float2 uvL=Snap(uv-float2(spread.x,0)),uvR=Snap(uv+float2(spread.x,0));
 float2 uvU=Snap(uv-float2(0,spread.y)),uvD=Snap(uv+float2(0,spread.y));
 float rawL=tex2D(depthMap,uvL).r,rawR=tex2D(depthMap,uvR).r;
 float rawU=tex2D(depthMap,uvU).r,rawD=tex2D(depthMap,uvD).r;
 float zL=Linear(rawL),zR=Linear(rawR),zU=Linear(rawU),zD=Linear(rawD);
 float3 pL=World(uvL,rawL),pR=World(uvR,rawR);
 float3 pU=World(uvU,rawU),pD=World(uvD,rawD);
 float3 toEye=normalize(cameraPos.xyz-p);
 // locallighting::TapWeights, per axis. An absolute yard threshold cannot
 // separate a silhouette from ground seen at a grazing angle: out there the
 // surface itself steps yards per pixel, so the old test read "edge" across
 // the whole far field and quietly degraded into a one-sided difference of
 // the terrain relief - an independent decision per pixel, which is what made
 // the pool read as noise. An edge is a window whose two sides disagree with
 // EACH OTHER more than with the surface's own slope; both halves are
 // continuous, so the tangent slides from central to one-sided instead of
 // switching.
 //
 // locallighting::InverseDepth: the test runs in 1/z, where every plane is
 // flat. In yards a flat road at the horizon is yards of depth per pixel and
 // its own perspective trips an absolute "this window straddles a step" test,
 // which is how flat ground at range came to be treated as an edge. The yard
 // tolerance is carried across (a yard at z is 1/z^2), so it keeps meaning the
 // same thing on the ground.
 //
 // The depth map also says which taps found a surface at all: a clear value is
 // the far plane, so a tap that reads it is the sky, not a neighbour. Handing
 // that ray to the tangent is what aimed lamp light along the horizon.
 float iC=1./max(z,.001),iL=1./max(zL,.001),iR=1./max(zR,.001);
 float iU=1./max(zU,.001),iD=1./max(zD,.001);
 float absolute=max(normalTuning.y,z*normalTuning.z)/max(z*z,.001);
 float2 wx=AxisWeights(abs(iL-iC),abs(iR-iC),abs(iL+iR-2*iC),absolute,step(rawL,.99990),step(rawR,.99990));
 float2 wy=AxisWeights(abs(iU-iC),abs(iD-iC),abs(iU+iD-2*iC),absolute,step(rawU,.99990),step(rawD,.99990));
 float wL=wx.x,wR=wx.y,wU=wy.x,wD=wy.y;
 // locallighting::ReconstructNormal: weighted central difference. With all
 // four weights at one this is exactly (far-near)/2 on both axes, i.e. the
 // central difference the pool had before; where a side is off-surface the
 // tangent becomes the one-sided difference on the surviving side.
 float3 tx=((p-pL)*wL+(pR-p)*wR)/max(wL+wR,1e-4);
 float3 ty=((p-pU)*wU+(pD-p)*wD)/max(wU+wD,1e-4);
 // A pixel of ground at the horizon can have its whole window in the sky: no
 // surface left to measure. Face the viewer there instead of handing the
 // cross product of two sky-ray remnants to the lamp as a normal.
 float3 c=cross(tx,ty);
 float3 n=dot(c,c)>1e-12?normalize(c):-toEye;
 if(dot(n,toEye)<0)n=-n;
 float intact=saturate((wL+wR+wU+wD)*.25);
 float3 sum=0,sourceDebug=0; float visibilityDebug=0; int count=(int)tuning.z;
 [loop] for(int i=0;i<8;++i) {
  if(i>=count) break;
  float3 toL=lightPosRadius[i].xyz-p; float dist=length(toL); float radius=lightPosRadius[i].w;
  float3 l=toL/max(dist,.001); float fall=saturate(1-dist/max(radius,.01));
  float attenuation=fall*fall/(1+dist*dist*.035);
  float cone=lightDirectionCone[i].w;
  if(cone>-0.5) attenuation*=smoothstep(cone,min(1.0,cone+.12),dot(-l,normalize(lightDirectionCone[i].xyz)));
  float diffuse=saturate(dot(n,l));
  // locallighting::PoolShade. The highlight is scaled by how much of the
  // window survived as one surface: pow(24) turns one compromised normal into
  // one bright speck, and specks are what every other smoothing step fails to
  // remove. `intact` is one on an untouched window, so flat ground is
  // unchanged. LIGHT DEBUG 4 shows it on its own.
  float spec=pow(saturate(dot(reflect(-l,n),toEye)),24)*.055*intact;
  float visibility=1; // Preserve native shadows; no duplicate screen-depth shadows.
  sum+=lightColorPower[i].rgb*lightColorPower[i].w*(diffuse+spec)*attenuation*visibility;
  sourceDebug+=lightColorPower[i].rgb*lightColorPower[i].w*fall*fall;
  visibilityDebug+=attenuation*(1-visibility);
 }
 sum*=tuning.x;
 if(tuning.y>.5&&tuning.y<1.5) return float4(sourceDebug,z);
 if(tuning.y>1.5&&tuning.y<2.5) return float4(sum,z);
 if(tuning.y>2.5&&tuning.y<3.5) return float4(saturate(visibilityDebug).xxx,z);
 if(tuning.y>3.5) return float4(intact.xxx,z);
 return float4(min(sum,3.0),z);
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
 float2 o[9]={float2(-1,-1),float2(0,-1),float2(1,-1),float2(-1,0),float2(0,0),
              float2(1,0),float2(-1,1),float2(0,1),float2(1,1)};
 float3 light=0; float sum=0;
 [unroll]for(int i=0;i<9;++i){
  float2 q=(base+o[i]+.5)*texel.xy; float4 s=tex2D(lightMap,q);
  // locallighting::CompositeSpatialWeight/CompositeDepthWeight. The spatial
  // kernel is a 1.5-texel tent over a 3x3 window instead of a bilinear 2x2:
  // a lamp pool is a smooth field, so the wider kernel costs nothing to look
  // at and averages away whatever grain the depth-derived normals still
  // carry. Support has to be above 1.0 or the outer ring weighs exactly zero
  // at every subpixel phase and the window collapses back to bilinear.
  // The depth mask must keep a floor: without it a tap window that straddles
  // a depth edge goes winner-takes-all and the pool snaps in 1-2 px steps
  // along the light-map grid (the "lines" at sub-1.0 quality). The floor
  // keeps the blend continuous everywhere; the masked-out side of a silhouette
  // still only contributes its (tiny) share, halved from the 2x2 value so
  // twice the taps cannot twice the leak.
  float2 spatial=float2(max(0,1.5-abs(o[i].x-f.x)),max(0,1.5-abs(o[i].y-f.y)));
  float kernel=spatial.x*spatial.y;
  float d=abs(s.a-receiverZ);
  float w=max(kernel*exp(-d/max(tolerance,.01)*3),kernel*0.01);
  light+=s.rgb*w;sum+=w;
 }
 // sum is >= .01 by construction of the floor, so this is always a bounded
 // weighted blend - no argmin fallback, hence no hard switch line anywhere.
 light=light/max(sum,.01);
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
    // zw are the texels of the buffer this pass is drawing into, not of the
    // scene: the normal's window is sized against them (locallighting::
    // Footprint), so the estimate always covers at least the pixel it lights.
    const float texel[4] = { 1.f / desc.Width,1.f / desc.Height,1.f / m_lightWidth,1.f / m_lightHeight };
    const float tuning[4] = { manager.IntensityScale()*f.environment[Lighting],float(manager.DebugMode()),
        float(count),float(occluded) };
    // One description of the normal's tuning, shared with the CPU mirror the
    // regression test runs (src/Effects/LocalLightingCore.h). LIGHT SMOOTHING
    // scales the world floor of the window; the edge scales stay fixed.
    const locallighting::NormalTuning normal = locallighting::DefaultNormalTuning(manager.NormalSpread());
    const float normalTuning[4] = { normal.minWorldSpread, normal.edgeFloor, normal.edgeSlope,
        normal.distanceGrowth };
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
    device->SetPixelShaderConstantF(7, normalTuning, 1);
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
