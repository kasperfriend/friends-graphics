#include "../Environment/LocationTuning.h"
#include "VolumetricClouds.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "../D3D9/DepthCapture.h"
#include "../D3D9/ScopedRenderState.h"
#include "../Diagnostics/PerformanceProfiler.h"
#include "EnvironmentFogCapture.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
namespace
{
// Raymarched cloud deck.
//
// The deck is one world-space slab (WoW world Z is up) whose density comes
// from the same tiling value-noise atlas the atmosphere already uses - no
// extra texture, no 3D volume texture (DX9 class hardware here is not
// guaranteed to have the bandwidth for one), and no readback. Only sky
// pixels are integrated: everything the client already drew keeps a
// transmittance of 1, so the composite pass below cannot touch solid
// geometry even where the quarter-resolution buffer bleeds across a
// silhouette.
//
// Lighting is the standard participating-medium pair: a short march toward
// the tracked sun/moon for self-shadowing (Beer), plus a powder term for
// the dark edges that pure Beer cannot reproduce, with a Henyey-Greenstein
// forward lobe so the sun side lights up when you look into it. Ambient is
// the zone's own captured fog colour, which keeps the deck in the same
// palette as the atmosphere instead of a fixed synthetic white.
const char* kCloudMarchSource = R"HLSL(
sampler2D depthMap:register(s0); sampler2D noiseMap:register(s1);
float4 cameraPos:register(c0);      // xyz=camera world position, w=elapsed seconds
float4 celestial:register(c1);      // xyz=world direction TOWARDS the tracked disc, w=intensity
float4 invView0:register(c2); float4 invView1:register(c3); float4 invView2:register(c4);
float4 projection:register(c5);     // x=A, y=B (raw->view z), z=P00, w=P11
float4 depthRange:register(c6);     // x=viewport MinZ, y=viewport depth range, z=sky threshold
float4 layer:register(c7);          // x=slab base Z, y=slab top Z, z=1/thickness, w=max march distance
float4 shape:register(c8);          // x=coverage, y=density, z=1/feature size, w=detail erosion
float4 wind:register(c9);           // xy=noise offset, z=vertical noise scale
float4 lightTuning:register(c10);   // x=absorption, y=direct gain, z=ambient gain, w=forward lobe
float4 sunColor:register(c11);      // rgb=direct colour, w=march steps
float4 ambientColor:register(c12);  // rgb=ambient colour, w=powder strength
float4 tuning:register(c13);        // x=horizon fade width, y=debug mode

// Two decorrelated slices of the tiling 2D atlas, interpolated along Z.
float Noise3(float3 p) {
 float z=floor(p.z),f=frac(p.z);f=f*f*(3-2*f);
 float2 offset=float2(.173,.317);
 float a=tex2Dlod(noiseMap,float4(p.xy+z*offset,0,0)).r;
 float b=tex2Dlod(noiseMap,float4(p.xy+(z+1)*offset,0,0)).r;
 return lerp(a,b,f);
}

// Base shape without the erosion octave: rounded bottom, flattening top.
// The light march uses this directly so self-shadowing stays cheap.
float Shape(float3 p) {
 float h=saturate((p.z-layer.x)*layer.z);
 float2 base=p.xy*shape.z+wind.xy;
 float broad=Noise3(float3(base,p.z*wind.z));
 float mid=Noise3(float3(base*2.27+float2(.41,.13),p.z*wind.z*1.6+3.7));
 float field=broad*.66+mid*.34;
 // The coverage slider is mapped across the MEASURED distribution of this
 // field (p2 = .27, p50 = .46, p98 = .64 for the shipped noise atlas), not
 // across 0..1: that is what makes 0 a genuinely clear sky, ~.4 a
 // half-covered one and 1 a solid overcast, instead of the whole usable
 // range hiding between .55 and .7 on the slider.
 float threshold=lerp(.70,.20,saturate(shape.x));
 float d=saturate((field-threshold)/.14);
 return d*smoothstep(0,.25,h)*(1-smoothstep(.62,1,h));
}

float Density(float3 p) {
 float d=Shape(p);
 float2 base=p.xy*shape.z*6.3-wind.xy*1.7;
 float erosion=Noise3(float3(base,p.z*wind.z*4.1+17.3));
 d=saturate((d-erosion*shape.w)/max(1-shape.w,.1));
 return d*shape.y;
}

float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
 float raw=tex2D(depthMap,uv).r;
 // INTZ stores viewport depth; normalize it exactly like the atmosphere's
 // boundary pass so one convention covers every injected pass.
 raw=raw>=.99999?1:saturate((raw-depthRange.x)/max(depthRange.y,.001));
 // Transmittance 1 / no in-scattering means "this pixel keeps the scene".
 if(raw<depthRange.z) return float4(0,0,0,1);

