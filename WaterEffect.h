#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include "src/Environment/LocationTuning.h"
#include <unordered_set>
#include <cstdio>
#include "src/D3D9/TrackedRenderState.h"
#include "src/D3D9/CelestialTracker.h"
#include "src/D3D9/ShaderBytecodeCache.h"
#include "src/D3D9/ScopedPassState.h"
#include "src/Lighting/LocalLightManager.h"
#include "src/Core/FrameClock.h"
#include "src/Core/FrameContext.h"
#include "src/Water/WaterMaterial.h"
#include "src/Water/WaveField.h"
#include "src/Water/WaterSeaState.h"
#include "src/Water/WaveMotion.h"
#include "src/Water/WaterQuality.h"
namespace watereffect {
using Microsoft::WRL::ComPtr;
// Backend readiness, NOT the user's switch. Keep shaders/hooks available even
// when water starts off, so F7/F12 and location changes can enable it live.
// enabled is published by the startup compile thread (see Configure), so it
// is atomic: the render thread reads it on every draw and must not race the
// one-time publish. effectEnabled is the single live switch; active is the
// F9/F11 master switch.
std::atomic<bool> enabled{false};
std::atomic<bool> compiling{false}; // true from Configure until the compile thread publishes
inline std::mutex waterCompileMutex; // guards the code/vertexCode publish in Configure
bool active=true, keyDown=false, hotkey=true;
bool effectEnabled=true,wavesEnabled=true,geometryWavesEnabled=true,reflectionsEnabled=true,refractionEnabled=true,depthEnabled=true,foamEnabled=true,sunGlintEnabled=true;
float strength=.003f;
float normalStrength=.16f, specularStrength=.55f;
float reflectionStrength=.70f,environmentStrength=.24f,reflectionDistance=120.f,reflectionThickness=1.8f;
float flowSpeed=1.f,flowRotation=.32f;
float refractionStrength=.12f,absorptionStrength=.18f,shoreFoamStrength=.12f,shoreFoamWidth=2.5f;
float swellStrength=.45f,sunGlintStrength=1.35f,fresnelStrength=1.f,crestFoamStrength=.18f;
// Wave height, in percent of the shipped sea (100 = the reference look). It is a percent
// of a real amplitude in world yards rather than an amplitude itself, so the number a
// player sets means the same thing at every wave size and in every zone: see
// BuildGeometryWaves in src/Water/WaveField.h, which folds the wave size and the sea state
// into it and clamps the Gerstner steepness so a wave can never fold over itself.
float geometryWavePercent=100.f;
// Size of one ripple lattice cell and how fast its detail fades when a pixel
// spans more than a cell. Both are world-space (yards), because the wave field
// is anchored to the terrain rather than to a tile's uv.
float waveScalePercent=100.f,waveSofteningPercent=100.f;
// Per-body authoring, exactly the three controls the reference renderer exposes:
// movement (flow, wave size, wind), look (clarity, absorption colour, deep colour,
// water tint) and reflection (SSR strength, sheen, the reflected sun/moon disc).
// Every one of them is read per area first, so a murky swamp and a clear mountain
// lake can differ without a code change.
float clarityPercent=100.f,windStrengthPercent=100.f,roughnessPercent=100.f;
float sheenPercent=50.f,shoalFoamPercent=100.f,sunDiscPercent=40.f,foamBrightPercent=100.f;
float sparklePercent=100.f;
unsigned waterColourTint=0x808080u,deepColour=0x0e2a28u,absorptionColour=0xff8c6bu;
// Whether the current location has an explicit player-authored colour override.
// Resolve these once on tuning/location reload: asking the Windows profile API
// up to nine times for EVERY harbour tile was avoidable render-thread I/O.
// The location cannot change in the middle of a draw; Sync() always causes
// ReloadTuning before the next frame.
bool scopedWaterColour=false,scopedDeepColour=false,scopedAbsorptionColour=false;
// The sea state: what this zone asked for and what the sky is doing, eased once per frame in
// Present() and multiplied into the controls below. It is the identity when a zone is untuned and
// the weather is clear, which is what lets `ZoneWaves` default to on.
float seaAgitation=1.f,seaMotion=1.f,seaShear=0.f,seaWind=1.f;
float weatherResponse=1.f,weatherEaseSeconds=14.f;
bool zoneWavesEnabled=true;
renderer::water::SeaWeather seaWeather;
renderer::water::SeaStateFilter seaFilter;
renderer::water::WaveMotion waveMotion;
renderer::water::DrawAnchor lastWaveAnchor;
renderer::FrameClock seaFrameClock;
void AdvanceSea(float dt);
// WATER QUALITY, 0..10, and the plan it resolves to. The level is the authored number; the
// plan is everything both shader stages and this file are allowed to ask about it, and it is
// resolved once per tuning reload rather than per tile, because the ladder lives in
// src/Water/WaterQuality.h and a per-draw lookup would be a per-draw way to disagree with it.
int waterQuality=renderer::water::kQualityDefault;
renderer::water::QualityPlan qualityPlan=renderer::water::QualityPlanFor(renderer::water::kQualityDefault);
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
D3DSURFACE_DESC waterTargetDesc{};
bool waterTargetDescValid=false;
// Selected local lights and the captured main view are immutable throughout a
// frame. Transforming the same eight lights into the same view once per liquid
// tile is wasted CPU work in cities; cache the 17-register upload payload by
// frame and the manager's content signature. D3D state is still saved/restored
// per draw because the client may change it between tiles.
struct LocalLightConstantCache {
    std::uint64_t frameIndex=0,selectionSignature=0;
    float intensityScale=0,reflection=0;
    bool managerEnabled=false,reflections=false,cameraValid=false,viewValid=false,valid=false;
    // WATER QUALITY's emitter cap is part of the cached payload's identity: a level change has
    // to invalidate it, or the tiles of the next frame keep mirroring the light count the
    // previous level asked for until something else happens to invalidate the entry.
    int localLights=-1;
    float values[68]{};
};
LocalLightConstantCache localLightConstantCache;
struct WaterMrtDeviceInfo {
    IDirect3DDevice9* device=nullptr;
    bool checked=false;
    bool supported=false;
    D3DCAPS9 caps{};
};
inline WaterMrtDeviceInfo waterMrtInfo;
inline void ResetMrtCapabilities(IDirect3DDevice9* device=nullptr) {
    waterMrtInfo={};waterMrtInfo.device=device;
}
inline bool SupportsWaterMrt(IDirect3DDevice9* device) {
    if(!device)return false;
    if(waterMrtInfo.device!=device)ResetMrtCapabilities(device);
    if(waterMrtInfo.checked)return waterMrtInfo.supported;
    auto& caps=waterMrtInfo.caps;
    if(FAILED(device->GetDeviceCaps(&caps)))return false; // retry after transient device errors
    if(caps.NumSimultaneousRTs<3||
       !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING)) {
        waterMrtInfo.checked=true;
        return false;
    }
    ComPtr<IDirect3D9> api;
    D3DDEVICE_CREATION_PARAMETERS creation{};D3DDISPLAYMODE mode{};
    if(FAILED(device->GetDirect3D(api.GetAddressOf()))||
       FAILED(device->GetCreationParameters(&creation))||
       FAILED(api->GetAdapterDisplayMode(creation.AdapterOrdinal,&mode)))
        return false; // retry transient query failures instead of disabling the water path
    if(FAILED(api->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,mode.Format,
           D3DUSAGE_RENDERTARGET|D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
           D3DRTYPE_TEXTURE,D3DFMT_R32F))) {
        waterMrtInfo.checked=true;
        return false;
    }
    waterMrtInfo.checked=true;
    waterMrtInfo.supported=true;
    return true;
}
std::vector<DWORD> code,vertexCode;
std::wstring shaderCacheDir;
// True when the published bytecode came from the on-disk cache: if the
// driver then refuses it, the consumer recompiles once before giving up
// (the fallback for a stale/corrupt cache entry, see the Create site).
bool codeFromCache = false;
inline ComPtr<IDirect3DPixelShader9> cachedReplacementPS;
inline ComPtr<IDirect3DVertexShader9> cachedReplacementVS;
inline IDirect3DDevice9* cachedShaderDevice = nullptr;
inline bool shaderCreationFailed = false;
inline bool NeedsScene() {
    // WATER QUALITY can remove the reason to take the copy at all: below level 2 there is no
    // screen-space reflection march and below level 3 no celestial occlusion march, so at
    // those levels the two features stop asking for a full-resolution scene/depth capture they
    // would never read. Refraction, depth absorption and foam still ask - the shore, the shoal
    // and the back-flow are all read out of that depth buffer at every level that has them.
    return enabled && active && effectEnabled && renderer::water::NeedsScene(
        reflectionsEnabled&&qualityPlan.ssrSteps>0, refractionEnabled, depthEnabled, foamEnabled,
        sunGlintEnabled&&qualityPlan.lightSteps>0);
}
bool showStatus=false;
unsigned frameMatches=0,frameCandidates=0,frameSceneMatches=0,frameNearMisses=0;
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
// x=peak lift in world yards, y=peak slide toward a crest in yards (Gerstner's Q*A),
// z=sigma, the dimensionless pinch the pixel stage sharpens its slope by, w=one
// lattice cell in yards. Zero amplitude is flat water, which is how WAVES OFF and a
// zone authored calm both arrive here: the stage collapses to the client's surface.
float4 waveControl:register(c200);
// Rows of the world->lattice matrix; the same value c209 carries to the PS.
float4 waveLattice:register(c201);
// xy=the phase of the two swell trains this stage displaces, in cells ALONG THEIR OWN
// DIRECTIONS and wrapped into their own wavelengths: the pixel shader's c210.zw, verbatim.
// Sharing those numbers is what keeps a displaced crest and the light on it on the same
// wave, and because each is reduced into a period of its own train, the wrap costs this
// stage nothing - the displacement needs no anchor of its own.
// z=the phase of swell train C, the twenty yard wave WATER QUALITY 8 and above also lifts,
// in the same units and the same wrap - it is c217.x, the phase the pixel stage shades that
// train with, so a crest this stage lifts is the crest the pixels light. w unused.
float4 wavePhase:register(c202);
// xy=the camera anchor for the low-frequency, world-anchored wave warp. Its
// phase is reduced in the warp noise's own frequency/period, not borrowed from
// the higher-frequency chop's phase.
float4 waveWarpPhase:register(c203);
// xy=the camera anchor for the wave-grouping envelope, in that field's own noise
// period. It never reaches the pixel stage: these pixels get the interpolated value
// and gradient through the spare lanes of TEXCOORD4/TEXCOORD7 instead, which is what keeps a
// set of waves nailed to the terrain while costing the pixel shader one multiply and no
// interpolant register (see Output below for why there is no twelfth one).
// z=the mid-wave gate: 1 when the mesh lifts train C as well as the two longest, 0 when it
// carries the two it has always carried. It is a multiplier rather than a branch because it
// has to scale the renormalization below at the same time as the train it lets in.
float4 waveGroupPhase:register(c204);
// The low-frequency warp and the grouping envelope use the same bounded value-noise
// hash as the pixel stage. This compact vertex copy keeps lifted crests on the same
// irregular swell field the pixel shader shades, and both fields are far smoother than
// the liquid mesh, so the vertex stage can afford to sample them and let the pixels
// interpolate: the wave they agree on is the wave the player sees.
float3 noiseGradient(float2 p) {
    float2 i=floor(p),f=p-i;
    i-=floor(i*(1.0/64.0))*64.0;
    float4 h=frac((i.x+i.y*64.0+float4(0,1,64,65))*.1031);
    h*=h+33.33;
    h=frac(h*(h+h));
    float2 u=f*f*(3-2*f),du=6*f*(1-f);
    float k=h.x-h.y-h.z+h.w;
    return float3(h.x+(h.y-h.x)*u.x+(h.z-h.x)*u.y+k*u.x*u.y,
                  du.x*((h.y-h.x)+k*u.y),du.y*((h.z-h.x)+k*u.x));
}
// The swell trains this stage displaces, spelled exactly as src/Water/WaveField.h carries
// them: unit directions in lattice space, k = 2pi over the wavelength in cells, the share of
// the authored amplitude each one lifts, and the train's own start in cells. The two longest
// are always displaced; train C - the twenty yard wave - is displaced at WATER QUALITY 8 and
// above, and is the shortest wave the client's liquid grid (about four yards apart) can carry
// without faceting, at five facets to a crest. Everything shorter is shaded but NOT displaced:
// lifting a wave on fewer facets than that is what facets, which is why the pixel stage
// carries eight trains and this stage carries at most three.
static const float2 kDirA=float2(1.0,0.0);
static const float2 kDirB=float2(0.8290376,-0.5591929);
static const float2 kDirC=float2(0.9335804,0.3583679);
static const float kRadA=0.39269908;
static const float kRadB=0.54636394;
static const float kRadC=0.78539816;
static const float kWeightA=1.0;
static const float kWeightB=0.62;
static const float kWeightC=0.34;
static const float kStartA=0.0;
static const float kStartB=4.255;      // .37 of train B's own wavelength
static const float kStartC=5.68;       // .71 of train C's own wavelength
// The grouping envelope, in the same units the pixel stage multiplies with: base plus
// depth times the noise value, and the gradient already scaled by depth and frequency
// (kGroupFrequency*kGroupDepth) so the pixels can use it as a product rule directly.
// Both are spelled from src/Water/WaveField.h, which tools/WaterRegression.cpp checks.
static const float kGroupBase=0.52;
static const float kGroupDepth=0.62;
static const float kGroupScale=0.01347826;
// Twelve interpolants would be one too many: without a register declared as psize0,
// vs_3_0 offers eleven o# registers and ps_3_0 accepts ten v# registers, and fxc says so with
// X5622/X5300 rather than with anything a reader can guess from the source. The envelope is
// therefore packed into the lanes that were already there - the gain rides in waveWarp.w, the
// gradient in worldRel.zw - because both of those registers exist for other reasons and each
// had a free component. The native UV registers also had four unused lanes. They carry the
// sine/cosine pairs of the two long trains this stage ALREADY evaluates for displacement.
// The pixel stage renormalizes each interpolated pair before assembling height, slope and pinch:
// this follows the original analytic curve much more closely than interpolating those aggregates,
// while still removing the expensive per-pixel projections and sin/cos evaluations. It adds no
// interpolator or texture sample.
struct Output {float4 pos:POSITION;float4 color:COLOR0;float4 uv0:TEXCOORD0;float4 uv1:TEXCOORD1;float fog:FOG;float3 viewPos:TEXCOORD2;float3 viewNormal:TEXCOORD3;float4 worldRel:TEXCOORD4;float3 waveTangent:TEXCOORD5;float3 waveBitangent:TEXCOORD6;float4 waveWarp:TEXCOORD7;};
Output main(float4 pos:POSITION,float3 normal:NORMAL,float4 color:COLOR0,float2 uv0:TEXCOORD0,float2 uv1:TEXCOORD1) {
    Output o;
    float4 p=modelView[0]*pos.x+modelView[1]*pos.y+modelView[2]*pos.z+modelView[3];
    // The world axes, in view space. This stage builds p as sum_j modelView[j]*pos[j],
    // so the matrix it applies is the transpose of the array as written, and world axis
    // i therefore arrives in view space as modelView[i].xyz - the same convention the
    // normal transform four lines below has always used. Reading the components ACROSS
    // the rows instead (float3(modelView[0].x,modelView[1].x,modelView[2].x)) yields the
    // view axis expressed in WORLD coordinates; dotting that with a view-space position
    // applies the camera's rotation twice over, so the coordinate that was supposed to
    // be world-anchored turned when the camera turned and slid when it moved. That one
    // transposition is also why the displacement below used
    // float3(modelView[0].z,modelView[1].z,modelView[2].z) - the view direction, not the
    // up axis - so geometry waves pushed water toward and away from the camera instead of
    // lifting it, and the surface could only read as curves drawn on a flat plane.
    float3 axisX=modelView[0].xyz;
    float3 axisY=modelView[1].xyz;
    float3 axisZ=modelView[2].xyz;
    // worldRel is the distance from the camera in WORLD axes: small and exact at any
    // world coordinate, identical for two neighbouring tiles - which is what keeps a wave
    // field from restarting at every tile edge - and it does not turn when the camera
    // turns. The camera's own position is added by the pixel shader as a constant phase
    // read from THIS draw's own matrix, so this is the only coordinate a wave field is
    // ever built from, and neither half can be a frame or a camera behind the other.
    float2 rel=float2(dot(axisX,p.xyz),dot(axisY,p.xyz));
    float2 cell=float2(dot(rel,waveLattice.xy),dot(rel,waveLattice.zw));
    // A slowly varying pseudorandom domain warp bends crests over the surface instead
    // of repeating eight perfect sine trains, and a second, broader field groups those
    // trains into sets. c203 and c204 carry the camera contribution after each
    // field's own scale/rotation, so `cell` remains relative and neither the bend nor
    // the grouping slides at tile seams or when the camera moves. Both are sampled
    // here, once per vertex, and handed to the pixels interpolated: at fifty and a
    // hundred yards a feature is far smoother than the liquid mesh, so interpolating
    // them costs nothing a player can see and buys the pixel stage two noise
    // evaluations it does not have to pay for.
    float2 warpCoord=float2(dot(cell,float2(.8253356,.5646425)),
                            dot(cell,float2(-.5646425,.8253356)))*.05+waveWarpPhase.xy;
    float3 waveWarp=noiseGradient(warpCoord);
    float warp=(waveWarp.x-.5)*6.5;
    float2 warpGradient=.05*float2(waveWarp.y*.8253356-waveWarp.z*.5646425,
                                   waveWarp.y*.5646425+waveWarp.z*.8253356);
    float2 groupCoord=float2(dot(cell,float2(.9702957,.2419219)),
                             dot(cell,float2(-.2419219,.9702957)))*.02173913+waveGroupPhase.xy;
    float3 waveGroup=noiseGradient(groupCoord);
    float2 groupGradient=kGroupScale*float2(waveGroup.y*.9702957-waveGroup.z*.2419219,
                                            waveGroup.y*.2419219+waveGroup.z*.9702957);
    // How much of its swell THIS patch of water is carrying. Everything the wave field
    // does - the lift here, the shading slope, the pinch, the chop and the whitecaps in
    // the pixel stage - is multiplied by this one number, so a set arrives, breaks and
    // leaves instead of the whole sea marching at once.
    float gain=kGroupBase+kGroupDepth*waveGroup.x;
    float2 swellCell=cell+warp*float2(.6,.8);
    // Gerstner waves: lift the surface, then slide it toward the coming crest. The slide
    // is the part that makes water read as a body moving in three dimensions - crests
    // pinch and steepen, troughs open out and flatten - and it is why the same amplitude
    // looks like a sea rather than like a sine viewed from the side. Both come from the
    // trains the pixel stage shades, from the shared phase above, so a crest that is
    // lifted is a crest that is lit.
    float thetaA=kRadA*(dot(kDirA,swellCell)+wavePhase.x+kStartA);
    float thetaB=kRadB*(dot(kDirB,swellCell)+wavePhase.y+kStartB);
    float sinA,cosA,sinB,cosB;
    sincos(thetaA,sinA,cosA);
    sincos(thetaB,sinB,cosB);
    // MID-WAVES: train C, the twenty yard wave, at WATER QUALITY 8 and above. It is evaluated
    // from the same wrapped phase the pixel stage shades that train with (c217.x), on the same
    // warped cell and inside the same grouping envelope, so the crest this stage lifts is the
    // crest those pixels light - the property the two long trains were packed into UV lanes
    // for, and the one a mid-scale wave has to keep or it reads as a second unrelated sea
    // moving under the first. It is the scale the eye reads as "waves" rather than as swell or
    // as texture: with only A and B displaced, the silhouette against a shore, a hull or the
    // horizon stays flat while the shading on it moves, which is the tell that a sea is a
    // picture. Train D (fourteen and a half yards) is why this stops at three - below four
    // mesh facets to a crest, a displaced wave is a staircase no normal map can hide.
    float midGate=waveGroupPhase.z;
    float thetaC=kRadC*(dot(kDirC,swellCell)+wavePhase.z+kStartC);
    float sinC,cosC;
    sincos(thetaC,sinC,cosC);
    // WAVE HEIGHT is a peak lift in yards, so letting a third train in has to add SHAPE and
    // not SIZE. Dividing the combined lift and slide by the displaced weight sum, times the
    // two-train sum, leaves both the peak height and the Gerstner slide/lift ratio exactly
    // what they were, and makes the fold budget smaller rather than larger: three trains now
    // share the one authored height. See kMeshPinchMax in src/Water/WaveField.h, which is the
    // number the fold clamp is written against.
    float meshScale=(kWeightA+kWeightB)/(kWeightA+kWeightB+midGate*kWeightC);
    // These two trig pairs used to be evaluated a second time per pixel. Pack
    // them into spare UV lanes; the pixel stage normalizes the interpolated
    // pairs, combines them with the six shorter trains and applies the shared
    // chain/product rule. The aggregate remains here only for mesh lift. Train C
    // has no spare lane left (there are exactly eleven, and the envelope already
    // lives in two of them), so it is evaluated in both stages instead - which
    // costs the pixels nothing they were not already spending, because C is one
    // of the six trains the shading has always evaluated for itself.
    float longSwell=(kWeightA*sinA+kWeightB*sinB+midGate*kWeightC*sinC)*meshScale;
    float lift=waveControl.x*longSwell*gain;
    float2 warpedDirA=kDirA+6.5*dot(kDirA,float2(.6,.8))*warpGradient;
    float2 warpedDirB=kDirB+6.5*dot(kDirB,float2(.6,.8))*warpGradient;
    float2 warpedDirC=kDirC+6.5*dot(kDirC,float2(.6,.8))*warpGradient;
    float2 slide=waveControl.y*meshScale*gain
        *(kWeightA*cosA*warpedDirA+kWeightB*cosB*warpedDirB+midGate*kWeightC*cosC*warpedDirC);
    // slide is in lattice axes; waveControl.w turns the scaled lattice rows back into the
    // unit world directions they were built from, and the same three axes carry the result
    // into view space. Displacement happens after the view transform and along the world
    // up axis, because a tile's uv restarts every 533 units and would stamp identical
    // square bumps onto every tile.
    float2 lateral=waveControl.w*(slide.x*waveLattice.xy+slide.y*waveLattice.zw);
    p.xyz+=axisZ*lift+axisX*lateral.x+axisY*lateral.y;
    o.pos=projection[0]*p.x+projection[1]*p.y+projection[2]*p.z+projection[3]*p.w;
    o.fog=min(pow(max(p.z*fogParams.x+fogParams.y,0),fogParams.z),1);
    // xy keep the client's native UVs byte-for-byte. zw are otherwise unused.
    // Pack by component so the pixel stage can normalize A/B together: uv0 has
    // both sines, uv1 both cosines.
    o.uv1=float4(uvAX.xy*uv0.x+uvAY.xy*uv0.y+uvAOffset.xy,cosA,cosB);
    o.uv0=float4(uvBX.xy*uv1.x+uvBY.xy*uv1.y+uvBOffset.xy,sinA,sinB);
    float3 n=modelView[0].xyz*normal.x+modelView[1].xyz*normal.y+modelView[2].xyz*normal.z;
    o.color=float4(diffuse.rgb*saturate(dot(-lightDirection.xyz,n))+ambient.rgb,1)*color;
    // viewPos is the DISPLACED point, so the surface depth the atmosphere marches to and
    // the column the absorption measures are the water the player can actually see;
    // worldRel is the undisplaced lattice coordinate where the pixel stage evaluates the
    // swell trains, while the broad warp sample is passed separately for interpolation.
    o.viewPos=p.xyz;o.viewNormal=n;o.worldRel=float4(rel,groupGradient);
    // The wave gradient is expressed in world axes; the same three axis vectors rotate it
    // into view space here so the pixel shader only has to scale them by the wave slope:
    // no screen-space approximation and no per-triangle winding flip in the shading normal.
    o.waveTangent=axisX*waveLattice.x+axisY*waveLattice.y;
    o.waveBitangent=axisX*waveLattice.z+axisY*waveLattice.w;
    // Pass the broad warp and the grouping envelope, each with its gradient, to the
    // pixel stage. At their fifty and hundred and fifteen yard scales both fields are
    // smooth across the liquid mesh, so perspective interpolation is accurate while
    // avoiding a second and third noise evaluation per pixel.
    // xyz = the warp's value and its gradient in lattice-cell axes; w = the grouping gain,
    // which the pixel stage multiplies into exactly the quantities this stage used it on.
    o.waveWarp=float4(waveWarp,gain);
    return o;
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
// x=sigma, the Gerstner pinch coefficient the vertex stage displaced with (its lateral
// slide over one lattice cell): it is what turns the shading slope into the slope of the
// surface that is actually there. y=ripple uv offset,z=normal response,w=specular.
float4 controls:register(c200);
float4 lightDirection:register(c201); // xyz=direction toward the sun/moon
float4 lightColor:register(c202);
// x=depth unpack A,y=depth unpack B with the viewport MaxZ folded in, so a
// captured depth costs one subtraction and one divide; z,w=screen scale.
float4 reflectionProjection:register(c203);
// x=SSR strength, y=screen-space reflection march steps, z=celestial occlusion march
// steps. Both counts come from the WATER QUALITY ladder in src/Water/WaterQuality.h and
// are trip counts, not gates: 0 skips the loop through the same test that already
// stopped it, and the loops are written against 12 so a level can hand them any count
// up to that without a second compiled shader. w=the capture's depth range, uploaded
// with the block and not read by this stage.
float4 reflectionControl:register(c204);
// x=environment strength,y=max trace distance,z=base thickness,w=capture valid
float4 reflectionStyle:register(c205);
// x=wave anti-alias gain, yz=refraction uv scale, w=debug mode
float4 flowControl:register(c206);
// x=refraction, y=depth absorption, z=shore foam, w=foam depth width
float4 waterStyle:register(c207);
// x=long swell strength, y=sun glitter, z=Fresnel response, w=crest foam
float4 foreverStyle:register(c208);
// Rows of the world->lattice matrix, scaled by 1/cell. The orientation is FIXED: it used
// to turn with the flow clock, and rotating a world-anchored basis rotates the whole sea
// about the world ORIGIN, which at ten thousand yards out is a translation of hundreds of
// yards per second. Wave directions come from the trains below instead.
float4 waveLattice:register(c209);
// xy=chop phase in cells, wrapped into the noise lattice. zw still mirror the
// vertex stage's A/B phases for ABI stability, but the pixel stage now receives
// those two already-evaluated long trains through spare UV lanes instead of
// spending the same sin/cos work again for every covered pixel.
float4 wavePhase:register(c210);
// xy=drift of the client's detail map in its uv, z=WATER QUALITY (0..10), w=the wave gate:
// 0 with WAVES off - and 0 at quality FLAT, which is the same thing asked for from the
// other direction - so no slope, no distortion, no roughness and no foam survives a switch
// that is supposed to remove the waves. The quality lane is the level itself rather than a
// pile of resolved booleans: this stage compares it against the gates in
// src/Water/WaterQuality.h, spelled there as `level - .5` so a level can never sit on a
// boundary, and one lane is why the block the CPU saves, uploads and restores per tile did
// not have to grow by a register to get a quality slider.
float4 waveFlow:register(c211);
// xyz=world up in view space, taken from THIS draw's own matrix (a frame-captured camera
// is a frame behind, and the client's reflection pass is a different camera entirely),
// w=wind strength: how much sea state this body of water is actually in. Everything that
// whips up foam, breaks a reflection or decides what the sky is, is scaled by it, so a
// river authored calm stays glass and a coast in a squall does not.
float4 upWind:register(c212);
// x=how deep light reaches (per view column), y=specular roughness gain,
// z=sheen floor, w=shoal foam gain
float4 bodyStyle:register(c213);
// rgb=water tint (0x808080 is neutral, it multiplies the client's own colour),
// a=gain on the reflected sun/moon disc
float4 waterTint:register(c214);
// rgb=the colour light converges to at depth, a=foam brightness
float4 deepTint:register(c215);
// rgb=per-channel absorption bias (a red-heavy value kills red first), w=sparkle gain
float4 absorbTint:register(c216);
// xyzw=the phase of swell trains C, D, E and F, in cells along their own direction and
// wrapped into their own wavelength, exactly like wavePhase.zw carries A and B. Four of
// them in one register rather than spread across three is what lets the pixel stage add
// the four projections of one batch in a single instruction: a float4 the shader can
// swizzle whole is cheaper than three the shader has to gather.
float4 waveSwell:register(c217);
// xy=the phase of swell trains G and H, in the same units as c217; z=the shore back-flow
// run-up in yards, 0 with the feature off (which is how every gate that reads it also reads
// "no", so a level below it cannot move a shoreline); w unused. The two shortest trains get
// their own register for the same reason C-F share one: a phase has to arrive contiguous to
// be added to a batch of projections in one instruction.
float4 waveSwell2:register(c218);
float3 safeNormalize(float3 v) {return v*rsqrt(max(dot(v,v),1e-8));}
// Analytic value noise on one rotated lattice, evaluated for all four corner
// hashes at once. Every hash argument is folded into a bounded range first:
// multiplying a raw lattice index by a large constant (the previous
// dot(i,float2(127.1,311.7))) ran out of float32 fraction bits at ordinary
// world distances, the noise collapsed onto its own lattice and hardened into
// flat axis-aligned squares. Indices here are wrapped to [0,64), so the largest
// argument is 4095 and .x/.y/.z never depend on where in the world you stand.
// .x is the value, .y/.z its exact partial derivatives.
float3 noiseGradient(float2 p) {
    float2 i=floor(p),f=p-i;
    i-=floor(i*(1.0/64.0))*64.0;
    float4 h=frac((i.x+i.y*64.0+float4(0,1,64,65))*.1031);
    h*=h+33.33;
    h=frac(h*(h+h));
    float2 u=f*f*(3-2*f),du=6*f*(1-f);
    float k=h.x-h.y-h.z+h.w;
    return float3(h.x+(h.y-h.x)*u.x+(h.z-h.x)*u.y+k*u.x*u.y,
                  du.x*((h.y-h.x)+k*u.y),du.y*((h.z-h.x)+k*u.x));
}
// The swell: EIGHT ANALYTIC, DIRECTIONAL sine trains on that one lattice, in a fan whose
// crest lines (0, -34, 21, 47, 83, 110, 65, 128 degrees) are never closer than 18 degrees
// apart, with wavelengths in a ~1.35 ratio (40, 28.75, 20, 14.5,
// 11, 8.5, 6.4 and 4.9 yards) and each train starting at an irregular point along its own
// direction. Three trains were the previous answer to "the water is a picture of curves
// drawn on a flat plane", and six were the answer to the next complaint; eight is the
// answer to the one after that, which was also true: with the band ending at 8.5 yards
// the eye regrouped the six surviving crest families into lines again, because nothing
// crossed them at the scale the chop lives at. What a real sea shows
// is the INTERFERENCE of a band of wavenumbers, and a band of eight incommensurate trains
// has no repeat length in it at all: crests grow and die, double up, curve around each
// other and change spacing, because there is no translation that leaves all eight phases
// intact. The grouping envelope below then decides how much of that swell each patch of
// water is carrying, which is the part a sine comb can never fake.
//
// Eight trains evaluated one at a time would cost eight copies of the same five
// operations. A/B are already evaluated for geometry in the vertex stage, so their
// sin/cos pairs arrive in uv0.zw / uv1.zw instead of being evaluated twice. The
// six shading-only trains stay in vector batches of four and two. One vector per
// COMPONENT of the directions lets `plane.x*kDirX+plane.y*kDirY` project all trains
// in a batch in two instructions, and turns their slopes back into one gradient with
// two dot products. The complete eight-train interference and every authored weight
// are unchanged; only the stage doing redundant long-wave work changed.
// tools/WaterRegression.cpp holds the packed A/B expressions, these six literals and
// the CPU table in src/Water/WaveField.h to each other.
static const float2 kDirX01=float2(1.0,0.8290376);      // trains A and B: 0 and -34 degrees
static const float2 kDirY01=float2(0.0,-0.5591929);
static const float2 kRad01=float2(0.39269908,0.54636394);
static const float2 kWgt01=float2(1.0,0.62);
static const float4 kDirX4=float4(0.9335804,0.6819984,0.1218693,-0.3420201);   // 21, 47, 83, 110 degrees
static const float4 kDirY4=float4(0.3583679,0.7313537,0.9925462,0.9396926);
static const float4 kRad4=float4(0.78539816,1.08330781,1.42799666,1.84799568);
static const float4 kWgt4=float4(0.34,0.19,0.12,0.075);
static const float4 kStart4=float4(5.68,1.102,2.42,2.822);   // and of C, D, E and F
static const float2 kDirX2=float2(0.4226183,-0.6156615);    // trains G and H: 65 and 128 degrees
static const float2 kDirY2=float2(0.9063078,0.7880108);
static const float2 kRad2=float2(2.46399424,3.22214631);    // 2pi over 2.55 and 1.95 cells
static const float2 kWgt2=float2(0.052,0.036);
static const float2 kStart2=float2(0.7395,1.2285);          // .29 and .63 of their own wavelengths
// 1/(2*sum of the weights): maps the summed swell onto 0..1 for the crest masks, so the
// mask thresholds stay authored in fractions of the largest wave this sea can make. The
// grouping envelope can carry a crest up to 1.14 times past it, which is where the foam
// thresholds saturate, and that is the intent: the tallest crest of a set caps white.
static const float kSwellToUnit=0.20550760;
// The grouping envelope's own two constants, spelled exactly as the vertex stage
// multiplied the noise by them, and as src/Water/WaveField.h declares them.
static const float kGroupBase=0.52;
static const float kGroupDepth=0.62;
// The shortest baseline the bed slope may be measured over, in world yards. A per-pixel depth
// difference divided by a per-pixel world footprint is a slope, but only while that footprint is
// wide enough to be measuring the bed rather than the depth buffer's own quantization - and at
// the water's edge one pixel is a few inches. 1.5 yards is the span tools/WaterPreview.cpp's
// FieldSlope samples (+-1.5 yd), so the shader and the mirror the look was tuned against read
// the same baseline; it is a floor, so any coarser pixel keeps its own footprint and distant
// water is untouched.
static const float kBedSlopeSpanYards=1.5;
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
WaterOutput main(float4 color:COLOR0,float4 uv0:TEXCOORD0,float4 uv1:TEXCOORD1,float fog:FOG,float3 viewPos:TEXCOORD2,float3 viewNormal:TEXCOORD3,float4 worldRel:TEXCOORD4,float3 waveTangent:TEXCOORD5,float3 waveBitangent:TEXCOORD6,float4 waveWarp:TEXCOORD7) {
    WaterOutput result_;
    result_.surfaceDepth=float4(viewPos.z,0,0,1);
    result_.mask=float4(1,1,1,1);
    // Preserve the original alpha: surfaceTexture never contributes alpha.
    // uv0.zw/uv1.zw carry long-wave values; native texture addressing remains xy.
    float4 base=tex2D(baseTexture,uv0.xy);
    // One world-anchored lattice carries every scale. The swell is analytic - rounded
    // forms, exact slopes for free, and the same waves the vertex stage displaces, so a
    // crest and the light on it cannot drift apart - and only the chop on top of it is
    // noise.
    float2 plane=float2(dot(worldRel.xy,waveLattice.xy),dot(worldRel.xy,waveLattice.zw));
    // The vertex stage sampled this broad, world-anchored warp and its gradient
    // at the same lattice point. Its fifty yard scale is smooth over the liquid mesh,
    // so interpolating it here preserves the bent crests without another pixel-noise
    // evaluation (the fine chop below remains per-pixel). The fourth lane is the
    // grouping gain, which travelled here because this register had it spare.
    float warp=(waveWarp.x-.5)*6.5;
    float2 warpGradient=.05*float2(waveWarp.y*.8253356-waveWarp.z*.5646425,
                                   waveWarp.y*.5646425+waveWarp.z*.8253356);
    float2 swellPlane=plane+warp*float2(.6,.8);
    // How much of its swell this patch of water is carrying: the SET it sits in. The
    // vertex stage sampled that envelope and its gradient and passed both down, so the
    // grouping costs one multiply and one dot here, and the wave field a player sees
    // changes as it travels instead of repeating at one amplitude forever. The gain is in
    // the warp's spare lane and the gradient in worldRel.zw, for the register-count reason
    // spelled out on the vertex stage's Output struct.
    float gain=kGroupBase+kGroupDepth*waveWarp.w;
    float2 footprint=fwidth(plane);
    // The pixel's own size in lattice cells, squared. The chop fade below reads it as a
    // fade; the shore block further down reads the SAME number as the pixel's footprint in
    // world yards, which is what turns a per-pixel depth difference into a bed slope that
    // does not move with the resolution or the camera. Two derivatives, computed once.
    float footprintSquared=dot(footprint,footprint);
    // Fade the unresolved octave by the pixel's own footprint. The swell is never
    // faded: a forty yard wave is still resolvable at any distance a lake can be
    // seen, and fading it with the chop is what made distant water go flat.
    float filter=rsqrt(1+flowControl.x*footprintSquared);
    // The vertex stage has already evaluated A/B while displacing the mesh.
    // uv0.zw contains their sines and uv1.zw their cosines. Renormalize both
    // interpolated pairs back onto the unit circle together; this retains the
    // original analytic long-wave shape to sub-percent RMS on the client's
    // ~4-yard liquid grid, without repeating either sin/cos here.
    float2 trigInv=rsqrt(max(uv0.zw*uv0.zw+uv1.zw*uv1.zw,1e-4));
    float2 sin01=uv0.zw*trigInv,cos01=uv1.zw*trigInv;
    // Evaluate only the six shorter, shading-only trains here, in batches of
    // four and two. The sum remains the same eight-direction authored band.
    float4 t4=kRad4*(swellPlane.x*kDirX4+swellPlane.y*kDirY4+waveSwell+kStart4);
    float2 t2=kRad2*(swellPlane.x*kDirX2+swellPlane.y*kDirY2+waveSwell2.xy+kStart2);
    float4 sin4,cos4;
    float2 sin2,cos2;
    sincos(t4,sin4,cos4);
    sincos(t2,sin2,cos2);
    float swell=dot(kWgt01,sin01)+dot(kWgt4,sin4)+dot(kWgt2,sin2);
    // Reconstructed long-wave gradient plus the exact analytic short waves.
    float2 swellSlope=float2(dot(kWgt01*kRad01*cos01,kDirX01)+dot(kWgt4*kRad4*cos4,kDirX4)+dot(kWgt2*kRad2*cos2,kDirX2),
                             dot(kWgt01*kRad01*cos01,kDirY01)+dot(kWgt4*kRad4*cos4,kDirY4)+dot(kWgt2*kRad2*cos2,kDirY2));
    // Chain rule through p' = p + direction * warp(p), using the interpolated
    // warp gradient. This keeps the normal on the same gently bent crest the
    // vertex stage lifted and slid without another per-pixel noise evaluation.
    swellSlope=(swellSlope+dot(swellSlope,float2(.6,.8))*warpGradient*6.5)*foreverStyle.x;
    // The envelope then has to be DIFFERENTIATED, not just multiplied in: h = gain*swell,
    // so dh = gain*dswell + swell*dgain. The second term is what makes the flank of a set
    // tilt the water as well as dim it, which is the difference between waves that fade
    // out (a mask over one pattern) and waves that run out (a sea).
    swellSlope=swellSlope*gain+swell*worldRel.zw;
    swell*=gain;
    // One minus the Gerstner Jacobian: how hard the surface is pinching at this point.
    // It is the same number the vertex stage's slide was clamped by, so dividing the
    // shading slope by it sharpens normals exactly where the water is really steepening -
    // crests tighten and catch the light, troughs open out and go flat - and it is zero
    // on water that was authored flat, where there is nothing to sharpen. It carries the
    // envelope as well, because the tallest crest of a set is also the pinched one.
    float pinch=controls.x*gain*(dot(kWgt01*kRad01,sin01)+dot(kWgt4*kRad4,sin4)+dot(kWgt2*kRad2,sin2));
    float jacobian=max(1.0-pinch,.3);
    // WATER QUALITY: one uniform level, resolved on the CPU by src/Water/WaterQuality.h,
    // which owns the ladder this stage only compares against. Each gate below is that
    // header's `GateFor(level)` - the level minus a half - so a level can never sit on a
    // boundary and the two halves of the renderer cannot disagree about what a level means.
    // Every gate is a comparison against a constant register, so a whole tile takes the same
    // branch: what a level removes is work the GPU never does, not work hidden behind
    // per-pixel divergence. The octaves are the expensive part of the wave field - a noise
    // evaluation is a four-corner hash and a bicubic - and they are the first thing a level
    // gives up, because a sea read from a distance is a silhouette and a colour long before
    // it is a texture.
    float qualityLevel=waveFlow.z;
    // The chop rides the swell instead of scrolling over it: sampling the noise at a
    // coordinate the swell itself offsets is what ties the small ripples to the big
    // forms, so they travel together and the surface reads as one body of water.
    // Skipped below FLAT/LOW: with no octave there is no chop slope, and the analytic
    // swell alone still shades - the .5 value it is seeded with is the neutral one every
    // consumer of the sample already treats as "no chop here".
    float3 ripple=float3(.5,0,0);
    [branch] if(qualityLevel>=1.5)
        ripple=noiseGradient(plane+wavePhase.xy+swell*float2(.62,.33));
    // A second, finer chop octave, sampled at a little over twice the base frequency
    // on a phase that runs the other way around the lattice. One noise octave is a
    // texture; two incommensurate ones are weather - the fine octave is what stops
    // the short swell trains and the base chop from aligning into corduroy at the
    // distance where neither is quite resolvable. It fades with the SQUARE of the
    // footprint filter, so it is gone before it can alias, and it rides the swell
    // further than the base octave does, which keeps it glued to the same crests.
    float3 rippleFine=float3(.5,0,0);
    [branch] if(qualityLevel>=3.5)
        rippleFine=noiseGradient(plane*2.17+float2(wavePhase.y,-wavePhase.x)*1.31+swell*float2(.9,.47));
    // MID-WAVES: the band between the shortest analytic train (a 4.9 yard wavelength) and
    // the base chop (one 2.5 yard cell) is where a sea carries the texture a player reads
    // as "waves", and the analytic band alone leaves that band regular - eight sines
    // interfere, but each one is still a smooth rounded form, and smooth rounded forms at a
    // fixed scale are what the eye groups back into lines. A third noise octave at just
    // under half the base frequency (5.4 yard lumps), counter-phased against the base
    // octave the way the fine one is and riding the swell between the two, breaks that
    // regularity at exactly the scale that was missing. It fades with `filter` rather than
    // with its square: at half the base frequency it stays resolvable about twice as far as
    // the fine octave does, and fading it as early would remove it from the middle distance
    // where it is the only thing standing between the swell and corduroy. The weight is
    // authored against the octave's own coordinate, like the fine octave's .30 is, so the
    // two numbers mean the same thing.
    float3 rippleMid=float3(.5,0,0);
    [branch] if(qualityLevel>=6.5)
        rippleMid=noiseGradient(plane*.46+float2(-wavePhase.x,wavePhase.y)*.87+swell*float2(.40,.66));
    // waveFlow.w is the wave gate. With WAVES off - or WATER QUALITY at FLAT, which zeroes
    // it from the other direction - there is no slope, no refraction distortion, no
    // roughness and no foam, whatever the field would have said.
    // The chop is inside the same envelope as the swell it rides: a patch of sea that is
    // carrying no set has no wind texture either, which is what a calm spot looks like.
    float2 slopes=((ripple.yz*.72*filter+rippleFine.yz*.30*filter*filter
        +rippleMid.yz*.40*filter)*gain+swellSlope)*waveFlow.w;
    // The chop's sampled VALUE fades with the footprint too, not only its slope:
    // unresolved value noise aliases into a fixed screen-aligned grid, which is what
    // read as square patterns on distant water. The swell keeps its value, so crest
    // masks and the sun road stay alive at range.
    float2 detail=float2(lerp(.5,ripple.x,filter),swell*kSwellToUnit+.5);
    float2 offset=slopes*controls.y;
    float3 surface=tex2D(surfaceTexture,uv1.xy+offset+waveFlow.xy).rgb;
    float3 rgb=surface+color.rgb*base.rgb;
    float3 v=safeNormalize(-viewPos);
    float3 n=safeNormalize(viewNormal);
    // Water can be drawn from either side. Orient the normal and mirror the wave
    // tilt with it; the tilt comes from the real world-space gradient of the field,
    // rotated into view space by the vertex shader. It is the NEGATIVE gradient: for a
    // height field the normal is (-dh/dx,-dh/dy,1), and adding the gradient instead -
    // which is what this line used to do - lit every wave face as though it leaned the
    // other way, so highlights sat in the troughs. Invisible while the surface was
    // flat, obvious the moment the geometry started lifting crests.
    float cameraSide=dot(n,v);
    // This draw is being viewed from the water side when the un-oriented normal points
    // away from the camera. Keep that fact after flipping the normal: flipping alone makes
    // the lighting two-sided, but it does not give an underwater view its different optical
    // path or the water-to-air critical angle.
    float underwater=1-step(-1e-4,cameraSide);
    float flip=cameraSide<0?-1:1;
    n*=flip;
    float2 tilt=-slopes*(controls.z/jacobian)*flip;
    float3 wave=safeNormalize(n+waveTangent*tilt.x+waveBitangent*tilt.y);
    float3 l=safeNormalize(-lightDirection.xyz);
    float NoL=dot(wave,l);
    float sunElev=dot(l,upWind.xyz);
    // A wave has to change the BODY of the water, not only its highlight. What makes
    // a swell read as a rounded form is a lit back and a shaded face, and the
    // hemisphere it sees: both are differences against the geometric normal, so flat
    // water and an authored zero-strength sea stay exactly as the client drew them.
    // The multiplier has a FLOOR, and the floor is the fix for a black shape that used to sit inside
    // the waves when the camera was down at the surface. `controls.z` is normalStrength times the
    // sea's own agitation and reaches 2.5, so on a face turning away from the source this expression
    // went NEGATIVE - and nothing saturates `rgb` between here and the output, so the render target
    // clamped it to pure black. The water is alpha-blended and writes no depth, so the far face of a
    // wave shows through the near one, and a far face clamped to black read as a silhouette cut out
    // of the sea rather than as water: "the blacked shape of a wave visible through the water".
    //
    // A shaded face is darker, not black, so the floor keeps the whole authored brightening range and
    // most of the shading, and it rises with dot(-v,l) - the light coming through the sheet toward
    // the viewer, which is the same cue the in-scatter lobe and the foam veil glow use below, so all
    // three agree about which side of a wave the sun is on. A thin sheet with the light behind it is
    // among the brightest things in a sea and never the darkest, which is what the .78 end says.
    float vDotL=dot(v,l);
    rgb*=max(1+controls.z*(1.9*(NoL-dot(n,l))+.75*(dot(wave,upWind.xyz)-dot(n,upWind.xyz))),
             lerp(.22,.78,saturate(-vDotL*1.7-.45)));
    // The captured pre-water colour and depth allow shallow refraction and a
    // depth transition without changing the original map or liquid data.
    float captureValid=reflectionStyle.w;
    float depthScale=1/max(viewPos.z,.05);
    float2 screenUV=float2(.5+.5*viewPos.x*reflectionProjection.z*depthScale,
                           .5-.5*viewPos.y*reflectionProjection.w*depthScale);
    float behindRaw=tex2Dlod(sceneDepth,float4(screenUV,0,0)).r;
    float behindZ=behindRaw>=.9999?100000:reflectionProjection.y/(behindRaw-reflectionProjection.x);
    // Foreground geometry is not a deep seabed. Never turn negative
    // separation into absorption/foam on the wrong side of a shoreline.
    float waterDepth=max(behindZ-viewPos.z,0);
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
    float column=min(effectiveDepth,400.0);
    // SHORE UNDERSTANDING: the captured depth buffer is the only geometry this pass can
    // read, so its slope is the only description of a shore available to it - and a slope
    // measured PER PIXEL is not a slope. It halves when the resolution doubles and halves
    // again when the camera steps back, which is why the shoal foam below used to exist
    // only within a few yards of the water's edge and disappear on a bar seen from the
    // beach above it. The lattice footprint this stage already took for the chop fade is
    // the pixel's own size in world yards - one lattice cell is `cellYards`, and the lattice
    // rows are unit world directions scaled by 1/cell - so dividing by it turns a
    // screen-space difference into the dimensionless bed slope it was always meant to be:
    // the same number at any resolution and any distance, and the same number the CPU mirror
    // measures in world space, which is the reading the look was tuned against. It carries
    // the view angle's 1/cos, because the separation this pass can read is along the view
    // axis rather than vertical; that is a property of the camera and not of the resolution,
    // and the authored gain absorbs it.
    // The divisor has a floor of kBedSlopeSpanYards, and the floor is the whole point of it:
    // `ddx(depth)` is a difference over ONE pixel, so dividing by a pixel footprint that is a
    // fifth of a yard - which is what a shoreline looks like from the water's edge - measures
    // the depth buffer's own quantization and dither, not the bed, and reports it as an
    // enormous slope. That number then drives the shoal foam, the beach/wall split and the
    // swash band width, so near the camera it read as a checkerboard of foam on the bottom that
    // crawled with the view. One and a half yards is the span the CPU mirror has always
    // measured its bed slope over (FieldSlope samples +-1.5 yd), so this floor also puts the
    // two halves of the renderer on the same baseline: the shader reads a coarser, calmer slope
    // up close and the same slope everywhere else.
    float bedSlope=length(float2(ddx(effectiveDepth),ddy(effectiveDepth)))
        /max(max(sqrt(footprintSquared),kBedSlopeSpanYards)
            *rsqrt(max(dot(waveLattice.xy,waveLattice.xy),1e-8)),1e-3);
    // A gentle beach and a rock wall both read as "the bottom comes up fast", and they are
    // not the same water: a beach carries a wide swash zone with sheets running up it and
    // draining back out, a wall carries a narrow bright contact line and nothing that can
    // run up it. One number separates them - about twelve degrees of bed is a beach, about
    // forty is a wall - and it is the only shore shape this pass can know.
    float beach=1-saturate(bedSlope*1.6-.32);
    // Shallow water barely bends the world; the deeper the column, the further the
    // refracted sample is pulled. This is what keeps a bar and a shoreline crisp
    // while open water shimmers.
    // SOFT CONTACT, at WATER QUALITY 10: the last inches of a waterline are a meniscus, not
    // a step. Pulling the refracted sample by the full wave slope right at the contact line
    // is what leaves a hard bright seam along every shore and every rock standing in it;
    // fading the offset over the first yard lets the bottom come up through undisturbed
    // water instead. One lerp on a uniform gate, so a level below it is untouched.
    float contact=lerp(1,saturate(effectiveDepth*.9),step(9.5,qualityLevel));
    float bend=waterStyle.x*saturate(column*.05+.25)*contact;
    float2 refractUV=screenUV+(depthValid?slopes*flowControl.yz*bend:float2(0,0));
    // Reject distortion across foreground silhouettes, rather than pulling
    // a bridge/character into the water. Missing depth keeps the native body.
    float refractRaw=tex2Dlod(sceneDepth,float4(refractUV,0,0)).r;
    refractUV=lerp(screenUV,refractUV,step(reflectionProjection.x+reflectionProjection.y*depthScale,refractRaw));
    float3 refracted=tex2Dlod(sceneTexture,float4(refractUV,0,0)).rgb;
    // Beer-Lambert per channel, with the authored deep colour as the asymptote:
    // "clear mountain lake" and "murky swamp" are then one number apart (Clarity)
    // instead of a re-tune of every term. Red dies first, which is why a deep pool
    // reads teal rather than as a darker grey.
    float3 transmittance=exp(-bodyStyle.x*waterStyle.y*column*absorbTint.rgb);
    float absorption=captureValid*(1-transmittance.g);
    // Light scattering back out of the column is what makes a crest glow when the
    // source is behind it. Without it water is a surface pasted onto the world;
    // a Henyey-Greenstein lobe forwards that glow into the sun, which is where the
    // reference shots put it.
    float g=.3+.34*saturate(sunElev);
    // One dot serves the Henyey-Greenstein denominator, the backlit gate below, and the body
    // shading's transmission floor above: dot(-v,l) is -dot(v,l) bit-exactly, so nothing is
    // re-derived. It is computed up there because that is the first of the three that needs it.
    float phaseDen=max(1+g*g+2*g*vDotL,1e-3);
    // The lobe is divided by 4*pi as well as normalized in exponent: a (1-g^2)/den^2 phase
    // function peaks at 18x on a face turned toward the sun, and the CPU preview showed what
    // that did to a whole sea - one flat pale wash. The 3/2 exponent is the HG function's
    // own, and .17 (1.2/4pi, folded) puts the forward peak near 1.3 and the side lobe near .08.
    float3 lightPos=max(lightColor.rgb,0);
    float3 inScatter=(deepTint.rgb*1.7+waterTint.rgb*.12)*absorption*(1-g*g)*.17/(phaseDen*sqrt(phaseDen))
        *saturate(sunElev*2.5+.2)*lightPos;
    // A crest thin enough to see through glows when the source is behind it. The lobe
    // above scatters in every direction; this is the half of it that needs a swell to
    // exist at all - looking into the light at the top of a wave - and it is the single
    // strongest cue that water is a volume rather than a surface with a picture on it.
    // Both gates matter: facing away from the light, or in a trough, there is no glow.
    float backlit=saturate(-vDotL*1.7-.45)*saturate(detail.y*1.7-.45);
    inScatter*=1+2.2*backlit;
    // BACK FLOW: the sheet that runs up a beach and drains back out again. The driver is the
    // longest swell train this pixel is standing in - the same analytic train A the vertex
    // stage lifted the mesh with, arriving through the same interpolated sine/cosine pair -
    // so the run-up comes in with the crest a player can already see coming, and the drain
    // follows it back out at the speed the sea is really travelling. No second clock and no
    // new phase: sin01.x is that crest's height here and cos01.x its rate of change, which is
    // positive while the water is rising and negative while it is leaving.
    //
    // Depth is the shoreline's own coordinate - "up the beach" is "less water over the
    // bottom" - so the whole effect is a shift of the depth the shore masks are built from,
    // and it needs no shore normal this pass has no way to read. It moves the shore and foam
    // edges ONLY: the absorption column, the refraction and the in-scattering above still
    // measure the water that is really there, because a sheet running up a beach is thin
    // water, and thin water is exactly what those terms describe.
    float runUp=waveSwell2.z*beach;   // yards of waterline travel, 0 with the feature off
    float draining=step(.0001,waveSwell2.z)*saturate(-cos01.x*1.8);
    // A beach spreads its shore band wider than a wall does, because the sheet that runs up
    // one keeps running: the same bed slope that says "beach" says how far up it goes.
    float swashWidth=1+beach*1.4*step(3.5,qualityLevel);
    float swashDepth=max(effectiveDepth-sin01.x*runUp,0);
    float shallowEdge=1-smoothstep(.08,.08+max(waterStyle.w,.1)*swashWidth,swashDepth);
    float shore=depthValid*shallowEdge;
    // Foam lives on the crest of a wave and in the chop that breaks it, so it is gated on
    // the swell's own height rather than on a slope projected down one arbitrary direction.
    float crest=saturate(.42+detail.y*.6+detail.x*.3);
    float rough=saturate(bodyStyle.z+length(slopes)*bodyStyle.y);
    // WHITECAPS: foam on the EDGES of the waves. Three things have to agree before a wave
    // breaks - it has to be at its peak, the Gerstner surface has to be pinching there
    // (the same number that sharpened the normal above, so foam appears exactly where the
    // geometry is steepening), and the chop has to be there to tear the line into streaks
    // instead of painting a stripe along every wave in the sea. `pinch` is zero on water
    // that was authored flat, so a calm zone shows no white line at all however high its
    // wind is, and a storm shows one on every third crest.
    float wind=upWind.w;
    float foamWind=saturate(wind);
    float breaking=saturate(pinch*2.4);
    float capBand=detail.y*1.06+breaking*.30+(detail.x-.5)*.26;
    // Broad warp noise gives foam its tongue-scale shape; the already drifting
    // chop sample tears the edges into smaller flecks. The moving threshold
    // breaks the repeated white stripe without another texture sample.
    float foamPattern=saturate(.45*waveWarp.x+.55*detail.x);
    float foamThreshold=.82+(1-foamPattern)*.14;
    float whitecap=smoothstep(foamThreshold,foamThreshold+.14,capBand);
    // Foam where the bottom comes up fast is the one cue available for a river hitting a
    // rock: the depth buffer is the only geometry this pass can read. It reads the bed slope
    // above rather than a per-pixel depth difference, so it is the same foam at any
    // resolution and at any distance instead of a term that only existed close to the camera;
    // it needs a real floor to be a shoal at all, and it stops twenty yards down, because a
    // cliff face under deep water is not water breaking over a bar. Both gates are what the
    // CPU mirror has always measured - it reads the world-space bed slope and gates it the
    // same way, and the published frames were tuned against that reading.
    float shoal=depthValid*saturate(bedSlope*bodyStyle.w*2.0-.3)*saturate(1-column*.05);
    float crestMask=smoothstep(.60,.94,detail.y+length(ripple.yz)*.05*filter)
        *(.35+.65*foamPattern);
    // FOAM LACE, at WATER QUALITY 7: a mask decides WHERE foam is and says nothing about
    // what it looks like, and one scalar of coverage can only ever be a grey sheet - which is
    // what "the foam is nice but flat" was. Real foam is a lace: bright filaments with dark,
    // water-filled gaps between them, at three scales at once. All three are already sampled
    // for the surface itself, so this costs arithmetic and no noise evaluation - the fine
    // chop is the fleck, the mid octave is the lump a fleck sits on, and the broad warp is
    // the tongue both are torn out of. It multiplies the crest terms whole, because a whitecap
    // IS a broken streak, and only .45 of the shore term, because a sheet running up a beach
    // is continuous in a way a cap on a crest is not.
    // ...and the lace reads those octaves' VALUES, which is exactly the thing the `detail`
    // fade above exists for: value noise a pixel cannot resolve aliases into a fixed
    // screen-aligned grid, and on foam - where the term multiplies a bright colour - that grid
    // reads as a checkerboard crawling with the camera instead of as texture. So each octave is
    // faded by the same factor its own slope uses (the fine one by the square, the mid one by
    // the first power), which pulls both back to the neutral .5 where they are unresolved, and
    // the lace itself fades toward 1 - no lace - on the same filter, because unresolved foam is
    // flat coverage and that is a truth rather than a loss.
    //
    // THE SHAPE OF THE MIX MATTERS MORE THAN THE FADE, and the first version of this got it wrong
    // in a way no fade could cover. It read `saturate(v*1.7-.35)` and `saturate(v*1.6-.30)`, and a
    // saturate whose ramp sits INSIDE the noise's own value range clips: measured over the lattice,
    // 21.9% of cells landed flat on the low plateau and 19.0% on the high one for the fleck, 20.2%
    // and 17.4% for the lump, and 43.1% of the finished lace sat exactly on 1.0. Value noise is one
    // blob per lattice cell, so a clipped blob is a flat SQUARE one cell across - 1.15 yards for the
    // fine octave, 5.4 for the mid - and 43% of the foam was paved with them. That was the
    // checkerboard: not aliasing at distance but the lattice itself, printed onto the foam by a
    // saturate, at every distance and every quality level above 6, which is exactly why fading it by
    // the pixel footprint left it on the screen.
    //
    // A product of the two octaves' COMPLEMENTS has no plateau anywhere inside the range: it reaches
    // 1 only where an octave reaches its own maximum, which for a continuous field is a set of
    // measure zero, and it is darkest only where both agree the water is open - which is what a
    // water-filled gap between two filaments actually is. Measured the same way it keeps the look:
    // mean 0.817 against 0.805, minimum 0.307 against 0.300, and 0.0% of cells on a plateau instead
    // of 43.1%.
    float lace=1;
    [branch] if(qualityLevel>=6.5) {
        float fine=lerp(.5,rippleFine.x,filter*filter);
        // The mid octave is deliberately NOT a foam input. It is the level-7 value that
        // brought the visible lattice back even after the clipped saturate was removed;
        // its screen projection is exactly the grid reported at quality 7 and above.
        // The broad warp is interpolated from vertices and is continuous across the liquid
        // mesh, so use it as the second lace scale instead of per-pixel mid value noise.
        float broad=lerp(.5,waveWarp.x,filter);
        lace=lerp(1,1-.70*(1-fine)*(1-broad),saturate(filter*2-.2));
    }
    // BACKWASH, at the same level: foam left on a beach lies ALONG the shoreline, in streaks
    // the next sheet pushes up and the drain pulls back out. Contours of the captured depth
    // are the shoreline's own coordinate, so a band in depth is a band along the shore, and
    // moving that band with the arriving swell is the retreat the eye reads as back flow -
    // the water going back out, taking the foam with it. The chop value tears the bands into
    // flecks so they can never read as rings painted on the water, `shallowEdge` keeps them
    // inside the swash zone they belong to, and `draining` is what makes them a backwash
    // rather than a run-up: the streaks are only there while the sheet is leaving.
    float backwash=0;
    [branch] if(qualityLevel>=6.5)
        backwash=saturate(.55+.45*sin(swashDepth*2.4-sin01.x*2.1))*(.30+.70*foamPattern)
            *saturate(shallowEdge*2.2)*draining;
    float foamAmount=saturate((shore*waterStyle.z*(.35+.65*foamWind)*lerp(1,lace,.45)*(1+.45*draining)
        +(crestMask*.45+whitecap*1.15+shoal*.6)*foamWind*foreverStyle.w*lace
        +backwash*waterStyle.z*lace)*crest);
    // Foam is not a decal on the water, it is a different surface: it does not mirror the
    // sky and it does not carry the sun road. Adding it to the roughness here spreads the
    // reflection toward the sky term and widens the glint lobe, and the two multipliers
    // below take the rest of the mirror away, all before either is computed.
    rough=saturate(rough+foamAmount*1.4);
    // What the surface reflects at grazing angles is the sky, not a flat fog
    // colour: horizon first, then a darker zenith, with the sun or moon disc in it -
    // the one clearly visible tell of the new renderer on open water. A wave face
    // tilted far enough to look down leaves rayUp at zero, so it reflects the bright
    // horizon band instead of the seabed, which is also what the reference does.
    float3 ray=safeNormalize(reflect(-v,wave));
    float rayUp=saturate(dot(ray,upWind.xyz));
    float3 sky=lerp(fogColor.rgb,fogColor.rgb*.5+float3(.015,.045,.11)*saturate(sunElev*2+.25),
                    rayUp*rayUp);
    sky+=lightPos*pow(saturate(dot(ray,l)),160)*waterTint.a;
    // Real Schlick with water's index of refraction (F0 = 0.02). This is the term
    // that decides whether water reads as glass or as a mirror, and the hand-tuned
    // curve it replaces was the one thing that could not be authored per body. It is
    // evaluated BEFORE the march below because the march's whole result is multiplied
    // by it on the way to the screen, and that product is the only honest answer to
    // "is this pixel worth twelve depth reads".
    float3 H=safeNormalize(v+l);
    float NoV=max(dot(wave,v),1e-4);
    float foreverFresnel=saturate(1-NoV);
    float fresnel=(.02+.98*pow(foreverFresnel,5.0))*foreverStyle.z;
    // Water-to-air rays from below undergo total internal reflection above the critical
    // angle (cos(theta_c)=sqrt(1-(1/1.333)^2) ~= .661). A Schlick curve alone tops out
    // below one, so it incorrectly kept showing the sky/terrain through a grazing underside.
    // Smooth only across a narrow angular band to avoid a visible branch boundary.
    float totalInternal=1-smoothstep(.63,.69,NoV);
    fresnel=max(fresnel,underwater*totalInternal*foreverStyle.z);
    // 2. Screen-space reflections against the pre-water copy, perturbed by the
    // wave normal. WATER QUALITY hands this loop its trip count - 0 skips it through
    // the same test that already stopped it, 12 is the ceiling the loop is written
    // against, 8 is what this shader always ran. Fewer steps is a coarser hit, and a
    // coarse hit on a surface this rough is indistinguishable from a sharp one once
    // the roughness mix below has spread it back toward the sky.
    float3 environment=sky;
    float3 reflected=environment;
    float hit=0;
    // The wave offset is the same on every step of the march: evaluate it
    // once instead of eight times.
    float3 waveSample=wave*.15;
    int ssrSteps=(int)reflectionControl.y;
    // The budget test is the march's OWN output expression rather than a guess about it:
    // whatever the loop finds is multiplied by fresnel before it can reach the screen, so at
    // near-normal incidence - looking straight down into water, which is most of the covered
    // pixels in a harbour - the whole march could only ever have contributed under half a
    // percent of the reflection mix. Bounded by the same two constants that mix uses, so the
    // skip is a statement about this shader's arithmetic and not a tuning judgement.
    float reflectionBudget=fresnel*(reflectionStyle.x*.5+reflectionControl.x*1.5);
    if(reflectionStyle.w>.5 && reflectionControl.x>.001 && ssrSteps>0 && reflectionBudget>.004) {
        float3 hitColor=environment;
        float invSteps=1.0/float(ssrSteps);
        [loop] for(int stepIndex=0;stepIndex<12 && stepIndex<ssrSteps && hit<1.0;++stepIndex) {
            float q=float(stepIndex+1)*invSteps;
            float traceDistance=.45+q*q*reflectionStyle.y;
            float3 samplePosition=viewPos+waveSample+ray*traceDistance;
            // Pre-compute reciprocal Z once: the two UV components share the
            // same max(z,.05) divisor, so one rcp replaces two per step.
            float invSampleZ=1.0/max(samplePosition.z,.05);
            float2 sampleUV=float2(.5+.5*samplePosition.x*reflectionProjection.z*invSampleZ,
                                   .5-.5*samplePosition.y*reflectionProjection.w*invSampleZ);
            float inside=step(.05,samplePosition.z)*saturate(min(min(sampleUV.x,sampleUV.y),
                                      min(1-sampleUV.x,1-sampleUV.y))*8);
            float raw=tex2Dlod(sceneDepth,float4(sampleUV,0,0)).r;
            float sceneZ=raw>=.9999?100000:reflectionProjection.y/(raw-reflectionProjection.x);
            float difference=samplePosition.z-sceneZ;
            float thickness=reflectionStyle.z+traceDistance*.05;
            // A ray leaving the surface can only hit something in front of it, so
            // one narrow band replaces the confidence-maximisation this used to run per
            // step: the first sample inside the band wins and later steps cannot
            // overwrite it, which is the right answer for a surface this smooth.
            float confidence=inside*step(-thickness,difference)*step(difference,thickness*.5);
            // A miss must not fetch full-resolution scene colour. The previous
            // arithmetic sampled it on all eight steps even though a zero lerp
            // discarded seven or eight of those reads. Branching only around
            // the colour fetch is output-identical, and a full first hit makes
            // every later contribution exactly zero, so stop the march there.
            [branch] if(confidence>0) {
                hitColor=lerp(hitColor,tex2Dlod(sceneTexture,float4(sampleUV,0,0)).rgb,
                              confidence*(1-hit));
                hit=max(hit,confidence);
            }
        }
        reflected=lerp(environment,hitColor,saturate(hit*1.8));
    }
    // Rough water does not lose reflection energy, it spreads it out. Mixing the
    // sharp screen-space hit back toward the smooth sky by the wave's own roughness
    // is the cheap stand-in for the blurred mip chain this pass cannot sample.
    reflected=lerp(reflected,environment,saturate(rough*1.6));
    // One GGX lobe, widened by the sun's own angular disc. That widening is what
    // stretches a low sun into the long shimmering band in the reference while a
    // high sun keeps a short tight one - and it costs nothing per step. This shader
    // used to evaluate three overlapping lobes plus a halo, which washed water out
    // and exceeded PS3 limits; one lobe plus a breakup mask is all it needed.
    float NoH=saturate(dot(wave,H));
    // The exponent IS the roughness and the sun's height: a low source smears the
    // road wide, a high one keeps it short and tight, and chop broadens it further.
    // That replaces a full Smith visibility chain, which cost eight slots to describe
    // a falloff the lobe width already produces.
    float sharpness=lerp(26.0,230.0,saturate(sunElev*1.8))/(1.0+rough*7.0)*absorbTint.w;
    // (sharpness+2) is the lobe's own energy normalization: pow(NoH,n) integrates to
    // 2*pi/(n+2), so without it a low sun smears a dim band and a high sun - where n is
    // large - faded to nothing. The CPU preview showed a whole sea with no sun road at all.
    float celestialPath=pow(NoH,sharpness)*(sharpness+2.0)*fresnel*saturate(NoL)
        *(.12+.88*saturate(detail.x*.75+detail.y*.25))*foreverStyle.y;
    // March from this water point toward the sun/moon through the captured
    // pre-water depth buffer. Buildings and terrain suppress direct glint,
    // while ambient sky reflection remains visible under cover.
    // WATER QUALITY hands this march its trip count the same way it hands the reflection
    // march one: 0 skips it and leaves the glint unoccluded (which is what the level is for
    // - a sun road that ignores buildings costs nothing to draw), 12 answers the question at
    // every step the reflection march is allowed to take.
    float lightVisibility=1.0;
    float3 waveLight=wave*.18;
    int lightSteps=(int)reflectionControl.z;
    if(reflectionStyle.w>.5 && celestialPath>.00005 && lightSteps>0) {
        float invLightSteps=1.0/float(lightSteps);
        [loop] for(int lightStep=0;lightStep<12 && lightStep<lightSteps && lightVisibility>0;++lightStep) {
            float q=float(lightStep+1)*invLightSteps;
            float traceDistance=1.5+q*q*110.0;
            float3 lightSample=viewPos+waveLight+l*traceDistance;
            // Pre-compute reciprocal Z once: the two UV components share the
            // same max(z,.05) divisor, so one rcp replaces two per step.
            float invLightZ=1.0/max(lightSample.z,.05);
            float2 lightUV=float2(.5+.5*lightSample.x*reflectionProjection.z*invLightZ,
                                  .5-.5*lightSample.y*reflectionProjection.w*invLightZ);
            float inside=step(.05,lightSample.z)*saturate(min(min(lightUV.x,lightUV.y),
                                    min(1-lightUV.x,1-lightUV.y))*8);
            float lightRaw=tex2Dlod(sceneDepth,float4(lightUV,0,0)).r;
            float blockerZ=lightRaw>=.9999?100000:reflectionProjection.y/(lightRaw-reflectionProjection.x);
            // Anything nearer than the sample point occludes the source: a single
            // sided band where the old version tested a thickness window on top of
            // an in-range test, which cost three more slots to say the same thing.
            float blocked=inside*step(blockerZ+1.5,lightSample.z);
            lightVisibility*=1.0-blocked;
            // Once an interior sample is blocked the product is exactly zero;
            // the loop condition skips remaining depth reads. Edge-faded samples
            // remain fractional and continue exactly as before.
        }
    }
    celestialPath*=lightVisibility;
    float3 sheenColor=lightPos;
    // 1. Water body: refracted bottom, absorbed and scattered through the column.
    float3 waterBody=rgb;
    // Gated on the capture, exactly as before: without a depth/colour copy there is
    // no column to absorb through, and inventing one would break the promise that an
    // unmatched or uncaptured water pixel keeps the client's own colour.
    if(captureValid>.5) {
        // Refraction strength scales the BLEND, never the colour: with refraction off
        // this has to fall back to the client's own pixels, not to a black bottom.
        float3 bottom=refracted*transmittance+inScatter+deepTint.rgb*absorption;
        float3 body=lerp(rgb,bottom,depthValid*saturate(waterStyle.x));
        // And the depth transition stays independent of it, so a lake with refraction
        // switched off still gets dark the further the bottom is away.
        waterBody=lerp(body,deepTint.rgb+inScatter,saturate(absorption*(.85-.22*fresnel)));
    }
    // Per-body colour authoring multiplies rather than replaces, so the zone's own
    // painted water survives tinted instead of being overwritten: 0x808080 neutral.
    // The CPU uploads the bytes divided by 128, so the register already carries 1.0 at
    // neutral and there is no gain to apply here. Doubling it (as an earlier revision of
    // this line did) puts every authored colour a stop high and clips the bright ones:
    // `tools/WaterDeviceRegression.cpp` renders a captured 128 gray through the whole
    // shader and requires 128 back, which is where that doubling showed up as 255.
    waterBody*=waterTint.rgb;
    rgb=waterBody;
    // Looking up through the interface is still looking through a water column: the camera
    // is on the water side, so the path to this surface must attenuate the view even when
    // the captured depth belongs to terrain a long way above or the depth capture is absent.
    // Use the actual camera-to-fragment ray length, not view-Z (which shortens the column at
    // grazing angles), and the same per-channel Beer-Lambert coefficient as the bottom path.
    // With the camera above water this is an exact identity; underwater it converges toward
    // the authored deep colour as the surface gets farther away.
    float cameraColumn=min(length(viewPos),250.0);
    float3 cameraTransmittance=exp(-bodyStyle.x*waterStyle.y*cameraColumn*absorbTint.rgb);
    float3 underwaterBody=rgb*cameraTransmittance+deepTint.rgb*(1-cameraTransmittance);
    rgb=lerp(rgb,underwaterBody,underwater);
    // Direct reflection of actual local emitters on the same wave normal.
    // This does not depend on SSR finding an opaque flame in scene depth.
    float3 localReflection=0;
    [loop]for(int li=0;li<8;++li){
        if(li>=(int)localControl.x)break;
        float3 delta=localPosition[li].xyz-viewPos;
        float radius=max(localPosition[li].w,.01);
        float distanceSquared=dot(delta,delta);
        // Selected lights are camera-local, not water-tile-local. In a dense
        // harbour most of them have zero reach for most water pixels. The old
        // loop still normalized a vector and evaluated pow(,64) before
        // multiplying by that exact zero; skip only that mathematically dead work.
        [branch] if(distanceSquared>=radius*radius) continue;
        float distance=sqrt(distanceSquared);float3 toLight=delta/max(distance,.001);
        float reach=saturate(1-distance/radius);
        float specular=pow(saturate(dot(wave,safeNormalize(toLight+v))),64);
        float fresnelLocal=.04+.96*foreverFresnel*foreverFresnel;
        localReflection+=localColor[li].rgb*localColor[li].w*specular*fresnelLocal*
            saturate(dot(wave,toLight))*reach*reach/(1+distance*distance*.035);
    }
    rgb+=localReflection*localControl.y;
    // Foam takes the mirror with it: a whitecap is a matte, air-filled surface, so it
    // keeps a fraction of the sky sheen and none of the sharp screen-space hit.
    float reflectionAmount=saturate(fresnel*(1.0-.5*rough)*(reflectionStyle.x*.5+reflectionControl.x*hit*1.5))
        *(1-saturate(foamAmount*.85));
    rgb=lerp(rgb,reflected,reflectionAmount);
    // 4. Celestial reflection path, then foam. Wind decides how much of any of it
    // exists: an authored calm river shows no white line at all, an authored sea
    // whips up on the crest edges and where the flow shallows over a bar. The masks
    // themselves were built with the wave field above, before anything they had to
    // suppress was computed.
    float pathEnergy=saturate(celestialPath*controls.w*.0992)*(1-saturate(foamAmount));   // .62 * (1/2pi), folded
    rgb+=sheenColor*pathEnergy*(.35+.65*fresnel);
    // Lit with a wrapped diffuse, so foam on the shaded side of a crest is still visible
    // against the water it sits on, and scaled by the authored foam brightness.
    // At WATER QUALITY 7 the colour is a ramp on the coverage the masks already produced
    // instead of one flat white: a thick cap scatters everything back and reads as the bright
    // end, a thin veil over a crest lets the water through and reads darker and greener, which
    // is what the lace above is a picture of. Below that level the colour is the shipped one
    // exactly, so the ramp cannot move the look of a machine that did not ask for it.
    float3 foamColour=deepTint.rgb*.3+float3(.42,.48,.43);
    [branch] if(qualityLevel>=6.5)
        foamColour=lerp(deepTint.rgb*.55+float3(.28,.34,.32),foamColour,saturate(foamAmount*1.35));
    rgb+=foamColour*foamAmount*(.55+.45*saturate(NoL+.25))*deepTint.a;
    // And at WATER QUALITY 10 the veil glows when the source is behind it: foam is air and
    // water, and a thin one lit from behind is translucent in exactly the way the crest above
    // is, which is the same cue the in-scattering lobe gives the body of the water. It uses
    // the one dot the lobe already computed, so it costs a gate and a multiply.
    [branch] if(qualityLevel>=9.5)
        rgb+=foamColour*(1-saturate(foamAmount*1.35))*foamAmount*saturate(-vDotL*1.4-.35)
            *saturate(sunElev*2.+.2)*.55*deepTint.a;
    if(flowControl.w>1.5){result_.color=float4(saturate(pathEnergy*3).xxx,1);return result_;}
    if(flowControl.w>.5){result_.color=float4(1,0,1,1);return result_;}
    float baseAlpha=color.a*base.a;
    float depthAlpha=absorption*(.78-.20*fresnel);
    float surfaceAlpha=saturate(baseAlpha+(1-baseAlpha)*depthAlpha);
    // `rgb` already contains the water/reflection mix. If alpha stays at the native
    // translucent base while reflectionAmount is high, the fixed-function blend mixes
    // the captured scene a second time and exposes the opposite liquid face through this
    // one - the underwater silhouette/seam seen while crossing the interface. Keep the
    // output coverage at least as strong as the reflection we already placed in rgb.
    surfaceAlpha=max(surfaceAlpha,reflectionAmount);
    result_.color=float4(fog*(rgb-fogColor.rgb)+fogColor.rgb,surfaceAlpha);
    result_.mask.a=result_.color.a;
    return result_;
}

)HLSL";
// 0xRRGGBB or decimal, per-area scoped like every other water key. Colours are the
// only values that need text rather than an integer, so they parse here rather than
// growing a colour type in the shared tuning reader.
unsigned ReadTuningColour(const wchar_t* key,unsigned fallback){
    wchar_t value[64]{};
    renderer::locationtuning::ReadString(L"Water",key,L"",value,std::size(value),tuningIni.c_str());
    if(!value[0]){std::wstring cached; if(renderer::locationtuning::ReadCachedString(mainIni.c_str(),L"Water",key,cached)&&!cached.empty()){size_t n=std::min(cached.size(),std::size(value)-1);for(size_t i=0;i<n;++i)value[i]=cached[i];value[n]=L'\0';}}
    if(!value[0])return fallback;
    const auto parsed=wcstol(value,nullptr,value[0]==L'0'&&(value[1]==L'x'||value[1]==L'X')?16:10);
    return parsed<0?fallback:unsigned(parsed)&0xffffffu;
}
int ReadTuning(const wchar_t* key,int fallback){wchar_t value[64]{};renderer::locationtuning::ReadString(L"Water",key,L"",value,std::size(value),tuningIni.c_str());return value[0]?int(wcstol(value,nullptr,10)):renderer::locationtuning::ReadInt(L"Water",key,fallback,mainIni.c_str());}
void ReloadTuning(){
    localLightConstantCache.valid=false;
    // Match the menu's source and default exactly. An obsolete Enabled=0 in
    // ModernWoWRenderer.ini must not veto a live (including per-location) ON.
    effectEnabled=renderer::locationtuning::ReadInt(L"Water",L"Enabled",1,tuningIni.c_str())!=0;
    wavesEnabled=ReadTuning(L"WavesEnabled",1)!=0;geometryWavesEnabled=ReadTuning(L"GeometryWavesEnabled",1)!=0;
    reflectionsEnabled=ReadTuning(L"ReflectionsEnabled",1)!=0;refractionEnabled=ReadTuning(L"RefractionEnabled",1)!=0;
    depthEnabled=ReadTuning(L"DepthEnabled",1)!=0;foamEnabled=ReadTuning(L"FoamEnabled",1)!=0;sunGlintEnabled=ReadTuning(L"SunGlintEnabled",1)!=0;
    // WATER QUALITY owns how much work the surface is allowed to take; every other number in
    // this block owns how it looks. It is read before them because both the capture decision
    // above and the constants this file uploads come out of the plan it resolves to, and the
    // ladder itself lives in one place - src/Water/WaterQuality.h - so a level means the same
    // thing here, in the two shader stages and in the F7 menu.
    waterQuality=std::clamp(ReadTuning(L"Quality",renderer::water::kQualityDefault),
        renderer::water::kQualityMin,renderer::water::kQualityMax);
    qualityPlan=renderer::water::QualityPlanFor(waterQuality);
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
    geometryWavePercent=float(std::clamp(ReadTuning(L"GeometryWavePercent",100),0,300));
    waveScalePercent=std::clamp(ReadTuning(L"WaveScalePercent",100),25,400)*1.f;
    clarityPercent=float(std::clamp(ReadTuning(L"ClarityPercent",100),5,200));
    windStrengthPercent=float(std::clamp(ReadTuning(L"WindStrengthPercent",100),0,200));
    roughnessPercent=float(std::clamp(ReadTuning(L"WaveRoughnessPercent",100),0,200));
    sheenPercent=float(std::clamp(ReadTuning(L"SheenPercent",50),0,100));
    shoalFoamPercent=float(std::clamp(ReadTuning(L"ShoalFoamPercent",100),0,300));
    sunDiscPercent=float(std::clamp(ReadTuning(L"SunDiscPercent",40),0,100));
    foamBrightPercent=float(std::clamp(ReadTuning(L"FoamBrightnessPercent",100),0,200));
    sparklePercent=float(std::clamp(ReadTuning(L"SparklePercent",100),25,200));
    waterColourTint=ReadTuningColour(L"WaterColour",0x808080u);
    deepColour=ReadTuningColour(L"DeepColour",0x0e2a28u);
    absorptionColour=ReadTuningColour(L"AbsorptionColour",0xff8c6bu);
    // Profile-file queries belong on reload, never in the draw loop. Sync()
    // invokes this function whenever the verified location/scope changes.
    scopedWaterColour=renderer::locationtuning::HasScopedValue(L"Water",L"WaterColour");
    scopedDeepColour=renderer::locationtuning::HasScopedValue(L"Water",L"DeepColour");
    scopedAbsorptionColour=renderer::locationtuning::HasScopedValue(L"Water",L"AbsorptionColour");
    waveSofteningPercent=float(std::clamp(ReadTuning(L"WaveSofteningPercent",100),0,400));
    // 0=off, 1=solid magenta (confirms this pixel is our water shader),
    // 2=grayscale celestial-glint term only (x3 gain), isolated from the
    // rest of the water shading - for debugging "the sun path isn't
    // showing" without guessing which term is actually zero.
    waterDebugMode=float(std::clamp(ReadTuning(L"WaterDebugMode",0),0,2));
    // Sea response: shared wind when linked, native weather otherwise.
    // The already-resolved wind includes the zone's weather damping; never
    // apply that multiplier (or a separate rain boost) a second time.
    weatherResponse=std::clamp(ReadTuning(L"WeatherResponsePercent",100),0,200)*.01f;
    weatherEaseSeconds=float(std::clamp(ReadTuning(L"WeatherEaseSeconds",14),1,120));
    zoneWavesEnabled=ReadTuning(L"ZoneWaves",1)!=0;
    if(!logPath.empty()){
        // One line per CHANGE, not one per reload. The reload chain runs on every
        // settled crossing and on every F7 edit, and an open-plus-write of a file in
        // the game folder is a cost on exactly the path a player is walking a border
        // on (see the chain's own timing line in ModernWoWRenderer.cpp). The text is
        // unchanged, so an existing log still reads the same way.
        std::ostringstream row;
        row<<"tuning enabled="<<effectEnabled<<" master="<<active<<" quality="<<waterQuality
            <<'('<<renderer::water::QualityName(waterQuality)<<')'<<" ripple="<<strength<<" normal="<<normalStrength<<" specular="<<specularStrength<<" reflection="<<reflectionStrength<<" environment="<<environmentStrength<<" distance="<<reflectionDistance<<" thickness="<<reflectionThickness<<" flow="<<flowSpeed<<','<<flowRotation<<" refract="<<refractionStrength<<" absorption="<<absorptionStrength<<" foam="<<shoreFoamStrength<<','<<shoreFoamWidth<<" forever="<<swellStrength<<','<<sunGlintStrength<<','<<fresnelStrength<<','<<crestFoamStrength<<" waves="<<waveScalePercent<<','<<waveSofteningPercent<<" debug="<<waterDebugMode<<'\n';
        static std::string lastRow;
        if(lastRow!=row.str()){lastRow=row.str();std::ofstream out(std::filesystem::path(logPath),std::ios::app);out<<lastRow;}
    }
}
void Configure(const std::wstring& base) {
    mainIni=base+L"ModernWoWRenderer.ini";tuningIni=base+L"GraphicsEffects.ini";
    logPath=base+L"WaterEffect.log";
    shaderCacheDir=renderer::shaderbytecode::Dir(base);
    enabled=false;
    code.clear();vertexCode.clear();codeFromCache=false;
    cachedReplacementPS.Reset();cachedReplacementVS.Reset();
    cachedShaderDevice=nullptr;shaderCreationFailed=false;
    // The replacement shaders (a ~540-slot pixel stage) are the largest fxc
    // cost at load. Compile them on a worker thread: the game is still
    // loading, the first water frame is the earliest possible consumer, and
    // D3DCompile is a standalone CPU pass that needs no device. Water draws
    // before the publish stay native; after it, the next tile switches over.
    // HookDevice installs the depth hooks while compiling is still true, so
    // a device created mid-compile is never left without the capture path.
    ReloadTuning();
    // Prime the CPU field before the first draw too, not one frame later:
    // a saved size/direction or calm wind must not flash the old default sea.
    AdvanceSea(0.f);
    showStatus=renderer::locationtuning::ReadInt(L"Water",L"ShowStatus",0,mainIni.c_str())!=0;
    compiling=true;
    std::thread([cacheDir=renderer::shaderbytecode::Dir(base),logPath=logPath,strength=strength,normalStrength=normalStrength,specularStrength=specularStrength,effectEnabled=effectEnabled]{
        try {
        ComPtr<ID3DBlob> blob,error;
        std::vector<DWORD> ps,vs;
        bool ok=false,fromCache=false;
        {
            std::ofstream log(std::filesystem::path(logPath),std::ios::app);
            // Both stages read the on-disk bytecode cache first; a miss
            // compiles and stores, so only the first launch on a machine
            // pays fxc for the water replacement stage.
            if(renderer::shaderbytecode::Load(cacheDir,source,"main","ps_3_0",blob)) {
                fromCache=true;
                log<<"compile=cached ripple="<<strength<<'\n';
            } else {
                const HRESULT hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.GetAddressOf(),error.GetAddressOf());
                log<<"compile="<<hr<<" ripple="<<strength<<'\n';
                if(SUCCEEDED(hr))renderer::shaderbytecode::Save(cacheDir,source,"main","ps_3_0",blob.Get());
                else if(error)log<<static_cast<const char*>(error->GetBufferPointer());
            }
            if(blob) {
                ps.resize(blob->GetBufferSize()/4);memcpy(ps.data(),blob->GetBufferPointer(),blob->GetBufferSize());
                blob.Reset();error.Reset();
                if(renderer::shaderbytecode::Load(cacheDir,vertexSource,"main","vs_3_0",blob)) {
                    fromCache=true;
                    log<<"vertex_compile=cached normal="<<normalStrength<<" specular="<<specularStrength<<'\n';
                } else {
                    const HRESULT hr=D3DCompile(vertexSource,strlen(vertexSource),nullptr,nullptr,nullptr,"main","vs_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.GetAddressOf(),error.GetAddressOf());
                    log<<"vertex_compile="<<hr<<" normal="<<normalStrength<<" specular="<<specularStrength<<'\n';
                    if(SUCCEEDED(hr))renderer::shaderbytecode::Save(cacheDir,vertexSource,"main","vs_3_0",blob.Get());
                    else if(error)log<<static_cast<const char*>(error->GetBufferPointer());
                }
                if(blob) {
                    vs.resize(blob->GetBufferSize()/4);memcpy(vs.data(),blob->GetBufferPointer(),blob->GetBufferSize());
                    blob.Reset();
                }
            }
            ok=!ps.empty()&&!vs.empty();
            log<<(ok?"water backend ready; ":"water backend NOT ready; ")
               <<"live enabled="<<effectEnabled<<" (GraphicsEffects.ini/location settings)\n";
        }
        // Publish both vectors and the readiness flag under the same lock the
        // first CreatePixelShader read uses; a partial publish (pixel but no
        // vertex bytecode) would fail every CreateVertexShader until reset.
        std::lock_guard lk(waterCompileMutex);
        code=std::move(ps);vertexCode=std::move(vs);
        codeFromCache=fromCache;
        enabled=ok;
        compiling=false;
        } catch (...) {
        // An exception escaping this std::thread calls std::terminate and
        // kills the whole game (an allocation, the log file, or a cache
        // write). Publish the same state a failed compile would, so the
        // backend stays dormant instead of taking the client down.
        std::lock_guard lk(waterCompileMutex);
        code.clear();vertexCode.clear();codeFromCache=false;
        enabled=false;compiling=false;
        }
    }).detach();
}
// Once per frame, from the same weather observation the cloud deck reads (see the call site in
// ModernWoWRenderer.cpp), so water and sky cannot disagree about whether it is raining. Water does
// not read the client's weather memory itself: one reader, many reactions.
void FeedWeather(int kind,float strength) noexcept {
    seaWeather.kind=std::clamp(kind,0,3);
    seaWeather.strength=std::isfinite(strength)?std::clamp(strength,0.f,1.f):0.f;
}
// The zone's own wave energy, as authored for the current map/zone/area. `ZoneWaves=0` pins it to
// 1, which is the pre-zone behaviour: a global WIND/WAVE SIZE setup with no per-area sea state.
float ZoneAgitation() noexcept {
    if(!zoneWavesEnabled)return 1.f;
    const float value=renderer::FrameContext::Current().environment[renderer::WaterAgitation];
    return std::isfinite(value)?std::clamp(value,0.f,4.f):1.f;
}
void AdvanceSea(float dt) {
    // Prepare next frame from the SAME immutable wind/weather snapshot the
    // clouds will read. No dependence on cloud rendering, camera visibility,
    // weather replacement textures or a successful depth capture.
    const auto& frame=renderer::FrameContext::Current();
    const renderer::water::SeaTuning tuning{ZoneAgitation(),weatherResponse,
        windStrengthPercent*.01f,frame.wind.DrivesWaves(),frame.wind.strength};
    const auto sea=seaFilter.Observe(tuning,seaWeather,dt,weatherEaseSeconds);
    seaAgitation=sea.agitation;seaMotion=sea.motion;seaShear=sea.shear;seaWind=sea.wind;
    // Prefer the main camera; waves-only/no-depth frames can use the most
    // recent accepted draw anchor. It chooses only the turn pivot, never
    // the per-draw phase (reflection draws retain their own camera below).
    const float pivotX=frame.cameraValid?frame.cameraPosition.x:lastWaveAnchor.camera[0];
    const float pivotY=frame.cameraValid?frame.cameraPosition.y:lastWaveAnchor.camera[1];
    waveMotion.Advance(dt,flowSpeed*seaMotion,std::clamp(flowRotation*(1.f+seaShear),0.f,1.f),
        waveScalePercent,frame.wind,pivotX,pivotY,frame.cameraValid||lastWaveAnchor.valid,wavesEnabled);
}
void Present() {
    if(!enabled)return;
    const auto& wind=renderer::FrameContext::Current().wind;
    AdvanceSea(wind.sequence?wind.deltaSeconds:
        seaFrameClock.Observe(renderer::PerformanceCounter(),renderer::PerformanceFrequency()));
    const bool down=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
    if(hotkey&&down&&!keyDown&&renderer::FrameContext::Current().wowFocused) {active=!active;std::ofstream(std::filesystem::path(logPath),std::ios::app)<<"active="<<active<<'\n';}
    keyDown=down;
    static unsigned frames=0;
    if(++frames%600==0)
        std::ofstream(std::filesystem::path(logPath),std::ios::app)
            <<"water frame candidates="<<frameCandidates<<" replaced="<<frameMatches
            <<" active="<<active<<" enabled="<<effectEnabled<<" sceneDraws="<<frameSceneMatches
            <<" liquidLayoutsIgnored="<<frameNearMisses<<" shaderFailure="<<shaderCreationFailed
            <<" sea="<<seaAgitation<<","<<seaMotion<<","<<seaShear<<" weather="<<seaWeather.kind
            <<','<<seaWeather.strength<<'\n';
    frameMatches=frameCandidates=frameSceneMatches=frameNearMisses=0;
}
inline void Reset(IDirect3DDevice9* d) {
    // Drop cached texture references before D3D9 Reset: keeping even a lookup
    // texture alive here can make the device reject reset of default-pool assets.
    renderer::water::ResetObservationCache(d);
    ResetMrtCapabilities(d);
    waterTargetDesc = {}; waterTargetDescValid = false;
    // Wind, sea state and accumulated phases survive GPU loss. They are CPU
    // state, not device resources; forgetting them would pop every crest.
    lastWaveAnchor.valid=false;
    localLightConstantCache.valid=false;
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
    // Save each touched constant span as one block. Pixel c180..c218 includes
    // local lights, the untouched c197..c199 gap, and all water controls, so
    // setup and restoration each cross the D3D9 driver only once.
    float oldPixelConstants[156]{},oldVertexControls[20]{},oldVertexFog[4]{};
    bool pixelConstantsSaved=false,vertexFogFallback=false;
    ComPtr<IDirect3DBaseTexture9> oldExtraTextures[2];
    DWORD oldSampler[2][6]{};bool extraState=false;
    ComPtr<IDirect3DSurface9> oldMaskRT;bool maskBound=false;
    ComPtr<IDirect3DSurface9> oldDepthRT;bool depthBound=false;
    D3DVIEWPORT9 oldViewport{};
    DWORD oldColorWrite[2]{};
    bool targetState=false;
    // True while the shared D3DSBT_ALL block holds this scope's original
    // pixel/vertex shader, texture and sampler state (everything the block
    // covers), leaving only the framebuffer attachments and the shader
    // constants to be saved and restored manually.
    bool blockState=false;

    void RestoreTargets() noexcept {
        if(!device||!targetState)return;
        if(maskBound)device->SetRenderTarget(1,oldMaskRT.Get());
        if(depthBound)device->SetRenderTarget(2,oldDepthRT.Get());
        // The color-write render states are block state: Apply restores
        // them, so only the legacy path writes them back manually.
        if(!blockState) {
            device->SetRenderState(D3DRS_COLORWRITEENABLE1,oldColorWrite[0]);
            device->SetRenderState(D3DRS_COLORWRITEENABLE2,oldColorWrite[1]);
        }
        device->SetViewport(&oldViewport);
        maskBound=depthBound=targetState=false;
    }

    void BindTargets(IDirect3DDevice9* d) {
        if(!waterTargetSurface||!waterMaskSurface||!waterDepthSurface||!waterTargetDescValid)return;
        ComPtr<IDirect3DSurface9> current;
        if(FAILED(d->GetRenderTarget(0,current.GetAddressOf()))||current.Get()!=waterTargetSurface||
           !SupportsWaterMrt(d))return;
        // The mask/depth resources were created to this immutable target
        // descriptor in BeforeClear. Fetching all three descriptors and
        // re-querying adapter/device capabilities for every tile was pure
        // per-draw driver overhead; retain the exact target descriptor instead.
        const D3DSURFACE_DESC& colorDesc=waterTargetDesc;
        const D3DCAPS9& caps=waterMrtInfo.caps;
        // Above 1x multisampling this returns, and unlike the colour capture in WaterReflection.h it
        // cannot be made to work: D3D9 requires every bound render target to share one multisample
        // type and quality, and the mask/depth surfaces below were created non-multisampled to match
        // an immutable target descriptor. What that costs is the water's own mask and depth outputs -
        // the "this pixel is water" marker the atmosphere pass reads - not the water draw, which goes
        // ahead without them. It is one more reason the client belongs at 1x, where the anti-aliasing
        // in the image pass takes over from hardware multisampling.
        if(colorDesc.MultiSampleType!=D3DMULTISAMPLE_NONE)return;
        if(colorDesc.Format!=D3DFMT_A8R8G8B8&&colorDesc.Format!=D3DFMT_X8R8G8B8&&
           !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS))return;
        // NOTFOUND means an unbound slot; other query failures cannot be safely restored.
        HRESULT maskHR=d->GetRenderTarget(1,oldMaskRT.GetAddressOf());
        HRESULT depthHR=d->GetRenderTarget(2,oldDepthRT.GetAddressOf());
        // The color-write reads only serve the manual restore; the block
        // path recovers those render states through Apply.
        if((FAILED(maskHR)&&maskHR!=D3DERR_NOTFOUND)||(FAILED(depthHR)&&depthHR!=D3DERR_NOTFOUND)||
           FAILED(d->GetViewport(&oldViewport))||
           (!blockState&&(FAILED(d->GetRenderState(D3DRS_COLORWRITEENABLE1,&oldColorWrite[0]))||
                          FAILED(d->GetRenderState(D3DRS_COLORWRITEENABLE2,&oldColorWrite[1])))))return;
        targetState=true;
        HRESULT hr=d->SetRenderTarget(1,waterMaskSurface);
        maskBound=SUCCEEDED(hr);
        if(SUCCEEDED(hr)){hr=d->SetRenderTarget(2,waterDepthSurface);depthBound=SUCCEEDED(hr);}
        if(SUCCEEDED(hr))hr=d->SetRenderState(D3DRS_COLORWRITEENABLE1,15);
        if(SUCCEEDED(hr))hr=d->SetRenderState(D3DRS_COLORWRITEENABLE2,15);
        if(SUCCEEDED(hr))hr=d->SetViewport(&oldViewport);
        if(FAILED(hr)){RestoreTargets();ReportFailure("water MRT binding",hr);}
    }

    // One line per distinct unapproved liquid signature, bounded so an
    // unfamiliar shader family cannot flood the log or cost per-draw time.
    static void ReportNearMiss(const renderer::water::Observation& observation) {
        ++frameNearMisses;
        static std::unordered_set<std::string> reported;
        if(reported.size()>=32)return;
        char line[224];
        std::snprintf(line,sizeof(line),
            "liquid material not covered by the captured water shaders: slot0=%ux%u slot1=%ux%u"
            " vs=0x%016llx ps=0x%016llx vertexShader=%d pixelShader=%d;"
            " add the shader identity if this is real water",
            observation.width[0],observation.height[0],observation.width[1],observation.height[1],
            static_cast<unsigned long long>(observation.vsHash),
            static_cast<unsigned long long>(observation.psHash),
            observation.vertexShaderBound?1:0,observation.pixelShaderBound?1:0);
        if(reported.insert(line).second&&!logPath.empty())
            std::ofstream(std::filesystem::path(logPath),std::ios::app)<<line<<'\n';
    }
    static void ReportFogFallback(const renderer::water::Observation& observation,const float* c4) {
        static std::unordered_set<std::string> reported;
        char line[288];
        std::snprintf(line,sizeof(line),
            "water vertex fog c4 is not a fog curve; using unfogged fallback:"
            " slot0=%ux%u slot1=%ux%u vs=0x%016llx ps=0x%016llx c4=(%.7g,%.7g,%.7g,%.7g)",
            observation.width[0],observation.height[0],observation.width[1],observation.height[1],
            static_cast<unsigned long long>(renderer::g_trackedState.vsHash),
            static_cast<unsigned long long>(renderer::g_trackedState.psHash),
            c4[0],c4[1],c4[2],c4[3]);
        if(reported.insert(line).second&&!logPath.empty())
            std::ofstream(std::filesystem::path(logPath),std::ios::app)<<line<<'\n';
    }

    explicit Scope(IDirect3DDevice9* d,bool skip=false,
                   const renderer::water::Observation* observed=nullptr) noexcept {
        if(skip||!enabled||!active||!effectEnabled)return;
        // A shader-creation failure is permanent until device reset; skip
        // the Observe + constant reads + all CPU math for every draw.
        if(shaderCreationFailed)return;
        try {
            const auto observation=observed?*observed:renderer::water::Observe(d);
            if(!observation.liquid) {
                // A draw carrying the liquid two-texture signature that the
                // captured shader families do not cover is exactly how a whole
                // 533-unit tile stays native water. Report each distinct
                // signature once: these are the hashes a new water family has
                // to be added to, and no other log line names them.
                if(observation.candidate)ReportNearMiss(observation);
                return;
            }
            ++frameCandidates;
            // The original pixel/vertex shader is either the block's or the
            // legacy save's to restore, so it is fetched only once the
            // capture path is known (below, after the constant reads).
            if(FAILED(d->GetPixelShaderConstantF(180,oldPixelConstants,39)))return;
            pixelConstantsSaved=true;
            if(FAILED(d->GetVertexShaderConstantF(200,oldVertexControls,5)))return;
            // c5..c8 provide this draw's model-view transform; c33 and c35
            // provide native lighting. Fetch their enclosing span once instead
            // of crossing the D3D9 driver three times for scattered reads.
            float nativeVertexConstants[31][4]{};
            if(FAILED(d->GetVertexShaderConstantF(5,nativeVertexConstants[0],31)))return;
            // This replacement VS consumes native c4 as its fog curve for every
            // material, including camera-dependent VS variants on canonical layouts.
            // The documented variants use c4 for fog, but the angle-dependent client
            // variant reported by players may not. If the contents do not have the
            // curve's finite shape, replace them with the identity fog curve for this
            // draw; otherwise a garbage exponent/offset can collapse the whole water
            // colour to fogColor and look like the water shader switched off.
            if(SUCCEEDED(d->GetVertexShaderConstantF(4,oldVertexFog,1))&&
               !renderer::water::HasFogCurve(oldVertexFog)) {
                vertexFogFallback=true;
                ReportFogFallback(observation,oldVertexFog);
            }
            float pixelConstants[39][4];
            std::memcpy(pixelConstants,oldPixelConstants,sizeof(pixelConstants));
            float* controls=pixelConstants[20]; // c200 within the c180 block
            std::fill(controls,controls+76,0.f);
            controls[1]=strength;controls[2]=normalStrength;controls[3]=specularStrength;
            std::memcpy(controls+4,nativeVertexConstants[28],sizeof(float)*4); // c33
            std::memcpy(controls+8,nativeVertexConstants[30],sizeof(float)*4); // c35
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
            const bool haveCapture=reflectionScene&&reflectionDepth;
            // Viewport MaxZ is folded into the depth unpack pair here, so
            // recovering a captured depth is one subtraction and one divide
            // instead of a divide and an extra multiply at every lookup (three
            // ray loops and the surface test each did that per pixel).
            const float depthRange=haveCapture&&reflectionData[7]>1e-4f?reflectionData[7]:1.f;
            // Missing INTZ (e.g. MSAA) must still render animated modern water.
            // All-zero unpack constants produced 0/0 even with captureValid=0;
            // multiplying that NaN by a disabled feature does not remove it.
            controls[12]=(haveCapture?reflectionData[0]:1.001f)*depthRange;
            controls[13]=(haveCapture?reflectionData[1]:-.1f)*depthRange;
            controls[14]=haveCapture?reflectionData[2]:1.f;
            controls[15]=haveCapture?reflectionData[3]:1.f;
            controls[19]=depthRange;
            controls[16]=reflectionsEnabled&&haveCapture?reflectionStrength:0;
            // c204.yz: the two ray marches' trip counts, straight out of the quality ladder.
            // Zero skips the loop through the same test that already stopped it, and the loops
            // themselves are written against twelve, so a level can hand them any count up to
            // that without a second compiled shader - one source, one bytecode cache entry,
            // one CreatePixelShader, whatever the level is.
            controls[17]=float(qualityPlan.ssrSteps);
            controls[18]=float(qualityPlan.lightSteps);
            controls[20]=reflectionsEnabled?environmentStrength:0;controls[21]=reflectionDistance;controls[22]=reflectionThickness;controls[23]=haveCapture?1.f:0.f;
            // x=anti-alias gain for the wave octave. yz=refraction offset, in uv:
            // the wave slope is unitless, the captured scene is in pixels, so the
            // conversion happens once per draw here rather than per pixel.
            controls[24]=waveSofteningPercent*.16f;
            const float refractGain=(refractionEnabled?refractionStrength:0.f)*120.f;
            controls[25]=haveCapture?refractGain*reflectionData[5]:0.f;
            controls[26]=haveCapture?refractGain*reflectionData[6]:0.f;
            controls[27]=waterDebugMode;
            controls[28]=refractionEnabled?refractionStrength:0;controls[29]=depthEnabled?absorptionStrength:0;controls[30]=foamEnabled?shoreFoamStrength*(.7f+.3f*seaAgitation):0;controls[31]=shoreFoamWidth;
            controls[1]=wavesEnabled?strength*seaAgitation:0;
            controls[2]=wavesEnabled?normalStrength*std::clamp(std::sqrt(seaAgitation),0.f,2.5f):0;
            controls[32]=wavesEnabled?swellStrength*seaAgitation:0;controls[33]=sunGlintEnabled?sunGlintStrength:0;controls[34]=fresnelStrength;controls[35]=foamEnabled?crestFoamStrength*seaAgitation:0;
            // The wave field itself is built in world space. Its phase carries
            // the camera position (so the pattern is nailed to the terrain and
            // not to a tile's uv or to the viewport) and the flow animation,
            // both reduced in double and wrapped into a period so the shader
            // never evaluates a coordinate large enough to lose the fraction
            // bits that keep noise from collapsing into squares.
            const auto& waveFrame=renderer::FrameContext::Current();
            // The anchor comes from THIS draw's own model->view matrix, read back from the
            // vertex constants the shader is about to use. A frame-captured camera cannot
            // serve here: it is captured on the first draw of the frame that carries the
            // perspective constants, so a water tile drawn before it is anchored to LAST
            // frame's camera and the sea slides backwards by exactly the distance the
            // player moved, and the client's own reflection pass renders water under a
            // mirrored camera that the capture never sees at all. Reading the draw is also
            // what makes the vertex stage's camera-relative coordinate and this phase two
            // halves of one anchor instead of two approximations of one.
            float modelView[4][4]{};
            std::memcpy(modelView,nativeVertexConstants,sizeof(modelView));
            auto anchor=renderer::water::AnchorFromModelView(modelView);
            if(!anchor.valid) {
                // No rigid basis on this draw (an unfilled constant register, a fixture):
                // fall back to the captured camera, which is at least the right world, and
                // to the captured up axis - taken across the columns, which is the same
                // convention the vertex stage uses for world +Z in view space.
                anchor.camera[0]=waveFrame.cameraPosition.x;
                anchor.camera[1]=waveFrame.cameraPosition.y;
                anchor.valid=waveFrame.cameraValid;
                const bool viewValid=waveFrame.viewRawValid;
                anchor.up[0]=viewValid?waveFrame.viewRaw.m[0][2]:0.f;
                anchor.up[1]=viewValid?waveFrame.viewRaw.m[1][2]:0.f;
                anchor.up[2]=viewValid?waveFrame.viewRaw.m[2][2]:1.f;
            }
            if(anchor.valid)lastWaveAnchor=anchor;
            // Direction, speed, shear and their phases were integrated once at
            // Present. Both shader stages and every draw share that field.
            const auto wave=waveMotion.Build(anchor.camera[0],anchor.camera[1],anchor.valid,wavesEnabled);
            // Geometry waves: the lift and the slide toward the crest, with the steepness
            // already clamped fold-safe on the CPU, so no slider combination can turn a
            // wave inside out. `sigma` goes to the pixel stage whether or not the mesh is
            // being displaced, because the crest sharpening and the whitecap mask are
            // properties of the sea and not of the toggle.
            const auto geometry=renderer::water::BuildGeometryWaves(geometryWavePercent,
                waveMotion.ScalePercent(),seaAgitation,
                geometryWavesEnabled&&qualityPlan.geometryTrains>=renderer::water::kGeometryTrainCount,
                wavesEnabled);
            controls[0]=geometry.sigma;
            controls[36]=wave.lattice[0];controls[37]=wave.lattice[1];
            controls[38]=wave.lattice[2];controls[39]=wave.lattice[3];
            controls[40]=wave.ripplePhase[0];controls[41]=wave.ripplePhase[1];
            // The two trains the vertex stage displaces share their register with the
            // pixel stage, and the four it does not are one float4 of their own (c217):
            // gathers from spread registers would cost the pixel stage more than the
            // four trains it saves, and a phase has to arrive contiguous to be added to
            // a batch of projections in one instruction.
            controls[42]=wave.swellPhase[0];controls[43]=wave.swellPhase[1];
            controls[44]=wave.uvDrift[0];controls[45]=wave.uvDrift[1];
            // c211.z: the WATER QUALITY level itself. Both stages compare it against the gates
            // in src/Water/WaterQuality.h, which spells each one as `level - .5`, so the level
            // can never sit on a boundary and the shader cannot disagree with the plan above
            // by one. w is the wave gate, and FLAT quality closes it the same way WAVES off
            // does: no slope, no distortion, no roughness and no foam survive either switch.
            controls[46]=float(qualityPlan.level);
            controls[47]=wavesEnabled&&qualityPlan.waves?1.f:0.f;
            // Per-body authoring. The up axis comes from the draw's own matrix, so the sky
            // gradient, the sun elevation and the wind term point the right way for this
            // draw - including a reflected water pass, which runs under the reflection
            // camera rather than the player's.
            controls[48]=anchor.up[0];controls[49]=anchor.up[1];controls[50]=anchor.up[2];
            controls[51]=seaWind;   // authored WIND, scaled by how much wind the water is actually in
            // Light penetration is authored as "how far light reaches" instead of an
            // absorption coefficient, which is how the reference describes it: 200 is a
            // clear mountain lake, 5 is swamp soup. 0.088 per view unit of column puts
            // the first e-fold near thirty yards at the shipped default.
            controls[52]=.088f*(100.f/clarityPercent);
            controls[53]=.25f*roughnessPercent*.01f;
            controls[54]=.02f+.06f*sheenPercent*.01f;
            controls[55]=shoalFoamPercent*.01f;
            // Tint is a multiply (0x808080 neutral) so the zone's own painted water
            // survives coloured rather than replaced; the deep colour is absolute,
            // because it is what the column converges to, and the absorption triple is
            // per-channel rate: a red-heavy value kills red first, which is why a deep
            // pool reads teal rather than as a darker grey.
            const auto byte=[&](unsigned colour,unsigned shift,float divisor) {
                return float((colour>>shift)&255u)/divisor;
            };
            controls[56]=byte(waterColourTint,16,128.f);controls[57]=byte(waterColourTint,8,128.f);
            controls[58]=byte(waterColourTint,0,128.f);controls[59]=sunDiscPercent*.004f;
            controls[60]=byte(deepColour,16,255.f);controls[61]=byte(deepColour,8,255.f);
            controls[62]=byte(deepColour,0,255.f);controls[63]=foamBrightPercent*.01f;
            controls[64]=byte(absorptionColour,16,128.f);controls[65]=byte(absorptionColour,8,128.f);
            controls[66]=byte(absorptionColour,0,128.f);controls[67]=sparklePercent*.01f;
            // Per-zone colours (EnvironmentProfiles.ini [Zone.N]/[Area.N], resolved and
            // blended by the environment manager) sit between the two authored layers:
            // they replace the global GraphicsEffects colours above, because azure in
            // Booty Bay is the zone's look, not a user preference - but an explicit
            // per-location colour in LocationGraphics.ini is a deliberate override of
            // exactly this place, so it still wins, key by key. The resolved values
            // are already in register space (tint/absorption over 128, deep over 255),
            // so this is a copy, not a second conversion that could drift from it.
            if(waveFrame.environment.water.authored) {
                const auto& zoneWater=waveFrame.environment.water;
                if(!scopedWaterColour)
                    {controls[56]=zoneWater.tint[0];controls[57]=zoneWater.tint[1];controls[58]=zoneWater.tint[2];}
                if(!scopedDeepColour)
                    {controls[60]=zoneWater.deep[0];controls[61]=zoneWater.deep[1];controls[62]=zoneWater.deep[2];}
                if(!scopedAbsorptionColour)
                    {controls[64]=zoneWater.absorption[0];controls[65]=zoneWater.absorption[1];controls[66]=zoneWater.absorption[2];}
            }
            // c217: the four trains only the shading carries, in one contiguous float4.
            controls[68]=wave.swellPhase[2];controls[69]=wave.swellPhase[3];
            controls[70]=wave.swellPhase[4];controls[71]=wave.swellPhase[5];
            // c218: the two shortest trains, which cross the coarse band at the scale
            // the chop lives at - the pair that stops the surviving crest families
            // from regrouping into lines at range.
            controls[72]=wave.swellPhase[6];controls[73]=wave.swellPhase[7];
            // c218.z: the shore back-flow run-up, in yards. It is built from the same geometry
            // amplitude the mesh lifts by - a run-up is an arriving crest spending its height
            // on the slope - so it inherits WAVE HEIGHT, WAVE SIZE, the zone's own sea state and
            // the WAVES switch without a second authoring path, and it is zero on water that was
            // authored flat or at a level below the feature, where nothing arrives to run up.
            controls[74]=renderer::water::SwashYards(qualityPlan,geometry.amplitude);
            // Both stages share the swell phases of the trains they both carry - and at WATER
            // QUALITY 8 and above that is three trains, not two: c202.z carries train C's own
            // wrapped phase, the same number c217.x gives the pixel stage, so the mid-scale
            // crest this stage lifts is the crest those pixels light. The vertex stage also
            // samples the low-frequency warp and the grouping envelope from this anchor and
            // interpolates both to pixels, keeping the lifted crest irregular and grouped
            // without re-hashing noise per pixel.
            // Registers: c200 control, c201 lattice, c202 swell phase, c203 warp, c204 grouping.
            // c204.z is the mid-wave gate: a multiplier rather than a branch, because it has to
            // scale the renormalization that keeps WAVE HEIGHT the same peak lift at every level.
            float vertexControls[20]={geometry.amplitude,geometry.lateral,geometry.sigma,
                geometry.cellYards,
                wave.lattice[0],wave.lattice[1],wave.lattice[2],wave.lattice[3],
                wave.swellPhase[0],wave.swellPhase[1],wave.swellPhase[2],0.f,
                wave.warpPhase[0],wave.warpPhase[1],0.f,0.f,
                wave.groupPhase[0],wave.groupPhase[1],qualityPlan.midGeometry?1.f:0.f,0.f};
            if(cachedShaderDevice != d) {
                cachedReplacementPS.Reset();cachedReplacementVS.Reset();
                cachedShaderDevice=d;shaderCreationFailed=false;
            }
            if(shaderCreationFailed)return;
            if(!cachedReplacementPS || !cachedReplacementVS) {
                // The bytecode is published by the startup compile thread;
                // take the same lock the publish uses. Empty means still
                // compiling (or failed) - this draw stays native.
                std::lock_guard lk(waterCompileMutex);
                if(code.empty()||vertexCode.empty())return;
                cachedReplacementPS.Reset();
                cachedReplacementVS.Reset();
                cachedShaderDevice = d;
                HRESULT hr=d->CreatePixelShader(code.data(), cachedReplacementPS.GetAddressOf());
                if(SUCCEEDED(hr))hr=d->CreateVertexShader(vertexCode.data(), cachedReplacementVS.GetAddressOf());
                if(FAILED(hr)&&codeFromCache) {
                    // A cached blob the driver refuses means the entry is
                    // stale or corrupt for this machine. Recompile once
                    // (deterministic fxc), replace the stale entries, and
                    // retry before falling back to native water. We hold
                    // waterCompileMutex, so the recompiled publish cannot
                    // race a concurrent first draw.
                    ComPtr<ID3DBlob> psBlob,vsBlob,rerr;
                    const HRESULT rhr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,psBlob.GetAddressOf(),rerr.GetAddressOf());
                    const HRESULT rhr2=SUCCEEDED(rhr)?D3DCompile(vertexSource,strlen(vertexSource),nullptr,nullptr,nullptr,"main","vs_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,vsBlob.GetAddressOf(),rerr.GetAddressOf()):E_FAIL;
                    if(SUCCEEDED(rhr)&&SUCCEEDED(rhr2)) {
                        renderer::shaderbytecode::Save(shaderCacheDir,source,"main","ps_3_0",psBlob.Get());
                        renderer::shaderbytecode::Save(shaderCacheDir,vertexSource,"main","vs_3_0",vsBlob.Get());
                        code.assign(static_cast<const DWORD*>(psBlob->GetBufferPointer()),static_cast<const DWORD*>(psBlob->GetBufferPointer())+psBlob->GetBufferSize()/4);
                        vertexCode.assign(static_cast<const DWORD*>(vsBlob->GetBufferPointer()),static_cast<const DWORD*>(vsBlob->GetBufferPointer())+vsBlob->GetBufferSize()/4);
                        codeFromCache=false;
                        hr=d->CreatePixelShader(code.data(), cachedReplacementPS.GetAddressOf());
                        if(SUCCEEDED(hr))hr=d->CreateVertexShader(vertexCode.data(), cachedReplacementVS.GetAddressOf());
                    } else if(rerr){
                        std::ofstream(std::filesystem::path(logPath),std::ios::app)
                            <<static_cast<const char*>(rerr->GetBufferPointer());
                    }
                    std::ofstream(std::filesystem::path(logPath),std::ios::app)
                        <<"water cache rejected; recompile hr="<<hr<<'\n';
                }
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
            // StretchRect preserves encoded scene bytes. If the native target
            // uses sRGB output, decode its copy before lighting/refraction and
            // let the unchanged output state encode once, not twice. Depth is
            // data and must NEVER be gamma-decoded.
            DWORD sceneSrgb = FALSE;
            if(FAILED(d->GetRenderState(D3DRS_SRGBWRITEENABLE,&sceneSrgb)))return;
            const D3DSAMPLERSTATETYPE samplerStates[]={D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_SRGBTEXTURE};
            {
                // One Capture replaces the ~16 shader/texture/sampler reads
                // of the legacy save and every restore-side write: the
                // block holds the original pixel and vertex shader, both
                // textures and all six sampler states per slot.
                blockState=renderer::detail::BeginPassState(d);
                if(!blockState) {
                    if(FAILED(d->GetPixelShader(original.GetAddressOf())) ||
                       FAILED(d->GetVertexShader(originalVertex.GetAddressOf())))return;
                    if(!original || !originalVertex)return;
                    for(unsigned slot=0;slot<2;++slot){if(FAILED(d->GetTexture(2+slot,oldExtraTextures[slot].GetAddressOf())))return;for(unsigned state=0;state<6;++state)if(FAILED(d->GetSamplerState(2+slot,samplerStates[state],&oldSampler[slot][state])))return;}
                }
                device=d;extraState=!blockState;
                if(vertexFogFallback) {
                    // Identity curve: fog=1 at every distance, preserving the computed
                    // water color instead of fogging the entire surface to the clear color.
                    const float noFog[4]={0.f,1.f,1.f,0.f};
                    if(FAILED(d->SetVertexShaderConstantF(4,noFog,1))){Restore();return;}
                }
                if(FAILED(d->SetTexture(2,reflectionScene))||FAILED(d->SetTexture(3,reflectionDepth))){Restore();return;}
                for(unsigned slot=0;slot<2;++slot) {
                    const DWORD values[]={DWORD(slot?D3DTEXF_POINT:D3DTEXF_LINEAR),DWORD(slot?D3DTEXF_POINT:D3DTEXF_LINEAR),
                        D3DTEXF_NONE,D3DTADDRESS_CLAMP,D3DTADDRESS_CLAMP,slot?FALSE:sceneSrgb};
                    for(unsigned state=0;state<6;++state) {
                        const HRESULT hr=d->SetSamplerState(2+slot,samplerStates[state],values[state]);
                        if(FAILED(hr)){Restore();ReportFailure("water sampler setup",hr);return;}
                    }
                }
            }
            if(FAILED(d->SetPixelShader(replacement.Get()))){Restore();return;}
            device=d;
            BindTargets(d);
            if(FAILED(d->SetVertexShader(replacementVertex.Get()))||
               FAILED(d->SetVertexShaderConstantF(200,vertexControls,5))){Restore();return;}
            {
                const auto& frame=renderer::FrameContext::Current();
                const auto& manager=renderer::LocalLightManager::Instance();
                const std::uint64_t signature=manager.SelectionSignature();
                const float intensity=manager.IntensityScale();
                auto& cache=localLightConstantCache;
                if(!cache.valid||cache.frameIndex!=frame.frameIndex||
                   cache.selectionSignature!=signature||cache.intensityScale!=intensity||
                   cache.reflection!=reflectionStrength||cache.managerEnabled!=manager.Enabled()||
                   cache.reflections!=reflectionsEnabled||cache.cameraValid!=frame.cameraValid||
                   cache.viewValid!=frame.viewRawValid||cache.localLights!=qualityPlan.localLights) {
                    std::fill(cache.values,cache.values+68,0.f);
                    if(reflectionsEnabled&&manager.Enabled()&&frame.cameraValid&&frame.viewRawValid){
                        // WATER QUALITY caps how many emitters this surface mirrors, and it caps
                        // them HERE rather than in the shader: the transform below is per light
                        // per frame, and the shader's loop already breaks on the count it is
                        // given, so a low level skips both the eight matrix transforms and the
                        // eight pow(,64) lobes instead of computing them and throwing them away.
                        const auto& selected=manager.Selected();
                        unsigned count=std::min<unsigned>(unsigned(std::max(qualityPlan.localLights,0)),
                                                          unsigned(selected.size()));
                        for(unsigned i=0;i<count;++i){const auto& light=selected[i];const auto& m=frame.viewRaw.m;
                            for(int axis=0;axis<3;++axis)cache.values[i*4+axis]=light.position.x*m[0][axis]+light.position.y*m[1][axis]+light.position.z*m[2][axis]+m[3][axis];
                            cache.values[i*4+3]=light.radius;cache.values[(8+i)*4]=light.color.x;cache.values[(8+i)*4+1]=light.color.y;cache.values[(8+i)*4+2]=light.color.z;cache.values[(8+i)*4+3]=light.intensity*intensity;
                        }
                        cache.values[64]=float(count);cache.values[65]=reflectionStrength;
                    }
                    cache.frameIndex=frame.frameIndex;cache.selectionSignature=signature;
                    cache.intensityScale=intensity;cache.reflection=reflectionStrength;
                    cache.managerEnabled=manager.Enabled();cache.reflections=reflectionsEnabled;
                    cache.cameraValid=frame.cameraValid;cache.viewValid=frame.viewRawValid;
                    cache.localLights=qualityPlan.localLights;cache.valid=true;
                }
                // Preserve c197..c199 from the saved block, splice in the cached
                // lights and water controls, then upload c180..c218 in one call.
                std::memcpy(pixelConstants,cache.values,sizeof(cache.values));
                if(FAILED(d->SetPixelShaderConstantF(180,pixelConstants[0],39))){Restore();return;}
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
        if(blockState) {
            // The block holds the shader/texture/sampler state: write back
            // only the two constant spans, then let Apply return everything
            // else in a single call.
            if(pixelConstantsSaved)device->SetPixelShaderConstantF(180,oldPixelConstants,39);
            device->SetVertexShaderConstantF(200,oldVertexControls,5);
            renderer::detail::EndPassState();
            blockState=false;
        } else {
            device->SetPixelShader(original.Get());
            device->SetVertexShader(originalVertex.Get());
            if(pixelConstantsSaved)device->SetPixelShaderConstantF(180,oldPixelConstants,39);
            device->SetVertexShaderConstantF(200,oldVertexControls,5);
            if(extraState) {
                const D3DSAMPLERSTATETYPE states[]={D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,
                    D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_SRGBTEXTURE};
                for(unsigned slot=0;slot<2;++slot) {
                    device->SetTexture(2+slot,oldExtraTextures[slot].Get());
                    for(unsigned state=0;state<6;++state)
                        device->SetSamplerState(2+slot,states[state],oldSampler[slot][state]);
                }
            }
        }
        // c4 is outside the block's documented water spans. Restore it after either
        // state-block or manual restoration, including every early-return path.
        if(vertexFogFallback)device->SetVertexShaderConstantF(4,oldVertexFog,1);
        device=nullptr;extraState=pixelConstantsSaved=vertexFogFallback=false;
    }
    ~Scope(){Restore();}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
}
