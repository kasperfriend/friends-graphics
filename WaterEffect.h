#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include "src/Environment/LocationTuning.h"
#include <unordered_set>
#include <cstdio>
#include "src/D3D9/TrackedRenderState.h"
#include "src/D3D9/CelestialTracker.h"
#include "src/Lighting/LocalLightManager.h"
#include "src/Core/FrameClock.h"
#include "src/Core/FrameContext.h"
#include "src/Water/WaterMaterial.h"
namespace watereffect {
using Microsoft::WRL::ComPtr;
// Backend readiness, NOT the user's switch. Keep shaders/hooks available even
// when water starts off, so F7/F12 and location changes can enable it live.
// effectEnabled is the single live switch; active is the F9/F11 master switch.
bool enabled=false, active=true, keyDown=false, hotkey=true;
bool effectEnabled=true,wavesEnabled=true,geometryWavesEnabled=false,reflectionsEnabled=true,refractionEnabled=true,depthEnabled=true,foamEnabled=true,sunGlintEnabled=true;
float strength=.003f;
float normalStrength=.16f, specularStrength=.55f;
float reflectionStrength=.70f,environmentStrength=.24f,reflectionDistance=120.f,reflectionThickness=1.8f;
float flowSpeed=1.f,flowRotation=.32f;
float refractionStrength=.12f,absorptionStrength=.18f,shoreFoamStrength=.12f,shoreFoamWidth=2.5f;
float swellStrength=.45f,sunGlintStrength=1.35f,fresnelStrength=1.f,crestFoamStrength=.18f;
float geometryWaveAmplitude=.10f;
float waterDebugMode=0;
IDirect3DTexture9* reflectionScene=nullptr;
IDirect3DTexture9* reflectionDepth=nullptr;
float reflectionData[12]{};
// Set once per frame by VolumeIntegration.h (BeforeClear) to persistent,
// frame-cleared render targets. Bound as extra render targets during every
// matched water draw below so water can mark its own pixels (coverage) and
// its own surface depth for the atmosphere pass - see
// FrameContext::waterMaskTexture / waterDepthTexture. The atmosphere ray
// must stop at the water SURFACE, not the seabed depth already sitting in
// the main depth buffer (water doesn't write it) - see
// DirectionalVolumetricLighting.cpp's boundary-depth pass.
IDirect3DSurface9* waterTargetSurface=nullptr;
IDirect3DSurface9* waterMaskSurface=nullptr;
IDirect3DSurface9* waterDepthSurface=nullptr;
std::vector<DWORD> code,vertexCode;
inline ComPtr<IDirect3DPixelShader9> cachedReplacementPS;
inline ComPtr<IDirect3DVertexShader9> cachedReplacementVS;
inline IDirect3DDevice9* cachedShaderDevice = nullptr;
inline bool shaderCreationFailed = false;
inline bool NeedsScene() {
    return enabled && active && effectEnabled && renderer::water::NeedsScene(
        reflectionsEnabled, refractionEnabled, depthEnabled, foamEnabled, sunGlintEnabled);
}
bool showStatus=false;
unsigned frameMatches=0,frameCandidates=0,frameSceneMatches=0;
std::wstring logPath;
std::wstring mainIni,tuningIni;
inline void ReportFailure(const char* operation,HRESULT hr) {
    static std::unordered_set<std::string> reported;
    if(reported.insert(operation).second)
        std::ofstream(std::filesystem::path(logPath),std::ios::app)
            <<operation<<" failed hr="<<std::hex<<hr<<std::dec<<"; native water retained if replacement cannot be installed\n";
}
const char* vertexSource=R"HLSL(
float4 projection[4]:register(c0);
float4 fogParams:register(c4);
float4 modelView[4]:register(c5);
float4 uvAX:register(c9);float4 uvAY:register(c10);float4 uvAOffset:register(c12);
float4 uvBX:register(c13);float4 uvBY:register(c14);float4 uvBOffset:register(c16);
float4 lightDirection:register(c33);float4 ambient:register(c34);float4 diffuse:register(c35);
// x=time, y=world-space displacement amplitude, z=long-wave strength
float4 waveControl:register(c200);
struct Output {float4 pos:POSITION;float4 color:COLOR0;float2 uv0:TEXCOORD0;float2 uv1:TEXCOORD1;float fog:FOG;float3 viewPos:TEXCOORD2;float3 viewNormal:TEXCOORD3;};
Output main(float4 pos:POSITION,float3 normal:NORMAL,float4 color:COLOR0,float2 uv0:TEXCOORD0,float2 uv1:TEXCOORD1) {
    Output o;
    float phaseA=(uv0.x*.83+uv0.y*.56)*6.28318+waveControl.x*.72;
    float phaseB=(uv0.x*-.47+uv0.y*.91)*6.28318-waveControl.x*.43;
    pos.z+=waveControl.y*(sin(phaseA)+sin(phaseB)*.55)*(.55+.45*waveControl.z);
    float4 p=modelView[0]*pos.x+modelView[1]*pos.y+modelView[2]*pos.z+modelView[3];
    o.pos=projection[0]*p.x+projection[1]*p.y+projection[2]*p.z+projection[3]*p.w;
    o.fog=min(pow(max(p.z*fogParams.x+fogParams.y,0),fogParams.z),1);
    o.uv1=uvAX.xy*uv0.x+uvAY.xy*uv0.y+uvAOffset.xy;
    o.uv0=uvBX.xy*uv1.x+uvBY.xy*uv1.y+uvBOffset.xy;
    float3 n=modelView[0].xyz*normal.x+modelView[1].xyz*normal.y+modelView[2].xyz*normal.z;
    o.color=float4(diffuse.rgb*saturate(dot(-lightDirection.xyz,n))+ambient.rgb,1)*color;
    o.viewPos=p.xyz;o.viewNormal=n;return o;
}
)HLSL";
const char* source=R"HLSL(
float4 localPosition[8]:register(c180); // view-space emitters
float4 localColor[8]:register(c188);
float4 localControl:register(c196); // count, enabled
sampler2D baseTexture:register(s0);
sampler2D surfaceTexture:register(s1);
sampler2D sceneTexture:register(s2);
sampler2D sceneDepth:register(s3);
float4 fogColor:register(c4);
float4 controls:register(c200);
float4 lightDirection:register(c201); // xyz=direction, w=flow rotation sine
float4 lightColor:register(c202);
// c203=(projection A,B,scaleX,scaleY)
float4 reflectionProjection:register(c203);
// x=SSR strength,y=1/width,z=1/height,w=viewport depth maximum
float4 reflectionControl:register(c204);
// x=environment strength,y=max trace distance,z=base thickness,w=enabled
float4 reflectionStyle:register(c205);
// x=animation speed, y=counter-rotating layer strength, z=debug, w=rotation cosine.
// Rotating the wave layer mirrors modern animated UVs without new ADT data.
float4 flowControl:register(c206);
// x=refraction, y=depth absorption, z=shore foam, w=foam depth width
float4 waterStyle:register(c207);
// x=long swell strength, y=sun glitter, z=Fresnel response, w=crest foam
float4 foreverStyle:register(c208);
float3 safeNormalize(float3 v) {return v*rsqrt(max(dot(v,v),1e-8));}
// Evaluate all four corner hashes together. Scalar float3 hashes in four
// calls per octave consumed hundreds of PS3 slots after inlining. This
// vector polynomial hash needs only a handful of vector ALU instructions.
float3 noiseGradient(float2 p) {
    float2 i=floor(p),f=frac(p);
    float4 h=frac((dot(i,float2(127.1,311.7))+float4(0,127.1,311.7,438.8))*.1031);
    h*=h+33.33;
    h=frac(h*(h+h));
    float2 u=f*f*(3-2*f),du=6*f*(1-f);
    float k=h.x-h.y-h.z+h.w;
    return float3(h.x+(h.y-h.x)*u.x+(h.z-h.x)*u.y+k*u.x*u.y,
                  du.x*((h.y-h.x)+k*u.y),du.y*((h.z-h.x)+k*u.x));
}
// Second output (COLOR1): marks this pixel as water, so the atmosphere
// pass knows where the water surface actually is (not the seabed depth
// already sitting in the main depth buffer - water doesn't write that).
// Written as white with the final water alpha; the GPU's alpha blend (the same one
// used for the colour output) turns it into a real per-pixel "how much of
// this ended up as water" fraction, not a flat 0/1, so partially-blended
// shore edges are partial too.
// Third output (COLOR2): the water surface's own view-space depth
// (viewPos.z), for the atmosphere to march air only from the camera to
// THIS point, not through the water down to the seabed.
struct WaterOutput { float4 color:COLOR0; float4 mask:COLOR1; float4 surfaceDepth:COLOR2; };
WaterOutput main(float4 color:COLOR0,float2 uv0:TEXCOORD0,float2 uv1:TEXCOORD1,float fog:FOG,float3 viewPos:TEXCOORD2,float3 viewNormal:TEXCOORD3) {
    WaterOutput result_;
    result_.surfaceDepth=float4(viewPos.z,0,0,1);
    result_.mask=float4(1,1,1,1);
    // Preserve the original alpha: surfaceTexture never contributes alpha.
    float4 base=tex2D(baseTexture,uv0);
    float time=controls.x*flowControl.x;
    float2 drift=float2(time*.19,-time*.11);
    // Preserve WoW's two material UV transforms and add a slower
    // counter-rotating component.  This keeps a coherent current instead of
    // making the reflection look like a stationary mirror with noisy normals.
    // Uniform trigonometry is evaluated once per draw on the CPU, not per pixel.
    float s=lightDirection.w,c=flowControl.w;
    float2 rotated=float2(c*uv0.x-s*uv0.y,s*uv0.x+c*uv0.y);
    float2 flowingUv=lerp(uv1,rotated,flowControl.y);
    float2 p=float2(dot(flowingUv,float2(.91,.41)),dot(flowingUv,float2(-.41,.91)))*float2(14,31);
    float3 swell=noiseGradient(p*.055+drift*.085);
    float3 a=noiseGradient(p+drift+float2(swell.x,swell.x*.71));
    float2 longSlopes=swell.yz;
    float2 slopes=a.yz*.72+longSlopes*foreverStyle.x;
    float filter=rsqrt(1+16*dot(fwidth(p),fwidth(p)));
    float flowGate=saturate(controls.y*100+controls.z*4);
    slopes*=filter*flowGate;
    float2 offset=slopes*controls.y;
    float3 surface=tex2D(surfaceTexture,uv1+offset+drift*.010*flowGate).rgb;
    float3 rgb=surface+color.rgb*base.rgb;
    float3 v=safeNormalize(-viewPos);
    float3 n=safeNormalize(viewNormal);
    // Water can be drawn from either side. Orient only the added lighting normal.
    n*=dot(n,v)<0?-1:1;
    float3 px=ddx(viewPos),py=ddy(viewPos);
    float2 ux=ddx(uv1),uy=ddy(uv1);
    float det=ux.x*uy.y-ux.y*uy.x;
    float3 tangent=(px*uy.y-py*ux.y)*(det<0?-1:1);
    tangent=safeNormalize(tangent-n*dot(n,tangent));
    float3 bitangent=safeNormalize(cross(n,tangent));
    float3 wave=safeNormalize(n+controls.z*(tangent*slopes.x+bitangent*slopes.y));
    // The captured pre-water colour and depth allow shallow refraction and a
    // depth transition without changing the original map or liquid data.
    float2 screenUV=float2(.5+.5*viewPos.x*reflectionProjection.z/max(viewPos.z,.05),
                           .5-.5*viewPos.y*reflectionProjection.w/max(viewPos.z,.05));
    float behindRaw=tex2Dlod(sceneDepth,float4(screenUV,0,0)).r;
    float behindZ=behindRaw>=.9999?100000:reflectionProjection.y/(behindRaw/max(reflectionControl.w,.001)-reflectionProjection.x);
    // Foreground geometry is not a deep seabed. Never turn negative
    // separation into absorption/foam on the wrong side of a shoreline.
    float waterDepth=max(behindZ-viewPos.z,0);
    float captureValid=reflectionStyle.w;
    float depthValid=captureValid*step(behindZ,99999)*step(viewPos.z,behindZ);
    // Open ocean has no seafloor within the depth buffer at all (behindRaw
    // reads as sky), so depthValid was 0 here - which used to zero every
    // absorption/tint term below AND still sample sceneTexture unmodified
    // at screenUV as "refracted", i.e. literally paint raw sky colour onto
    // the water. That is exactly what read as "the sea looks transparent"
    // no matter how high DepthAbsorptionPercent was set: absorption was
    // gated to zero regardless of the slider. No floor found means this is
    // genuinely deep, not "not water" - treat it as maximum depth instead.
    float openOcean=step(99999,behindZ);
    float effectiveDepth=lerp(waterDepth,9999.0,openOcean);
    float2 refractOffset=slopes*float2(reflectionControl.y,reflectionControl.z)*waterStyle.x*120;
    float2 refractUV=screenUV+(depthValid?refractOffset:float2(0,0));
    // Reject distortion across foreground silhouettes, rather than pulling
    // a bridge/character into the water. Missing depth keeps the native body.
    float refractRaw=tex2Dlod(sceneDepth,float4(refractUV,0,0)).r;
    float surfaceRaw=(reflectionProjection.x+reflectionProjection.y/max(viewPos.z,.05))*reflectionControl.w;
    refractUV=lerp(screenUV,refractUV,step(surfaceRaw,refractRaw));
    float3 refracted=tex2Dlod(sceneTexture,float4(refractUV,0,0)).rgb;
    float3 deepTint=lerp(fogColor.rgb*.42,float3(.025,.105,.135),.62);
    float absorption=captureValid*(1-exp(-effectiveDepth*waterStyle.y*.14));
    float shallowEdge=1-smoothstep(.08,.08+max(waterStyle.w,.1),effectiveDepth);
    float shore=depthValid*shallowEdge;
    float crest=saturate(.55+a.x*.45+dot(slopes,float2(.18,-.12)));
    float3 l=safeNormalize(-lightDirection.xyz);
    float ndh=saturate(dot(wave,safeNormalize(l+v)));
    float ndl=saturate(dot(wave,l));
    // One rough specular lobe with wave breakup creates the sun road. The
    // previous shader evaluated three overlapping lobes, a halo and another
    // copy of the same path; besides washing out water, it exceeded PS3 limits.
    float foreverFresnel=pow(1-saturate(dot(wave,v)),2);
    float fresnel=saturate(.08+foreverStyle.z*(.10+.62*foreverFresnel));
    float pathBreakup=.18+.82*saturate(a.x*.7+swell.x*.3);
    float celestialPath=pow(ndh,64)*pathBreakup*foreverStyle.y*fresnel*ndl;

    // March from this water point toward the sun/moon through the captured
    // pre-water depth buffer. Buildings and terrain suppress direct glint,
    // while ambient sky reflection remains visible under cover.
    float lightVisibility=1.0;
    if(reflectionStyle.w>.5 && celestialPath>.00005) {
        [loop] for(int lightStep=0;lightStep<8;++lightStep) {
            float q=(lightStep+1)/8.0;
            float traceDistance=1.5+q*q*110.0;
            float3 lightSample=viewPos+wave*.18+l*traceDistance;
            float2 lightUV=float2(.5+.5*lightSample.x*reflectionProjection.z/max(lightSample.z,.05),
                                  .5-.5*lightSample.y*reflectionProjection.w/max(lightSample.z,.05));
            float inside=step(0,lightUV.x)*step(lightUV.x,1)*step(0,lightUV.y)*step(lightUV.y,1)*step(.05,lightSample.z);
            float lightRaw=tex2Dlod(sceneDepth,float4(lightUV,0,0)).r;
            float blockerZ=lightRaw>=.9999?100000:reflectionProjection.y/(lightRaw/max(reflectionControl.w,.001)-reflectionProjection.x);
            float delta=lightSample.z-blockerZ;
            float thickness=2.0+traceDistance*.10;
            float blocked=inside*step(blockerZ,99999)*step(.12,delta)*step(delta,thickness);
            lightVisibility*=1.0-blocked;
        }
    }
    celestialPath*=lightVisibility;
    float3 sheenColor=max(lightColor.rgb,0);
    rgb*=1+controls.z*.6*(dot(wave,l)-dot(n,l));

    // 1. Water body: bottom refracted scene + depth absorption
    float3 waterBody=rgb;
    if(captureValid>.5) {
        float3 bottom=lerp(rgb,refracted,depthValid*saturate(waterStyle.x));
        waterBody=lerp(bottom,deepTint,saturate(absorption*(.85-.22*fresnel)));
    }

    // 2. Screen-Space Reflections (SSR) with wave normal perturbation
    float3 environment=lerp(fogColor.rgb,max(sheenColor,.08),.28);
    float3 reflected=environment;
    float hit=0,edge=0;
    if(reflectionStyle.w>.5 && reflectionControl.x>.001) {
        float3 ray=safeNormalize(reflect(-v,wave));
        float3 hitColor=environment;
        float bestConfidence=0;
        [loop] for(int stepIndex=0;stepIndex<12;++stepIndex) {
            float q=(stepIndex+1)/12.0;
            float traceDistance=.45+q*q*reflectionStyle.y;
            float3 samplePosition=viewPos+wave*.15+ray*traceDistance;
            float2 sampleUV=float2(.5+.5*samplePosition.x*reflectionProjection.z/max(samplePosition.z,.05),
                                   .5-.5*samplePosition.y*reflectionProjection.w/max(samplePosition.z,.05));
            // border is already zero outside the viewport; don't compute the
            // same four inside tests a second time for every SSR step.
            float inside=step(.05,samplePosition.z);
            float raw=tex2Dlod(sceneDepth,float4(sampleUV,0,0)).r;
            float sceneZ=raw>=.9999?100000:reflectionProjection.y/(raw/max(reflectionControl.w,.001)-reflectionProjection.x);
            float difference=samplePosition.z-sceneZ;
            float thickness=reflectionStyle.z+traceDistance*.05;
            float range=saturate(1-abs(difference)/max(thickness,.01));
            float sided=step(-thickness*.28,difference);
            float border=saturate(min(min(sampleUV.x,sampleUV.y),min(1-sampleUV.x,1-sampleUV.y))*8);
            float confidence=inside*step(sceneZ,99999)*sided*range*border;
            float replace=step(bestConfidence+.0001,confidence);
            hitColor=lerp(hitColor,tex2Dlod(sceneTexture,float4(sampleUV,0,0)).rgb,replace);
            bestConfidence=max(bestConfidence,confidence);
        }
        hit=saturate(bestConfidence*1.8);
        edge=hit;
        reflected=lerp(environment,hitColor,edge);
    }

    // 3. Surface reflection according to Fresnel and SSR strength
    float reflectionAmount=saturate((.10+.65*foreverFresnel)*(reflectionStyle.x*.55+reflectionControl.x*edge*1.6));
    rgb=lerp(waterBody,reflected,reflectionAmount);

    // Direct reflection of actual local emitters on the same wave normal.
    // This does not depend on SSR finding an opaque flame in scene depth.
    float3 localReflection=0;
    [loop]for(int li=0;li<8;++li){
        if(li>=(int)localControl.x)break;
        float3 delta=localPosition[li].xyz-viewPos;
        float distance=length(delta);float3 toLight=delta/max(distance,.001);
        float reach=saturate(1-distance/max(localPosition[li].w,.01));
        float specular=pow(saturate(dot(wave,safeNormalize(toLight+v))),64);
        float fresnelLocal=.04+.96*foreverFresnel*foreverFresnel;
        localReflection+=localColor[li].rgb*localColor[li].w*specular*fresnelLocal*
            saturate(dot(wave,toLight))*reach*reach/(1+distance*distance*.035);
    }
    rgb+=localReflection*localControl.y;

    // 4. Celestial reflection path & shore/crest foam
    float pathEnergy=saturate(celestialPath*controls.w*.62);
    rgb+=max(sheenColor,float3(.20,.22,.24))*pathEnergy;
    float waveFoam=smoothstep(.70,.91,a.x*.7+swell.x*.3+length(longSlopes)*.10);
    float foamAmount=(shore*waterStyle.z+waveFoam*foreverStyle.w)*crest;
    rgb+=float3(.42,.48,.43)*foamAmount;
    if(flowControl.z>1.5){result_.color=float4(saturate(pathEnergy*3).xxx,1);return result_;}
    if(flowControl.z>.5){result_.color=float4(1,0,1,1);return result_;}
    float baseAlpha=color.a*base.a;
    float depthAlpha=absorption*(.78-.20*fresnel);
    result_.color=float4(fog*(rgb-fogColor.rgb)+fogColor.rgb,saturate(baseAlpha+(1-baseAlpha)*depthAlpha));
    result_.mask.a=result_.color.a;
    return result_;
}
)HLSL";
int ReadTuning(const wchar_t* key,int fallback){wchar_t value[64]{};renderer::locationtuning::ReadString(L"Water",key,L"",value,std::size(value),tuningIni.c_str());return value[0]?int(wcstol(value,nullptr,10)):renderer::locationtuning::ReadInt(L"Water",key,fallback,mainIni.c_str());}
void ReloadTuning(){
    // Match the menu's source and default exactly. An obsolete Enabled=0 in
    // ModernWoWRenderer.ini must not veto a live (including per-location) ON.
    effectEnabled=renderer::locationtuning::ReadInt(L"Water",L"Enabled",1,tuningIni.c_str())!=0;
    wavesEnabled=ReadTuning(L"WavesEnabled",1)!=0;geometryWavesEnabled=ReadTuning(L"GeometryWavesEnabled",0)!=0;
    reflectionsEnabled=ReadTuning(L"ReflectionsEnabled",1)!=0;refractionEnabled=ReadTuning(L"RefractionEnabled",1)!=0;
    depthEnabled=ReadTuning(L"DepthEnabled",1)!=0;foamEnabled=ReadTuning(L"FoamEnabled",1)!=0;sunGlintEnabled=ReadTuning(L"SunGlintEnabled",1)!=0;
    strength=std::clamp(ReadTuning(L"RipplePermille",18),0,50)*.001f;
    normalStrength=std::clamp(ReadTuning(L"NormalPercent",28),0,60)*.01f;
    specularStrength=std::clamp(ReadTuning(L"SpecularPercent",115),0,400)*.01f;
    reflectionStrength=std::clamp(ReadTuning(L"ReflectionPercent",80),0,200)*.01f;
    environmentStrength=std::clamp(ReadTuning(L"EnvironmentPercent",45),0,200)*.01f;
    reflectionDistance=float(std::clamp(ReadTuning(L"ReflectionDistance",120),20,300));
    reflectionThickness=std::clamp(ReadTuning(L"ReflectionThicknessPercent",350),25,600)*.01f;
    flowSpeed=std::clamp(ReadTuning(L"FlowSpeedPercent",100),0,300)*.01f;
    flowRotation=std::clamp(ReadTuning(L"FlowRotationPercent",32),0,100)*.01f;
    refractionStrength=std::clamp(ReadTuning(L"RefractionPercent",14),0,100)*.01f;
    absorptionStrength=std::clamp(ReadTuning(L"DepthAbsorptionPercent",38),0,100)*.01f;
    shoreFoamStrength=std::clamp(ReadTuning(L"ShoreFoamPercent",28),0,60)*.01f;
    shoreFoamWidth=std::clamp(ReadTuning(L"ShoreFoamWidthPercent",120),25,800)*.01f;
    swellStrength=std::clamp(ReadTuning(L"SwellPercent",55),0,150)*.01f;
    sunGlintStrength=std::clamp(ReadTuning(L"SunGlintPercent",160),0,400)*.01f;
    fresnelStrength=std::clamp(ReadTuning(L"FresnelPercent",100),0,200)*.01f;
    crestFoamStrength=std::clamp(ReadTuning(L"CrestFoamPercent",20),0,100)*.01f;
    geometryWaveAmplitude=std::clamp(ReadTuning(L"GeometryWavePercent",10),0,35)*.01f;
    // 0=off, 1=solid magenta (confirms this pixel is our water shader),
    // 2=grayscale celestial-glint term only (x3 gain), isolated from the
    // rest of the water shading - for debugging "the sun path isn't
    // showing" without guessing which term is actually zero.
    waterDebugMode=float(std::clamp(ReadTuning(L"WaterDebugMode",0),0,2));
    if(!logPath.empty()){std::ofstream out(std::filesystem::path(logPath),std::ios::app);out<<"tuning enabled="<<effectEnabled<<" master="<<active<<" ripple="<<strength<<" normal="<<normalStrength<<" specular="<<specularStrength<<" reflection="<<reflectionStrength<<" environment="<<environmentStrength<<" distance="<<reflectionDistance<<" thickness="<<reflectionThickness<<" flow="<<flowSpeed<<','<<flowRotation<<" refract="<<refractionStrength<<" absorption="<<absorptionStrength<<" foam="<<shoreFoamStrength<<','<<shoreFoamWidth<<" forever="<<swellStrength<<','<<sunGlintStrength<<','<<fresnelStrength<<','<<crestFoamStrength<<" debug="<<waterDebugMode<<'\n';}
}
void Configure(const std::wstring& base) {
    mainIni=base+L"ModernWoWRenderer.ini";tuningIni=base+L"GraphicsEffects.ini";
    logPath=base+L"WaterEffect.log";
    enabled=false;
    code.clear();vertexCode.clear();
    cachedReplacementPS.Reset();cachedReplacementVS.Reset();
    cachedShaderDevice=nullptr;shaderCreationFailed=false;
    // Compile independently of the live switch. HookDevice uses readiness to
    // install draw/depth hooks once, before a player can turn water on in F7.
    ReloadTuning();
    showStatus=renderer::locationtuning::ReadInt(L"Water",L"ShowStatus",0,mainIni.c_str())!=0;
    ComPtr<ID3DBlob> blob,error;
    HRESULT hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.GetAddressOf(),error.GetAddressOf());
    std::ofstream log(std::filesystem::path(logPath),std::ios::app);
    log<<"compile="<<hr<<" ripple="<<strength<<'\n';
    if(FAILED(hr)){enabled=false;if(error)log<<static_cast<const char*>(error->GetBufferPointer());return;}
    code.resize(blob->GetBufferSize()/4);memcpy(code.data(),blob->GetBufferPointer(),blob->GetBufferSize());
    blob.Reset();error.Reset();
    hr=D3DCompile(vertexSource,strlen(vertexSource),nullptr,nullptr,nullptr,"main","vs_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.GetAddressOf(),error.GetAddressOf());
    log<<"vertex_compile="<<hr<<" normal="<<normalStrength<<" specular="<<specularStrength<<'\n';
    if(FAILED(hr)){enabled=false;if(error)log<<static_cast<const char*>(error->GetBufferPointer());return;}
    vertexCode.resize(blob->GetBufferSize()/4);memcpy(vertexCode.data(),blob->GetBufferPointer(),blob->GetBufferSize());
    enabled=true;
    log<<"water backend ready; live enabled="<<effectEnabled<<" (GraphicsEffects.ini/location settings)\n";
}
void Present() {
    if(!enabled)return;
    const bool down=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
    DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    if(hotkey&&down&&!keyDown&&pid==GetCurrentProcessId()) {active=!active;std::ofstream(std::filesystem::path(logPath),std::ios::app)<<"active="<<active<<'\n';}
    keyDown=down;
    static unsigned frames=0;
    if(++frames%600==0)
        std::ofstream(std::filesystem::path(logPath),std::ios::app)
            <<"water frame candidates="<<frameCandidates<<" replaced="<<frameMatches
            <<" active="<<active<<" enabled="<<effectEnabled<<" sceneDraws="<<frameSceneMatches
            <<" shaderFailure="<<shaderCreationFailed<<'\n';
    frameMatches=frameCandidates=frameSceneMatches=0;
}
inline void Reset(IDirect3DDevice9* d) {
    if(cachedShaderDevice == d) {
        cachedReplacementPS.Reset();
        cachedReplacementVS.Reset();
        cachedShaderDevice = nullptr;
        shaderCreationFailed=false;
    }
}
void DrawStatus(IDirect3DDevice9* d,const char* external=nullptr,unsigned line=0) noexcept {
    if(!external&&(!enabled||!showStatus))return;
    try {
        const char* label=external?external:(!active||!effectEnabled)?"WATER OFF":frameMatches?"WATER ON":"WATER WAIT";
        const char* letters="WATERONFIHZX";
        const char* glyphs[]={"10001100011000110101101011101110001","01110100011000111111100011000110001","11111001000010000100001000010000100","11111100001000011110100001000011111","11110100011000111110101001001010001","01110100011000110001100011000101110","10001110011010110011100011000110001","11111100001000011110100001000010000","11111001000010000100001000010011111","10001100011000111111100011000110001","11111000010001000100010001000011111","10001100010101000100010101000110001"};
        auto rect=[&](float x,float y,float width,float height,DWORD color){
            y+=line*28.f;
            D3DRECT r{static_cast<LONG>(x),static_cast<LONG>(y),
                      static_cast<LONG>(x+width),static_cast<LONG>(y+height)};
            d->Clear(1,&r,D3DCLEAR_TARGET,color,1.f,0);
        };
        rect(16,40,160,25,0xff101820);
        DWORD color=strstr(label,"OFF")?0xffaaaaaa:strstr(label,"WAIT")?0xffffcc55:0xff66ff99;
        for(unsigned i=0;label[i];++i){const char* found=strchr(letters,label[i]);if(!found)continue;auto glyph=glyphs[found-letters];for(int y=0;y<7;++y)for(int x=0;x<5;++x)if(glyph[y*5+x]=='1')rect(22.f+i*12.f+x*2.f,46.f+y*2.f,2,2,color);}
    }catch(...){}
}
struct Scope {
    ComPtr<IDirect3DPixelShader9> original,replacement;
    ComPtr<IDirect3DVertexShader9> originalVertex,replacementVertex;
    IDirect3DDevice9* device=nullptr;
    float old[36]{},oldVertexControls[4]{},oldLocalLights[68]{};bool localLightsSaved=false;
    ComPtr<IDirect3DBaseTexture9> oldExtraTextures[2];
    DWORD oldSampler[2][6]{};bool extraState=false;
    ComPtr<IDirect3DSurface9> oldMaskRT;bool maskBound=false;
    ComPtr<IDirect3DSurface9> oldDepthRT;bool depthBound=false;
    D3DVIEWPORT9 oldViewport{};
    DWORD oldColorWrite[2]{};
    bool targetState=false;