 float3 view=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1);
 float3 viewRay=view/length(view);
 float3 ray=normalize(viewRay.x*invView0.xyz+viewRay.y*invView1.xyz+viewRay.z*invView2.xyz);
 // Below the horizon there is no deck, and grazing rays would otherwise
 // stretch the slab to the far plane and turn into a hard band.
 float horizon=smoothstep(0,max(tuning.x,.001),ray.z);
 if(horizon<=.002) return float4(0,0,0,1);

 float upward=max(ray.z,.0015);
 float entry=max((layer.x-cameraPos.z)/upward,0);
 float exitDistance=min((layer.y-cameraPos.z)/upward,layer.w);
 if(exitDistance<=entry) return float4(0,0,0,1);

 int steps=(int)sunColor.w;
 float stepLength=(exitDistance-entry)/max((float)steps,1);
 // Fixed spatial stratification (never a frame counter): the quarter-res
 // buffer is upsampled bilinearly, which averages neighbouring phases.
 float jitter=frac(52.9829189*frac(dot(vpos,float2(.06711056,.00583715))));
 // The deck stops at the march limit long before the slab geometry does,
 // so fade it out there as well or the cut shows up as a straight edge.
 float reach=1-smoothstep(layer.w*.62,layer.w,entry);
 float fade=horizon*reach;
 if(fade<=.002) return float4(0,0,0,1);
 float3 lightDir=normalize(celestial.xyz);
 float cosTheta=dot(ray,lightDir);
 float g=clamp(lightTuning.w,0,.92);
 float forward=(1-g*g)/(12.566371*pow(max(1+g*g-2*g*cosTheta,.04),1.5));
 float isotropic=.0796;
 float lightStep=max((layer.y-layer.x)*.22,1);

 float transmittance=1; float3 scattered=0; float direct=0;
 [loop] for(int i=0;i<32;++i) {
   if(i>=steps||transmittance<.012) break;
   float t=entry+(i+jitter)*stepLength;
   float3 p=cameraPos.xyz+ray*t;
   float density=Density(p);
   if(density>.002) {
     float lightOptical=0;
     [unroll] for(int j=0;j<4;++j) {
       float3 q=p+lightDir*lightStep*(j+.65);
       lightOptical+=Shape(q)*shape.y*lightStep;
     }
     float lightTransmittance=exp(-lightOptical*lightTuning.x);
     float powder=1-exp(-density*lightStep*2.4);
     float height=saturate((p.z-layer.x)*layer.z);
     float3 sun=sunColor.rgb*celestial.w*lightTuning.y*lightTransmittance*
                (forward+isotropic)*lerp(1,powder,ambientColor.w);
     float3 ambient=ambientColor.rgb*lightTuning.z*lerp(.45,1.15,height);
     // Analytic in-scattering over the step: energy conserving, and stable
     // when the step length changes with the slab crossing angle.
     float stepTransmittance=exp(-density*stepLength);
     scattered+=transmittance*(sun+ambient)*(1-stepTransmittance);
     direct+=lightTransmittance*(1-stepTransmittance);
     transmittance*=stepTransmittance;
   }
 }

 scattered*=fade;
 transmittance=lerp(1,saturate(transmittance),fade);
 if(tuning.y>1.5&&tuning.y<2.5) return float4((1-transmittance).xxx,transmittance);
 if(tuning.y>2.5) return float4(saturate(direct).xxx,transmittance);
 return float4(max(scattered,0),transmittance);
})HLSL";

