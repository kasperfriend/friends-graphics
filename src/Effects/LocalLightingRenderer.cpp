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
// TapWeights, ReconstructNormal and PoolShade. Change it in both places in
// the same commit.
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
// the side the offset points away from, .y the other side. i1X are the inner
// taps (the tangent distance), i2X the outer taps at twice the distance (the
// step test's other point).
float2 AxisWeights(float iL,float iR,float iC,float iL2,float iR2,float absolute,
                   float surfL,float surfR,float surf2L,float surf2R) {
 // locallighting::SideStep: a per-side second difference - the plane's own
 // slope cancels out of it, so it is exactly |J|, the depth jump of whatever
 // foreign surface that side reached onto, and it says WHICH side that is. A
 // three-tap window cannot: the slope step and the jump trade off exactly, and
 // a rule keyed on their comparison - or on the sign of the deviation gap -
 // guesses wrong the moment a nearby object's jump comes up small against the
 // grazing slope. That is what painted shape-copies of nearby characters and
 // objects into the pool, and dark streaks where the guess flipped.
 // Named `drop`, not `step`: `step` is an HLSL intrinsic.
 // Does this window straddle a step at all: the two-sided disagreement
 // (exactly a foreign tap's jump - the plane's slope cancels out of it)
 // against BOTH scales the window knows - the yard noise floor, and the
 // window's own one-sided changes, which are the terrain's own relief. The
 // relief comparison keeps the pool following real ground; the band closes
 // over a step at once instead of the old [.30,.85] slide that left half of a
 // nearby object's tap steering the tangent - the shape-copies.
 float curv=abs(iL+iR-2*iC);
 float disagreements=abs(iL-iC)+abs(iR-iC);
 float fire=smoothstep(absolute,absolute*4,curv)*smoothstep(.25,.55,curv/max(disagreements,absolute));
 // Which side carried the step: a per-side second difference
 // (locallighting::SideStep). A step stands alone on its side; a genuine
 // curve shows up on both sides alike. Named `drop`, not `step`.
 float qL=surf2L>0?abs(iL2-2*iL+iC):0.;
 float qR=surf2R>0?abs(iR2-2*iR+iC):0.;
 float dropL=fire*smoothstep(.7,.9,qL/max(qL+qR,absolute));
 float dropR=fire*smoothstep(.7,.9,qR/max(qL+qR,absolute));
 // locallighting::TapWeights: a side that measured nothing - the frame clamp
 // of an off-frame window (surf is 0 for a tap that snapped onto the
 // receiver's own texel), or the sky beyond it - hands the axis to the other
 // side whole. The drops only arbitrate between two measured sides.
 if(surfL<=0||surfR<=0) return float2(surfL>0?1.:0.,surfR>0?1.:0.);
 return float2(1-dropL,1-dropR);
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
 // The outer taps at twice the distance feed the step test only - no world
 // point is reconstructed from them.
 float2 uvL2=Snap(uv-float2(spread.x*2,0)),uvR2=Snap(uv+float2(spread.x*2,0));
 float2 uvU2=Snap(uv-float2(0,spread.y*2)),uvD2=Snap(uv+float2(0,spread.y*2));
 float rawL=tex2D(depthMap,uvL).r,rawR=tex2D(depthMap,uvR).r;
 float rawU=tex2D(depthMap,uvU).r,rawD=tex2D(depthMap,uvD).r;
 float rawL2=tex2D(depthMap,uvL2).r,rawR2=tex2D(depthMap,uvR2).r;
 float rawU2=tex2D(depthMap,uvU2).r,rawD2=tex2D(depthMap,uvD2).r;
 float zL=Linear(rawL),zR=Linear(rawR),zU=Linear(rawU),zD=Linear(rawD);
 float zL2=Linear(rawL2),zR2=Linear(rawR2),zU2=Linear(rawU2),zD2=Linear(rawD2);
 float3 pL=World(uvL,rawL),pR=World(uvR,rawR);
 float3 pU=World(uvU,rawU),pD=World(uvD,rawD);
 float3 toEye=normalize(cameraPos.xyz-p);
 // locallighting::TapWeights, per axis. An absolute yard threshold cannot
 // separate a silhouette from ground seen at a grazing angle: out there the
 // surface itself steps yards per pixel, so the old test read "edge" across
 // the whole far field and quietly degraded into a one-sided difference of
 // the terrain relief - an independent decision per pixel, which is what made
 // the pool read as noise. The step test is a per-side second difference over
 // the outer taps (locallighting::SideStep): slope-independent, and it names
 // the side the step is on instead of leaving the two sides to be guessed
 // apart. Both halves are continuous, so the tangent slides from central to
 // one-sided instead of switching.
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
 // that ray to the tangent is what aimed lamp light along the horizon. So does
 // a tap that snapped onto the receiver's own texel: the frame clamp of an
 // off-frame window returns that texel's depth with zero deviation, which
 // reads as the flattest possible neighbour - locallighting::TapSurvival.
 float surfL=step(rawL,.99990)*step(1e-20,dot(uvL-uv,uvL-uv));
 float surfR=step(rawR,.99990)*step(1e-20,dot(uvR-uv,uvR-uv));
 float surfU=step(rawU,.99990)*step(1e-20,dot(uvU-uv,uvU-uv));
 float surfD=step(rawD,.99990)*step(1e-20,dot(uvD-uv,uvD-uv));
 float surfL2=step(rawL2,.99990)*step(1e-20,dot(uvL2-uv,uvL2-uv));
 float surfR2=step(rawR2,.99990)*step(1e-20,dot(uvR2-uv,uvR2-uv));
 float surfU2=step(rawU2,.99990)*step(1e-20,dot(uvU2-uv,uvU2-uv));
 float surfD2=step(rawD2,.99990)*step(1e-20,dot(uvD2-uv,uvD2-uv));
 // locallighting::ReconstructNormal, horizon-flight veto. The window is a
 // fixed UV span, but its world size depends on the ray: the sideways taps
 // land a world window away, while the up tap of a steep down-look is the
 // same ray barely above the horizon, which lands tens of yards out and
 // turns the up-axis tangent into a long chord of the terrain - its
 // direction is the relief's average over that span, not the local slope.
 // That is a soft wrong-light band around everything the pool lights, and
 // it grows with LIGHT SMOOTHING, because smoothing is exactly what widens
 // the UV window until it erases the ray's down-angle. The test is per
 // axis: sideways taps are closer than along-ray taps by construction, so a
 // tap is only foreign to the window when it outruns its own partner - and
 // it must outrun the receiver's own view depth: a tap further out than the
 // receiver is a ray skimming the horizon, while a tap that merely outruns
 // its partner can be one hitting a nearby relief bump (both taps within
 // the receiver's depth, and both are measured).
 {
  float vz=max(z,.25);
  float dL=length(pL-p),dR=length(pR-p),dU=length(pU-p),dD=length(pD-p);
  if(surfL>0&&surfR>0)
  {
   if(dL>2.5*dR&&dL>vz) surfL=0.;
   else if(dR>2.5*dL&&dR>vz) surfR=0.;
  }
  if(surfU>0&&surfD>0)
  {
   if(dU>2.5*dD&&dU>vz) surfU=0.;
   else if(dD>2.5*dU&&dD>vz) surfD=0.;
  }
 }
 float iC=1./max(z,.001),iL=1./max(zL,.001),iR=1./max(zR,.001);
 float iU=1./max(zU,.001),iD=1./max(zD,.001);
 float iL2=1./max(zL2,.001),iR2=1./max(zR2,.001),iU2=1./max(zU2,.001),iD2=1./max(zD2,.001);
 float absolute=max(normalTuning.y,z*normalTuning.z)/max(z*z,.001);
 float2 wx=AxisWeights(iL,iR,iC,iL2,iR2,absolute,surfL,surfR,surfL2,surfR2);
 float2 wy=AxisWeights(iU,iD,iC,iU2,iD2,absolute,surfU,surfD,surfU2,surfD2);
 float wL=wx.x,wR=wx.y,wU=wy.x,wD=wy.y;
 // locallighting::ReconstructNormal, near-object gate. The step test above
 // reads DEPTH only: an object standing beside the receiver has a depth like
 // the terrain a few yards away - the same jump, the same second difference
 // - and passes it as terrain, its tap steering the tangent and the pool
 // painting a shape-copy of the object. Wide smoothing windows make both
 // sides contest, so the copies grow with smoothing. Whatever the depth
 // says, a standing body is a metre or more off the local ground plane, and
 // a plane - however sloped - has exactly zero second difference in world
 // height. inv1 is the camera's sky direction; a side whose height second
 // difference AND whose own height both clear the window's relief scale is
 // foreign and slides to zero. A dead tap counts as the receiver's own
 // level, so the gate still fires when the foreign side is the only one
 // that measured a surface.
 float hL=surfL>0?dot(pL-p,inv1.xyz):0.,hR=surfR>0?dot(pR-p,inv1.xyz):0.;
 float hU=surfU>0?dot(pU-p,inv1.xyz):0.,hD=surfD>0?dot(pD-p,inv1.xyz):0.;
 float liveW=surfL+surfR+surfU+surfD;
 // The window's own spread is already named `spread` (in uv); this is the
 // mean world distance of the surviving taps, which sizes the gate.
 float gateSpread=(surfL*length(pL-p)+surfR*length(pR-p)+surfU*length(pU-p)+surfD*length(pD-p))/max(liveW,1.);
 float gtol=.1+.1*gateSpread;
 float curvX=abs(hL+hR),curvY=abs(hU+hD);
 wL*=1.-smoothstep(gtol,gtol*2,curvX)*smoothstep(gtol,gtol*2,abs(hL));
 wR*=1.-smoothstep(gtol,gtol*2,curvX)*smoothstep(gtol,gtol*2,abs(hR));
 wU*=1.-smoothstep(gtol,gtol*2,curvY)*smoothstep(gtol,gtol*2,abs(hU));
 wD*=1.-smoothstep(gtol,gtol*2,curvY)*smoothstep(gtol,gtol*2,abs(hD));
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