    void RestoreTargets() noexcept {
        if(!device||!targetState)return;
        if(maskBound)device->SetRenderTarget(1,oldMaskRT.Get());
        if(depthBound)device->SetRenderTarget(2,oldDepthRT.Get());
        device->SetRenderState(D3DRS_COLORWRITEENABLE1,oldColorWrite[0]);
        device->SetRenderState(D3DRS_COLORWRITEENABLE2,oldColorWrite[1]);
        device->SetViewport(&oldViewport);
        maskBound=depthBound=targetState=false;
    }

    void BindTargets(IDirect3DDevice9* d) {
        if(!waterTargetSurface||!waterMaskSurface||!waterDepthSurface)return;
        ComPtr<IDirect3DSurface9> current;
        D3DCAPS9 caps{};
        if(FAILED(d->GetRenderTarget(0,current.GetAddressOf()))||current.Get()!=waterTargetSurface||
           FAILED(d->GetDeviceCaps(&caps))||caps.NumSimultaneousRTs<3 ||
           !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING))return;
        D3DSURFACE_DESC colorDesc{},maskDesc{},depthDesc{};
        if(FAILED(current->GetDesc(&colorDesc))||FAILED(waterMaskSurface->GetDesc(&maskDesc))||
           FAILED(waterDepthSurface->GetDesc(&depthDesc))||
           !renderer::water::TargetsMatch(colorDesc.Width,colorDesc.Height,maskDesc.Width,maskDesc.Height)||
           !renderer::water::TargetsMatch(colorDesc.Width,colorDesc.Height,depthDesc.Width,depthDesc.Height)||
           colorDesc.MultiSampleType!=D3DMULTISAMPLE_NONE)return;
        if(colorDesc.Format!=D3DFMT_A8R8G8B8&&colorDesc.Format!=D3DFMT_X8R8G8B8&&
           !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS))return;
        // Creating an R32F target does not guarantee that alpha blending to it
        // is supported. An incompatible MRT can make the entire water draw fail.
        ComPtr<IDirect3D9> api;
        D3DDEVICE_CREATION_PARAMETERS creation{};D3DDISPLAYMODE mode{};
        if(FAILED(d->GetDirect3D(api.GetAddressOf()))||FAILED(d->GetCreationParameters(&creation))||
           FAILED(api->GetAdapterDisplayMode(creation.AdapterOrdinal,&mode))||
           FAILED(api->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,mode.Format,
               D3DUSAGE_RENDERTARGET|D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,D3DRTYPE_TEXTURE,depthDesc.Format)))return;
        // NOTFOUND means an unbound slot; other query failures cannot be safely restored.
        HRESULT maskHR=d->GetRenderTarget(1,oldMaskRT.GetAddressOf());
        HRESULT depthHR=d->GetRenderTarget(2,oldDepthRT.GetAddressOf());
        if((FAILED(maskHR)&&maskHR!=D3DERR_NOTFOUND)||(FAILED(depthHR)&&depthHR!=D3DERR_NOTFOUND)||
           FAILED(d->GetViewport(&oldViewport))||
           FAILED(d->GetRenderState(D3DRS_COLORWRITEENABLE1,&oldColorWrite[0]))||
           FAILED(d->GetRenderState(D3DRS_COLORWRITEENABLE2,&oldColorWrite[1])))return;
        targetState=true;
        HRESULT hr=d->SetRenderTarget(1,waterMaskSurface);
        maskBound=SUCCEEDED(hr);
        if(SUCCEEDED(hr)){hr=d->SetRenderTarget(2,waterDepthSurface);depthBound=SUCCEEDED(hr);}
        if(SUCCEEDED(hr))hr=d->SetRenderState(D3DRS_COLORWRITEENABLE1,15);
        if(SUCCEEDED(hr))hr=d->SetRenderState(D3DRS_COLORWRITEENABLE2,15);
        if(SUCCEEDED(hr))hr=d->SetViewport(&oldViewport);
        if(FAILED(hr)){RestoreTargets();ReportFailure("water MRT binding",hr);}
    }

    explicit Scope(IDirect3DDevice9* d,bool skip=false) noexcept {
        if(skip||!enabled||!active||!effectEnabled)return;
        try {
            if(!renderer::water::IsMaterial(d))return;
            ++frameCandidates;
            if(FAILED(d->GetPixelShader(original.GetAddressOf())) ||
               FAILED(d->GetVertexShader(originalVertex.GetAddressOf())))return;
            if(!original || !originalVertex)return;
            if(FAILED(d->GetPixelShaderConstantF(200,old,9)))return;
            if(FAILED(d->GetPixelShaderConstantF(180,oldLocalLights,17)))return;
            localLightsSaved=true;
            if(FAILED(d->GetVertexShaderConstantF(200,oldVertexControls,1)))return;
            // Relative time avoids both uptime-sized float precision loss and
            // the old discontinuous wave jump every ten minutes.
            static const double animationStart=renderer::PerformanceSeconds();
            float controls[36]={float(renderer::PerformanceSeconds()-animationStart),strength,normalStrength,specularStrength};
            if(FAILED(d->GetVertexShaderConstantF(33,controls+4,1))||FAILED(d->GetVertexShaderConstantF(35,controls+8,1)))return;
            // Prefer CelestialTracker's confirmed sun/moon direction (from
            // the actual disc draw call, same source the sky rays/glow use)
            // over WoW's own light-direction constant for the glint path -
            // that constant can drift from the visible disc (the whole
            // reason CelestialTracker exists), which showed up as the water
            // "sun road" pointing at empty sky instead of the real sun.
            // viewSpaceDirection already points TOWARD the source; the
            // shader negates lightDirection to get that vector, so upload
            // it pre-negated.
            {
                const renderer::CelestialBody& sunBody=renderer::CelestialTracker::Instance().Sun();
                const renderer::CelestialBody& moonBody=renderer::CelestialTracker::Instance().Moon();
                const renderer::CelestialBody* body=sunBody.visible?&sunBody:(moonBody.visible?&moonBody:nullptr);
                if(body){
                    controls[4]=-body->viewSpaceDirection.x;
                    controls[5]=-body->viewSpaceDirection.y;
                    controls[6]=-body->viewSpaceDirection.z;
                }
            }
            // Missing INTZ (e.g. MSAA) must still render animated modern water.
            // All-zero unpack constants produced 0/0 even with captureValid=0;
            // multiplying that NaN by a disabled feature does not remove it.
            if(reflectionScene&&reflectionDepth)memcpy(controls+12,reflectionData,sizeof(reflectionData));
            else {controls[12]=1.001f;controls[13]=-.1f;controls[14]=controls[15]=1.f;controls[19]=1.f;}
            controls[16]=reflectionsEnabled&&reflectionScene&&reflectionDepth?reflectionStrength:0;
            controls[20]=reflectionsEnabled?environmentStrength:0;controls[21]=reflectionDistance;controls[22]=reflectionThickness;controls[23]=reflectionScene&&reflectionDepth?1.f:0.f;
            controls[24]=flowSpeed;controls[25]=flowRotation;
            controls[26]=waterDebugMode;
            const float flowAngle=controls[0]*flowSpeed*.035f;
            controls[7]=std::sin(flowAngle);controls[27]=std::cos(flowAngle);
            controls[28]=refractionEnabled?refractionStrength:0;controls[29]=depthEnabled?absorptionStrength:0;controls[30]=foamEnabled?shoreFoamStrength:0;controls[31]=shoreFoamWidth;
            controls[1]=wavesEnabled?strength:0;controls[2]=wavesEnabled?normalStrength:0;
            controls[32]=wavesEnabled?swellStrength:0;controls[33]=sunGlintEnabled?sunGlintStrength:0;controls[34]=fresnelStrength;controls[35]=foamEnabled?crestFoamStrength:0;
            float vertexControls[4]={controls[0],geometryWavesEnabled?geometryWaveAmplitude:0,wavesEnabled?swellStrength:0,0};
            if(cachedShaderDevice != d) {
                cachedReplacementPS.Reset();cachedReplacementVS.Reset();
                cachedShaderDevice=d;shaderCreationFailed=false;
            }
            if(shaderCreationFailed || code.empty() || vertexCode.empty())return;
            if(!cachedReplacementPS || !cachedReplacementVS) {
                cachedReplacementPS.Reset();
                cachedReplacementVS.Reset();
                cachedShaderDevice = d;
                HRESULT hr=d->CreatePixelShader(code.data(), cachedReplacementPS.GetAddressOf());
                if(SUCCEEDED(hr))hr=d->CreateVertexShader(vertexCode.data(), cachedReplacementVS.GetAddressOf());
                if(FAILED(hr)) {
                    shaderCreationFailed=true;
                    D3DCAPS9 caps{};d->GetDeviceCaps(&caps);
                    std::ofstream(std::filesystem::path(logPath),std::ios::app)
                        <<"water shader creation failed hr="<<std::hex<<hr<<std::dec
                        <<" PS30 slots="<<caps.MaxPixelShader30InstructionSlots
                        <<"; keeping native water until device reset\n";
                    return;
                }
            }
            replacement = cachedReplacementPS;
            replacementVertex = cachedReplacementVS;
            const D3DSAMPLERSTATETYPE samplerStates[]={D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_SRGBTEXTURE};
            {
                for(unsigned slot=0;slot<2;++slot){if(FAILED(d->GetTexture(2+slot,oldExtraTextures[slot].GetAddressOf())))return;for(unsigned state=0;state<6;++state)if(FAILED(d->GetSamplerState(2+slot,samplerStates[state],&oldSampler[slot][state])))return;}
                device=d;extraState=true;
                if(FAILED(d->SetTexture(2,reflectionScene))||FAILED(d->SetTexture(3,reflectionDepth))){Restore();return;}
                for(unsigned slot=0;slot<2;++slot) {
                    const DWORD values[]={DWORD(slot?D3DTEXF_POINT:D3DTEXF_LINEAR),DWORD(slot?D3DTEXF_POINT:D3DTEXF_LINEAR),
                        D3DTEXF_NONE,D3DTADDRESS_CLAMP,D3DTADDRESS_CLAMP,FALSE};
                    for(unsigned state=0;state<6;++state) {
                        const HRESULT hr=d->SetSamplerState(2+slot,samplerStates[state],values[state]);
                        if(FAILED(hr)){Restore();ReportFailure("water sampler setup",hr);return;}
                    }
                }
            }
            if(FAILED(d->SetPixelShader(replacement.Get()))){Restore();return;}
            device=d;
            BindTargets(d);
            if(FAILED(d->SetVertexShader(replacementVertex.Get()))||FAILED(d->SetPixelShaderConstantF(200,controls,9))||FAILED(d->SetVertexShaderConstantF(200,vertexControls,1))){Restore();return;}
            {
                float lights[17][4]{};
                const auto& frame=renderer::FrameContext::Current();
                const auto& manager=renderer::LocalLightManager::Instance();
                if(reflectionsEnabled&&manager.Enabled()&&frame.cameraValid&&frame.viewRawValid){
                    const auto& selected=manager.Selected();unsigned count=std::min<unsigned>(8,unsigned(selected.size()));
                    for(unsigned i=0;i<count;++i){const auto& light=selected[i];const auto& m=frame.viewRaw.m;
                        for(int axis=0;axis<3;++axis)lights[i][axis]=light.position.x*m[0][axis]+light.position.y*m[1][axis]+light.position.z*m[2][axis]+m[3][axis];
                        lights[i][3]=light.radius;lights[8+i][0]=light.color.x;lights[8+i][1]=light.color.y;lights[8+i][2]=light.color.z;lights[8+i][3]=light.intensity*manager.IntensityScale();
                    }
                    lights[16][0]=float(count);lights[16][1]=reflectionStrength;
                }
                if(FAILED(d->SetPixelShaderConstantF(180,lights[0],17))){Restore();return;}
            }
            static bool reported=false;
            if(!reported){std::ofstream log(std::filesystem::path(logPath),std::ios::app);log<<"matched structural water texture layout; replacement active (shader variants accepted)\n";log<<"direction="<<controls[4]<<','<<controls[5]<<','<<controls[6]<<" color="<<controls[8]<<','<<controls[9]<<','<<controls[10]<<" specular="<<specularStrength<<'\n';reported=true;}
            static bool reflectionReported=false;if(reflectionScene&&reflectionDepth&&!reflectionReported){std::ofstream(std::filesystem::path(logPath),std::ios::app)<<"SSR scene+depth input active strength="<<reflectionStrength<<" environment="<<environmentStrength<<'\n';reflectionReported=true;}
            ++frameMatches;
            if(reflectionScene&&reflectionDepth)++frameSceneMatches;
        }catch(...){Restore();}
    }
    void Restore() noexcept {
        if(!device)return;
        RestoreTargets();
        if(localLightsSaved)device->SetPixelShaderConstantF(180,oldLocalLights,17);
        device->SetPixelShader(original.Get());
        device->SetVertexShader(originalVertex.Get());
        device->SetPixelShaderConstantF(200,old,9);
        device->SetVertexShaderConstantF(200,oldVertexControls,1);
        if(extraState) {
            const D3DSAMPLERSTATETYPE states[]={D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,
                D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_SRGBTEXTURE};
            for(unsigned slot=0;slot<2;++slot) {
                device->SetTexture(2+slot,oldExtraTextures[slot].Get());
                for(unsigned state=0;state<6;++state)
                    device->SetSamplerState(2+slot,states[state],oldSampler[slot][state]);
            }
        }
        device=nullptr;extraState=localLightsSaved=false;
    }
    ~Scope(){Restore();}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
}