// Full-resolution composite. The low-resolution deck is a smooth signal
// with no depth edges of its own, so a plain bilinear fetch is enough -
// geometry pixels carry transmittance 1 and therefore fade the clouds out
// across a silhouette instead of haloing it.
const char* kCloudCompositeSource = R"HLSL(
sampler2D sceneMap:register(s0); sampler2D cloudMap:register(s1); sampler2D depthMap:register(s2);
float4 texels:register(c0);      // x=1/full width, y=1/full height, z=1/low width, w=1/low height
float4 depthRange:register(c1);  // x=viewport MinZ, y=viewport depth range, z=sky threshold, w=debug
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float4 scene=tex2D(sceneMap,uv);
 float4 cloud=tex2D(cloudMap,uv);
 if(depthRange.w>.5&&depthRange.w<1.5) return float4(cloud.rgb,1);
 float raw=tex2D(depthMap,uv).r;
 raw=raw>=.99999?1:saturate((raw-depthRange.x)/max(depthRange.y,.001));
 float sky=smoothstep(depthRange.z,1,raw);
 float3 lit=scene.rgb*saturate(cloud.a)+max(cloud.rgb,0);
 return float4(lerp(scene.rgb,lit,sky),scene.a);
})HLSL";

bool Compile(IDirect3DDevice9* d,const char* source,IDirect3DPixelShader9** shader)
{
 ComPtr<ID3DBlob> blob,errors;
 if(FAILED(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.GetAddressOf(),errors.GetAddressOf()))||!blob)return false;
 return SUCCEEDED(d->CreatePixelShader(static_cast<DWORD*>(blob->GetBufferPointer()),shader));
}

// Sky pixels sit at the far plane. The same threshold the atmosphere's
// integration uses, kept in one place for both cloud passes.
constexpr float kSkyThreshold = .9999f;

// Extinction of fully dense cloud material per world unit. DensityPercent
// scales this, so the ini value stays a readable percentage instead of a
// physical coefficient: 100% over a 190-unit deck is an optical depth of
// about 4, which reads as a solid cloud with soft edges.
constexpr float kBaseExtinction = .022f;
}

VolumetricClouds& VolumetricClouds::Instance(){static VolumetricClouds clouds;return clouds;}

void VolumetricClouds::Configure(const std::wstring& basePath)
{
 const std::wstring ini=basePath+L"GraphicsEffects.ini";
 m_logPath=basePath+L"Clouds.log";
 auto read=[&](const wchar_t* key,int fallback)->int{return static_cast<int>(renderer::locationtuning::ReadInt(L"VolumetricClouds",key,fallback,ini.c_str()));};
 m_settings.enabled=read(L"Enabled",1)!=0;
 m_settings.quality=static_cast<uint32_t>(std::clamp(read(L"Quality",1),0,2));
 static constexpr uint32_t kSamples[]={14,20,28};
 static constexpr float kScales[]={.25f,.25f,.5f};
 m_settings.sampleCount=kSamples[m_settings.quality];
 m_settings.resolutionScale=kScales[m_settings.quality];
 m_settings.coverage=std::clamp(read(L"CoveragePercent",42),0,100)*.01f;
 m_settings.density=std::clamp(read(L"DensityPercent",100),10,400)*.01f;
 m_settings.altitude=float(std::clamp(read(L"Altitude",480),120,2000));
 m_settings.thickness=float(std::clamp(read(L"Thickness",190),40,900));
 m_settings.minClearance=float(std::clamp(read(L"MinimumClearance",140),40,600));
 m_settings.featureSize=float(std::clamp(read(L"FeatureSize",2600),400,12000));
 m_settings.detail=std::clamp(read(L"DetailPercent",34),0,80)*.01f;
 m_settings.windSpeed=float(std::clamp(read(L"WindSpeed",11),0,120));
 m_settings.windAngle=float(std::clamp(read(L"WindAngle",25),-180,180));
 m_settings.absorption=std::clamp(read(L"AbsorptionPercent",125),20,400)*.01f;
 m_settings.sunIntensity=std::clamp(read(L"SunIntensityPercent",130),0,400)*.01f;
 m_settings.ambientIntensity=std::clamp(read(L"AmbientPercent",62),0,250)*.01f;
 m_settings.moonStrength=std::clamp(read(L"MoonStrengthPercent",30),0,150)*.01f;
 m_settings.phase=std::clamp(read(L"ForwardScatterPercent",55),0,92)*.01f;
 m_settings.horizonFade=std::clamp(read(L"HorizonFadePermille",85),5,400)*.001f;
 m_settings.maxDistance=float(std::clamp(read(L"MaxDistance",9000),1000,40000));
 m_settings.debugMode=static_cast<CloudDebugMode>(std::clamp(read(L"DebugMode",0),0,3));
 m_settings.weatherCoverageBoost=0.f;
}

void VolumetricClouds::Reset(IDirect3DDevice9* device)
{
 if(m_owner&&m_owner!=device)return;
 m_cloudSurface.Reset();m_cloudTexture.Reset();
 m_marchShader.Reset();m_compositeShader.Reset();
 m_owner=nullptr;m_fullWidth=m_fullHeight=m_lowWidth=m_lowHeight=0;
}

bool VolumetricClouds::EnsureShaders(IDirect3DDevice9* d)
{
 if(m_marchShader&&m_compositeShader)return true;
 return Compile(d,kCloudMarchSource,m_marchShader.GetAddressOf())&&
        Compile(d,kCloudCompositeSource,m_compositeShader.GetAddressOf());
}

bool VolumetricClouds::EnsureResources(IDirect3DDevice9* d,uint32_t w,uint32_t h)
{
 const uint32_t lw=std::max(1u,uint32_t(w*m_settings.resolutionScale));
 const uint32_t lh=std::max(1u,uint32_t(h*m_settings.resolutionScale));
 if(m_owner==d&&m_fullWidth==w&&m_fullHeight==h&&m_lowWidth==lw&&m_lowHeight==lh&&m_cloudTexture)return true;
 Reset(d);m_owner=d;m_fullWidth=w;m_fullHeight=h;m_lowWidth=lw;m_lowHeight=lh;
 auto tex=[&](D3DFORMAT format){
  m_cloudTexture.Reset();m_cloudSurface.Reset();
  return SUCCEEDED(d->CreateTexture(lw,lh,1,D3DUSAGE_RENDERTARGET,format,D3DPOOL_DEFAULT,m_cloudTexture.GetAddressOf(),nullptr))&&
         SUCCEEDED(m_cloudTexture->GetSurfaceLevel(0,m_cloudSurface.GetAddressOf()));
 };
 // Half-float keeps bright sunlit tops and the transmittance channel
 // separable; the 8-bit fallback only costs banding on very bright decks.
 if(tex(D3DFMT_A16B16G16R16F))return true;
 if(tex(D3DFMT_A8R8G8B8))return true;
 m_owner=nullptr;m_fullWidth=m_fullHeight=m_lowWidth=m_lowHeight=0;
 return false;
}

void VolumetricClouds::DrawScreenQuad(IDirect3DDevice9* d,uint32_t w,uint32_t h)
{
 struct V{float x,y,z,rhw,u,v;};float fw=float(w)-.5f,fh=float(h)-.5f;
 V q[]={{-.5f,-.5f,0,1,0,0},{fw,-.5f,0,1,1,0},{-.5f,fh,0,1,0,1},{fw,fh,0,1,1,1}};
 d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(V));
}

bool VolumetricClouds::Render(IDirect3DDevice9* d,const FrameContext& f,IDirect3DSurface9* target)
{
 VolumetricCloudSettings settings=m_settings;
 settings.coverage=std::clamp(settings.coverage+settings.weatherCoverageBoost,0.f,1.f);
 if(!settings.enabled||settings.coverage<=0.f||settings.density<=0.f)return false;
 if(!d||!target||!f.cameraValid||!f.depthAvailable||!f.depthTexture||!f.sceneColor||!f.atmosphereNoise)return false;

 D3DSURFACE_DESC desc{};
 if(FAILED(target->GetDesc(&desc))||!EnsureResources(d,desc.Width,desc.Height)||!EnsureShaders(d))return false;

 const ULONGLONG tick=GetTickCount64();
 const float dt=m_lastTick?std::clamp(float(tick-m_lastTick)*.001f,.001f,.1f):1.f/60.f;
 m_lastTick=tick;m_elapsed+=dt;

 // Absolute world slab, but never below the camera: a flying mount or a
 // mountain top must not end up inside the band, where a sky-only pass has
 // nothing sensible to show.
 const float base=std::max(settings.altitude,f.cameraPosition.z+settings.minClearance);
 const float thickness=std::max(settings.thickness,20.f);
 const float featureSize=std::max(settings.featureSize,1.f);
 const float windRadians=settings.windAngle*.01745329252f;
 // The noise atlas tiles over one uv unit, so wrapping the accumulated
 // wind offset is exact - and it keeps the constant small enough that
 // texture-coordinate precision never drifts during a long session.
 const float windOffsetX=std::fmod(std::cos(windRadians)*settings.windSpeed*m_elapsed/featureSize,1.f);
 const float windOffsetY=std::fmod(std::sin(windRadians)*settings.windSpeed*m_elapsed/featureSize,1.f);

 // Zone-authored fog colour doubles as the sky ambient so the deck belongs
 // to the same palette as the atmosphere below it.
 const auto& envFog=EnvironmentFogCapture::Instance().State();
 const float ambR=envFog.colorValid?envFog.ambientFogColor[0]:.42f;
 const float ambG=envFog.colorValid?envFog.ambientFogColor[1]:.52f;
 const float ambB=envFog.colorValid?envFog.ambientFogColor[2]:.66f;
 const float daylight=std::clamp(f.daylightFactor,0.f,1.f);
 const float moonlight=std::clamp(f.moonlightFactor,0.f,1.f);
 const float ambientLevel=std::clamp(std::max(daylight,moonlight*.5f),.2f,1.f);
 const bool moon=f.celestialIsMoon;
 const float directIntensity=f.celestialIntensity*(moon?settings.moonStrength:1.f);
 // The tracked light colour is the client's own directional colour; fall
 // back to a neutral daylight/moonlight tint when it has not been seen.
 const float lightLength=std::sqrt(f.directionalLightColor.x*f.directionalLightColor.x+
                                   f.directionalLightColor.y*f.directionalLightColor.y+
                                   f.directionalLightColor.z*f.directionalLightColor.z);
 const bool useTracked=lightLength>.05f;
 const float sunR=useTracked?std::clamp(f.directionalLightColor.x,0.f,2.f):(moon?.62f:1.f);
 const float sunG=useTracked?std::clamp(f.directionalLightColor.y,0.f,2.f):(moon?.72f:.97f);
 const float sunB=useTracked?std::clamp(f.directionalLightColor.z,0.f,2.f):(moon?.92f:.86f);

 if((++m_renderFrames%600)==0&&!m_logPath.empty()){
  std::ofstream out(std::filesystem::path(m_logPath),std::ios::app);
  out<<"clouds base="<<base<<" thickness="<<thickness<<" coverage="<<settings.coverage
     <<" density="<<settings.density<<" steps="<<settings.sampleCount
     <<" moon="<<(moon?1:0)<<" intensity="<<directIntensity
     <<" camera=("<<f.cameraPosition.x<<','<<f.cameraPosition.y<<','<<f.cameraPosition.z<<")\n";
 }

 ScopedRenderState state(d);
 DepthCapture::Instance().RawSetDepth(d,nullptr);
 for(auto s:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,
             D3DRS_SCISSORTESTENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_SRGBWRITEENABLE,
             D3DRS_ALPHABLENDENABLE})d->SetRenderState(s,FALSE);
 d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
 d->SetRenderState(D3DRS_COLORWRITEENABLE,0xF);
 d->SetVertexShader(nullptr);

 D3DVIEWPORT9 low{0,0,m_lowWidth,m_lowHeight,0,1},full{0,0,m_fullWidth,m_fullHeight,0,1};
 {
  ScopedGpuTimer timer(d,GpuPerfStage::VolumetricClouds);
  d->SetViewport(&low);
  d->SetRenderTarget(0,m_cloudSurface.Get());
  d->SetPixelShader(m_marchShader.Get());
  d->SetTexture(0,f.depthTexture);
  d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
  d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
  d->SetTexture(1,f.atmosphereNoise);
  d->SetSamplerState(1,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);d->SetSamplerState(1,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
  d->SetSamplerState(1,D3DSAMP_MIPFILTER,D3DTEXF_NONE);d->SetSamplerState(1,D3DSAMP_SRGBTEXTURE,FALSE);
  d->SetSamplerState(1,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);d->SetSamplerState(1,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);

  float c0[4]={f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,m_elapsed};
  float c1[4]={f.sunDirectionWorld.x,f.sunDirectionWorld.y,f.sunDirectionWorld.z,directIntensity};
  // A frame without a confirmed disc still needs a usable march direction.
  if(std::abs(c1[0])+std::abs(c1[1])+std::abs(c1[2])<.01f){c1[0]=0;c1[1]=0;c1[2]=1;}
  float inv[3][4]={{f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},
                   {f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},
                   {f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0}};
  float c5[4]={f.projUnpack[0],f.projUnpack[1],f.projUnpack[2],f.projUnpack[3]};
  float c6[4]={f.viewport.MinZ,f.depthMaxZ-f.viewport.MinZ,kSkyThreshold,0};
  float c7[4]={base,base+thickness,1.f/thickness,settings.maxDistance};
  float c8[4]={settings.coverage,settings.density*kBaseExtinction,1.f/featureSize,settings.detail};
  float c9[4]={windOffsetX,windOffsetY,1.f/std::max(thickness*.5f,1.f),0};
  float c10[4]={settings.absorption,settings.sunIntensity,settings.ambientIntensity*ambientLevel,settings.phase};
  float c11[4]={sunR,sunG,sunB,float(std::min(32u,settings.sampleCount))};
  float c12[4]={ambR*ambientLevel,ambG*ambientLevel,ambB*ambientLevel,.65f};
  float c13[4]={settings.horizonFade,float(uint32_t(settings.debugMode)),0,0};
  d->SetPixelShaderConstantF(0,c0,1);d->SetPixelShaderConstantF(1,c1,1);
  d->SetPixelShaderConstantF(2,inv[0],3);
  d->SetPixelShaderConstantF(5,c5,1);d->SetPixelShaderConstantF(6,c6,1);
  d->SetPixelShaderConstantF(7,c7,1);d->SetPixelShaderConstantF(8,c8,1);
  d->SetPixelShaderConstantF(9,c9,1);d->SetPixelShaderConstantF(10,c10,1);
  d->SetPixelShaderConstantF(11,c11,1);d->SetPixelShaderConstantF(12,c12,1);
  d->SetPixelShaderConstantF(13,c13,1);
  DrawScreenQuad(d,m_lowWidth,m_lowHeight);
  d->SetTexture(0,nullptr);d->SetTexture(1,nullptr);
 }

 {
  ScopedGpuTimer timer(d,GpuPerfStage::VolumetricClouds);
  d->SetViewport(&full);
  d->SetRenderTarget(0,target);
  d->SetPixelShader(m_compositeShader.Get());
  d->SetTexture(0,f.sceneColor);
  d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
  d->SetTexture(1,m_cloudTexture.Get());
  d->SetSamplerState(1,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);d->SetSamplerState(1,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
  d->SetSamplerState(1,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
  d->SetSamplerState(1,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(1,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
  d->SetTexture(2,f.depthTexture);
  d->SetSamplerState(2,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(2,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  d->SetSamplerState(2,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(2,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
  float c0[4]={1.f/float(m_fullWidth),1.f/float(m_fullHeight),1.f/float(m_lowWidth),1.f/float(m_lowHeight)};
  float c1[4]={f.viewport.MinZ,f.depthMaxZ-f.viewport.MinZ,kSkyThreshold,float(uint32_t(settings.debugMode))};
  d->SetPixelShaderConstantF(0,c0,1);d->SetPixelShaderConstantF(1,c1,1);
  DrawScreenQuad(d,m_fullWidth,m_fullHeight);
  d->SetTexture(0,nullptr);d->SetTexture(1,nullptr);d->SetTexture(2,nullptr);
 }
 return true;
}
}
