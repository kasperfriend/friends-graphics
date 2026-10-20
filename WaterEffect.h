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
#include "src/Core/DeviceHealth.h"
#include "src/Water/WaterMaterial.h"
#include "src/Water/WaveField.h"
#include "src/Water/WaterSeaState.h"
#include "src/Water/WaveMotion.h"
#include "src/Water/UnderwaterMedium.h"
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
bool effectEnabled=true,wavesEnabled=true,geometryWavesEnabled=true,reflectionsEnabled=true,refractionEnabled=true,depthEnabled=true,depthFogEnabled=true,foamEnabled=true,sunGlintEnabled=true;
float strength=.003f;
float normalStrength=.16f, specularStrength=.55f;
float reflectionStrength=.70f,environmentStrength=.24f,reflectionDistance=120.f,reflectionThickness=1.8f;
float flowSpeed=1.f,flowRotation=.32f;
// WATER OPACITY: the coverage floor above, 0..100 percent. Read with the other
// tuning so a per-location override and the F7 slider land in the same place.
float opacityPercent=85.f;
float waterVisibilityYards=90.f;
float horizonFogYards=900.f;
// The distance at which the CLIENT's own fog curve has all but finished, in yards,
// reduced once per frame from the curve the draw actually carries. It is the scale
// of the per-pixel distance fog the surface applies instead of the interpolated
// per-vertex one: one number for the frame, so two tiles of the same sea at the same
// distance cannot fog differently, and a curve sampled once per pixel cannot be
// distorted by a triangle edge.
float fogCurveYards=0.f;
float fogCurveStart=0.f,fogCurveEnd=0.f,fogCurvePower=1.f;
bool fogCurveValid=false;
unsigned fogCurveFrame=0xffffffffu;
float refractionStrength=.12f,absorptionStrength=.18f,depthFogStrength=2.f,shoreFoamStrength=.12f,shoreFoamWidth=2.5f;
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

// Whether the camera is inside the liquid, and how far under it. The water draws below
// report the camera against every liquid plane they belong to; the tracker in
// src/Water/UnderwaterMedium.h turns those reports into a ramp that cannot blink. The water
// pixel stage gets its camera-side classification from controls[75] (c218.w), the
// full-screen pass that renders the world inside the medium (src/Effects/Underwater.h) gets
// its submersion from the same tracker, and so do the air passes that fade out on it, so
// there is exactly one camera-vs-water answer per frame and every stage agrees with it.
//
// Note what is NOT here: a switch. Which side of the interface the camera is on is a fact
// about the camera, and the water stage's use of it - classifying by that fact instead of by
// triangle facing, marching the camera-to-surface column instead of a column of air, and
// covering a face by what its own column extinguishes - is the fix for seeing the front of
// one wave through the back of the next. A tuning key that could turn that off would be a key
// that could turn the bug back on. What [Underwater] in GraphicsEffects.ini switches is the
// medium: the full-screen pass and the ramp the air passes fade out on, both gated on
// renderer::Underwater::IsActive() where the ramp is published (VolumeIntegration.h).
renderer::underwater::Tracker submersionTracker;
float underwaterSeconds=0.f;

// What the water draws resolved for the medium this frame. Published per draw because the
// colours and this draw's light belong to the draw that authored them; held across frames
// so a camera under the surface keeps its medium while the sea is behind it and out of
// view. The WAVE FIELD is deliberately not here: its phases are anchored to a camera, and
// the only camera they may be anchored to is the one the full-screen pass reconstructs
// world positions against at composite time - a field published by a draw, or rebuilt at
// the previous frame's Present, is anchored to a camera a frame's worth of swimming away,
// which slides the caustic net across the bed by exactly that much. The composite builds
// its own through UnderwaterFieldAt, below.
struct UnderwaterFrame {
    renderer::underwater::Medium medium;
    float up[3]={0.f,0.f,1.f};
    float lightDir[3]={0.f,0.f,1.f};
    float lightColour[3]={0.f,0.f,0.f};
    float sunElevation=0.f;
    // The player's own [Underwater] turbidity dial, published by the compositor's
    // tuning reload - the one medium dial this stage cannot read for itself,
    // because its section belongs to the full-screen pass. Each draw scales it by
    // its own frame's zone multiplier (exactly as clarity is scaled beside it) and
    // spends it on the liquid's own pixels: the exit wash the pass applies to the
    // pixels around the plane has to apply through the plane as well, or dirty
    // water would fog the world everywhere except where the water itself is.
    float turbidity=1.f;
    bool published=false;
};
UnderwaterFrame underwaterFrame;

// The three authored water colours, resolved once, exactly as the water draw's own controls
// resolve them: the tuned value, replaced by the loaded world location's colour when that
// location authored one, and kept as the tuned value where LocationGraphics.ini scoped an
// explicit override for this place. Both the water draws and the frame's medium go through
// this, so the pass that renders the world under the surface cannot drift from the water it
// is under - and so a clarity or colour change reaches a camera that is not looking at any
// liquid right now, which a diver with the F7 menu open is.
struct WaterColourSet {
    float tint[3]={1.f,1.f,1.f};
    float deep[3]={1.f,1.f,1.f};
    float absorb[3]={1.f,1.f,1.f};
};
// `zoneWater` is the environment the CALLER is drawing under: the draw passes its own
// waveFrame.environment.water, the frame's medium passes the frame it was settled for.
// Taking it rather than reaching for the current frame is what makes the two unable to
// resolve their colours from different frames - the resolver is a function of the globals,
// the zone layer and the location overrides, and nothing else.
WaterColourSet ResolveWaterColourSet(const renderer::ResolvedWaterColours& zoneWater) {
    auto byte=[&](unsigned colour,unsigned shift,float divisor) {
        return float((colour>>shift)&255u)/divisor;
    };
    WaterColourSet out;
    out.tint[0]=byte(waterColourTint,16,128.f);out.tint[1]=byte(waterColourTint,8,128.f);
    out.tint[2]=byte(waterColourTint,0,128.f);
    out.deep[0]=byte(deepColour,16,255.f);out.deep[1]=byte(deepColour,8,255.f);
    out.deep[2]=byte(deepColour,0,255.f);
    out.absorb[0]=byte(absorptionColour,16,128.f);out.absorb[1]=byte(absorptionColour,8,128.f);
    out.absorb[2]=byte(absorptionColour,0,128.f);
    if(zoneWater.authored) {
        if(!scopedWaterColour)
            {out.tint[0]=zoneWater.tint[0];out.tint[1]=zoneWater.tint[1];out.tint[2]=zoneWater.tint[2];}
        if(!scopedDeepColour)
            {out.deep[0]=zoneWater.deep[0];out.deep[1]=zoneWater.deep[1];out.deep[2]=zoneWater.deep[2];}
        if(!scopedAbsorptionColour)
            {out.absorb[0]=zoneWater.absorption[0];out.absorb[1]=zoneWater.absorption[1];
             out.absorb[2]=zoneWater.absorption[2];}
    }
    return out;
}

// The medium the frame is inside: the same clarity, the same absorption strength and the
// same three authored colours the water draw's Beer-Lambert uses, so a seabed under the
// surface fades out with the same coefficient the water above it fades out with and the two
// meet without a seam. There is no second clarity for the underwater view: [Underwater]
// authors the light inside this medium, and WATER CLARITY above authors the medium, so a
// zone that asked for swamp soup is swamp soup under the surface exactly as in it.
renderer::underwater::Medium UnderwaterMediumForFrame(const WaterColourSet& colours,
                                                      float clarityMultiplier=1.f) {
    using renderer::underwater::Vec3;
    // Named in the register order the medium takes them - absorption rate, deep colour, tint -
    // because all three are the same type and a swapped pair would compile and quietly paint
    // every ocean with its own absorption curve.
    const Vec3 absorb{colours.absorb[0],colours.absorb[1],colours.absorb[2]};
    const Vec3 deep{colours.deep[0],colours.deep[1],colours.deep[2]};
    const Vec3 tint{colours.tint[0],colours.tint[1],colours.tint[2]};
    // WATER CLARITY and WATER DEPTH are the medium's own two numbers: how far light reaches,
    // and how much of the column's extinction the depth term is allowed to contribute. There
    // is deliberately no second clarity for the underwater view, because there is one water.
    // With DEPTH off the water stage measures no column at all, so the medium reports itself
    // invalid and the pass does not run - the client's own underwater fog is left standing
    // alone, which is what switching that feature off has always meant.
    // The zone's clarity multiplier is the same number the surface draw uploads
    // in bodyStyle.x, resolved from the same frame's environment at both call
    // sites - one water, one clarity, with the place allowed to say how clear
    // its own water is and the player's WATER CLARITY still the master dial.
    const float zoneClarity=std::isfinite(clarityMultiplier)
        ?std::clamp(clarityMultiplier,.1f,4.f):1.f;
    return renderer::underwater::MediumFor(clarityPercent*zoneClarity,absorptionStrength,absorb,deep,tint,
                                           depthEnabled);
}

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
// When that anchor was read, on the same clock the sea integrates on. It may only
// stand in for a camera the frame did not capture while it is still the place the
// player is standing: a teleport leaves the last anchor of the OLD zone behind, and
// a loading screen leaves it behind with no draw to replace it.
double lastWaveAnchorSeconds=-1.0;
// How long a draw's own camera may stand in for the frame camera. The field's
// phases are continuous at the frame pivot (WaveMotion::Advance compensates every
// heading and size edit there), so a pivot from anywhere but here turns such an
// edit into a phase shift proportional to the distance - which is the water that
// "races" for a few seconds after a teleport and then settles.
constexpr double kAnchorFreshSeconds=.25;
renderer::FrameClock seaFrameClock;
void AdvanceSea(float dt);
inline bool LastWaveAnchorFresh() noexcept {
    if(!lastWaveAnchor.valid||!(lastWaveAnchorSeconds>=0.0))return false;
    const double age=renderer::PerformanceSeconds()-lastWaveAnchorSeconds;
    return age>=0.0&&age<=kAnchorFreshSeconds;
}

// One frame of the underwater ramp, and the frame the full-screen medium reads. Called
// from Present, after the water draws have had their say and before the counters reset, so
// the reports belong to the frame that is over. Defined below the sea state rather than next
// to the medium it publishes, because a frame that drew no liquid has to rebuild the wave
// field, and that is built by the motion and anchored on the draw the sea state remembers.
// Why the camera's side of the water never arrives, counted per frame for the status line:
// a player report of "the medium does nothing" is undiagnosable without knowing which of the
// reading's refusals the draws are hitting, or whether any liquid draw matched at all.
int uwPlaneValid=0,uwPlaneRigid=0,uwPlaneRot=0,uwPlaneRange=0,uwPlaneOffscreen=0,uwPlaneNoVertex=0;
int uwPlaneDetNeg=0,uwPlaneFar=0;
int uwPlaneOutside=0,uwLastCoverage=0,uwLastContains=0;
float uwLastYards=0.f,uwLastVertexZ=0.f,uwLastHorizontal=0.f,uwLastFootprint=0.f;
float uwLastLane=0.f;
int uwPublished=0;
// The render target the player's own view is being drawn into this frame: the
// volume composite's scene target while that path runs, published by
// VolumeIntegration every frame it prepares one, null when the client draws to
// the backbuffer directly. A liquid draw landing anywhere else - the client's
// reflection texture above all - describes a mirrored camera's water, and its
// plane reading must never reach the submersion tracker. This target identity is
// what replaced the old "negative determinant means reflection" test: that sign
// is a handedness convention this tree never established, and guessing it can
// refuse every ordinary draw while admitting the mirrored one.
IDirect3DSurface9* mainTargetSurface=nullptr;
// The draw the next Scope belongs to, handed over by the draw detours, which are
// the only place the primitive's own parameters exist. The plane reading needs
// the local positions of vertices the draw actually consumes - its first and its
// last: tiles may carry their water level in the vertices rather than the matrix
// (measuring the draw origin's plane then answers for a plane one water level
// away), and the pair is what says how far the measured water is from the camera
// horizontally and how much of it there is, without which a raised lake anywhere
// in view reads exactly like the lake the camera is in. Every detour sets this
// immediately before constructing its Scope and ReadLiquidVertices consumes it,
// so a hint can never label a draw it did not come from. The primitive type and
// count ride along because locating the LAST consumed element needs both.
bool nextDrawHintValid=false,nextDrawIndexed=false;
int nextDrawBaseVertexIndex=0;
D3DPRIMITIVETYPE nextDrawPrimitiveType=D3DPT_TRIANGLELIST;
UINT nextDrawPrimitiveCount=0;
UINT nextDrawStartVertex=0,nextDrawStartIndex=0,nextDrawStrideUP=0;
const void* nextDrawVerticesUP=nullptr;
const void* nextDrawIndicesUP=nullptr;
D3DFORMAT nextDrawIndexFormatUP=D3DFMT_INDEX16;
inline void SetNextDrawHint(bool indexed,int baseVertexIndex,UINT startVertex,
    UINT startIndex,const void* verticesUP,const void* indicesUP,
    D3DFORMAT indexFormatUP,UINT strideUP,
    D3DPRIMITIVETYPE primitiveType=D3DPT_TRIANGLELIST,UINT primitiveCount=0) noexcept {
    nextDrawPrimitiveType=primitiveType;nextDrawPrimitiveCount=primitiveCount;
    nextDrawIndexed=indexed;nextDrawBaseVertexIndex=baseVertexIndex;
    nextDrawStartVertex=startVertex;nextDrawStartIndex=startIndex;
    nextDrawVerticesUP=verticesUP;nextDrawIndicesUP=indicesUP;
    nextDrawIndexFormatUP=indexFormatUP;nextDrawStrideUP=strideUP;
    nextDrawHintValid=true;
}
// One consumed vertex pair per (buffers, offsets, primitive) the client keeps
// drawing: liquid chunks are static geometry and their vertices never change, so
// a chunk is locked and read once and answered from here afterwards. The entries
// hold the buffers themselves - a bare pointer could name a different buffer
// after the client frees and reallocates - which is why Reset clears the cache:
// an extra reference to a default-pool buffer would make the client's own Reset
// fail. The primitive is part of the key: the same buffer drawn with a different
// count consumes a different LAST vertex, and a height remembered for one extent
// of a buffer must never answer for another - one buffer can hold two lakes.
// How many emitted vertices of one liquid draw the coverage read copies.
// A liquid chunk is an 8x8 tile grid - a few hundred indices - and even a
// WMO sheet is far under this; a draw past it answers from the first/last
// pair as before rather than copying a merged ocean to ask where it ends.
inline constexpr UINT kMaxLiquidCoverageElements=4096;
struct LiquidVertexSamples {
    float first[3]={0.f,0.f,0.f};
    float last[3]={0.f,0.f,0.f};
    bool haveFirst=false,haveLast=false;
    // Every vertex the draw emits, in emitted order, when the whole draw
    // could be read: PlaneOverCamera measures the camera against these
    // triangles instead of against the first/last rectangle. Points into
    // the cache entry (or the user-pointer scratch below); valid only for
    // the Scope that read it, which is the only consumer.
    const renderer::underwater::Vec3* coverage=nullptr;
    UINT coverageCount=0;
    renderer::underwater::LiquidTopology coverageTopology=
        renderer::underwater::LiquidTopology::TriangleList;
    bool haveCoverage=false;
    // The coverage vertices' exact local x/y extremes, measured as the draw
    // was read in (every position the read accepts is finite by its own
    // check): with them the plane reading refuses a far sheet from its box
    // alone instead of scanning its triangles every frame.
    float coverageMinX=0.f,coverageMaxX=0.f,coverageMinY=0.f,coverageMaxY=0.f;
    bool haveCoverageBounds=false;
};
struct LiquidVertexZEntry {
    ComPtr<IDirect3DVertexBuffer9> vertices;
    ComPtr<IDirect3DIndexBuffer9> indices;
    UINT streamOffset=0,stride=0,startIndex=0,startVertex=0,positionOffset=0;
    int baseVertexIndex=0;
    D3DPRIMITIVETYPE primitiveType=D3DPT_TRIANGLELIST;
    UINT primitiveCount=0;
    bool indexed=false;
    float first[3]={0.f,0.f,0.f};
    float last[3]={0.f,0.f,0.f};
    bool haveLast=false;
    bool valid=false;
    renderer::underwater::Vec3 coverage[kMaxLiquidCoverageElements]{};
    UINT coverageCount=0;
    bool haveCoverage=false;
    float coverageMinX=0.f,coverageMaxX=0.f,coverageMinY=0.f,coverageMaxY=0.f;
    bool haveCoverageBounds=false;
};
LiquidVertexZEntry liquidVertexZCache[64]{};
unsigned liquidVertexZNext=0;
// The coverage read's staging: one draw's worth of emitted positions (and
// the vertex numbers that produced them), filled under one range lock per
// buffer and copied into the cache entry before the next draw reuses it.
// Static rather than stack - 48 KB is not a Scope local's business - and
// safe because Scope runs on the one render thread.
renderer::underwater::Vec3 liquidCoverageScratch[kMaxLiquidCoverageElements]{};
long long liquidCoverageIndexScratch[kMaxLiquidCoverageElements]{};
// Bumped once per Present. A cache miss locks a client buffer, and a read-only
// lock can still wait for the GPU to be finished with it, so at most one new
// chunk is read per frame: a whole coast arriving in a single frame must not
// pay that wait dozens of times over (a first dive once stopped rendering
// that way, on a cold cache and a cold driver). Draws past the budget answer
// from their origin's plane until their turn comes, which is within a second
// of looking at any body of water, and the answer by then is exact.
unsigned presentSerial=0;
unsigned liquidVertexZLockSerial=~0u;
// The exact local x/y extremes of a coverage read, measured once while the
// positions are already in hand (and already known finite - the read checks
// every one): PlaneOverCamera spends them to refuse a far sheet from its
// bounding box instead of re-scanning its triangles on every frame.
inline void MeasureLiquidCoverageBounds(const renderer::underwater::Vec3* verts,UINT count,
    float& minX,float& maxX,float& minY,float& maxY) noexcept {
    minX=minY=1e30f;maxX=maxY=-1e30f;
    for(UINT i=0;i<count;++i) {
        minX=std::min(minX,verts[i].x);maxX=std::max(maxX,verts[i].x);
        minY=std::min(minY,verts[i].y);maxY=std::max(maxY,verts[i].y);
    }
}
// The last vertex declaration the liquid read parsed, and where it said the
// position lives. Liquid draws overwhelmingly share one declaration, and
// re-reading its element array on every liquid draw of every frame was a
// driver call (and a 17-element copy) spent rediscovering the same stream
// and offset. One remembered declaration, held like the buffers beside it
// and dropped with them at reset.
ComPtr<IDirect3DVertexDeclaration9> liquidDeclarationCache;
UINT liquidDeclarationPositionStream=0,liquidDeclarationPositionOffset=0;
bool liquidDeclarationCacheValid=false,liquidDeclarationHavePosition=false;
inline void ClearLiquidVertexZCache() noexcept {
    // Release the buffers and invalidate; the scalar key fields and the
    // coverage array are dead while `valid` is false, and clearing the
    // array entry by entry would only spend the reset rewriting it.
    for(auto& entry:liquidVertexZCache) {
        entry.vertices.Reset();entry.indices.Reset();
        entry.coverageCount=0;entry.haveCoverage=false;entry.haveCoverageBounds=false;
        entry.valid=false;
    }
    liquidDeclarationCache.Reset();
    liquidDeclarationCacheValid=false;
}
void AdvanceUnderwater(float dt) {
    // Wrapped here rather than only in the consumer, because a clock that grows without bound
    // loses its fraction bits: renderer::underwater::Wobble wraps its own argument into the
    // wobble period, but `seconds-floor(seconds/period)*period` is only as precise as
    // `seconds`, and after a few days un-wrapped that is not precise enough to animate
    // anything. Keeping the published clock inside one period keeps every consumer of it
    // exact, at any session length.
    underwaterSeconds+=dt;
    const float wobblePeriod=renderer::underwater::kWobblePeriodSeconds;
    if(!(underwaterSeconds>=0.f&&underwaterSeconds<1e6f)) {
        underwaterSeconds=0.f;   // a NaN, an infinity or a negative dt: start the clock again
    } else {
        // Subtracting whole periods rather than resetting to zero, so the four wobble rates -
        // 1.7, 1.1, 1.3 and .9 times the clock, all even multiples of pi over 20pi - stay
        // continuous across the wrap instead of jumping a frame's worth of phase.
        underwaterSeconds-=std::floor(underwaterSeconds/wobblePeriod)*wobblePeriod;
    }
    if(!enabled||!effectEnabled) {
        // No water means no interface to be on either side of: no held submersion, no ramp,
        // and nothing left over from a swim that ended before the effect was turned back on.
        submersionTracker=renderer::underwater::Tracker{};
        underwaterFrame.published=false;
        return;
    }
    const auto& frame=renderer::FrameContext::Current();
    const float camera[3]={frame.cameraPosition.x,frame.cameraPosition.y,frame.cameraPosition.z};
    submersionTracker.Advance(dt,frame.cameraValid?camera:nullptr,frame.cameraValid);
    submersionTracker.BeginFrame();
    // Keep the published frame current on a frame that drew no liquid at all - which, for a
    // diver looking at the seabed, is most frames: the surface is behind the camera and
    // frustum-culled, so no Scope runs and nothing publishes. Clarity and the authored colours
    // live outside the draw, and so do the up axis and the light: world +Z in view space is a
    // row of this frame's captured view, and the source the surface is lit by is
    // CelestialTracker's confirmed disc, which is a singleton rather than a per-draw constant.
    // Only the light's COLOUR is a per-draw constant (c35), so that one is held - it moves on
    // the scale of a weather front, not of a frame. The wave field is not kept current here at
    // all: the composite builds it fresh for its own camera every frame (UnderwaterFieldAt),
    // which is also what keeps the caustic net alive while no surface is in view. Holding the
    // up axis instead of refreshing it was a visible bug in the most common underwater view
    // there is: it stayed pointed where the camera was looking when it last saw water, which
    // puts the beams, the sky glow and the bed's facing test on the wrong axis for every turn
    // of the diver's head after that.
    if(!underwaterFrame.published) {
        underwaterFrame.medium=UnderwaterMediumForFrame(ResolveWaterColourSet(frame.environment.water),
            zoneWavesEnabled?frame.environment[renderer::WaterClarity]:1.f);
        if(frame.viewRawValid) {
            for(int i=0;i<3;++i)underwaterFrame.up[i]=frame.viewRaw.m[2][i];
        }
        // viewSpaceDirection already points TOWARD the source, which is the direction the pass
        // wants; the draw path arrives at the same vector by negating c201, which the draw path
        // itself uploaded pre-negated.
        const renderer::CelestialBody& sunBody=renderer::CelestialTracker::Instance().Sun();
        const renderer::CelestialBody& moonBody=renderer::CelestialTracker::Instance().Moon();
        const renderer::CelestialBody* body=sunBody.visible?&sunBody
            :(moonBody.visible?&moonBody:nullptr);
        if(body) {
            const float lx=body->viewSpaceDirection.x,ly=body->viewSpaceDirection.y,
                lz=body->viewSpaceDirection.z;
            const float ll=std::sqrt(lx*lx+ly*ly+lz*lz);
            if(ll>1e-5f) {
                underwaterFrame.lightDir[0]=lx/ll;
                underwaterFrame.lightDir[1]=ly/ll;
                underwaterFrame.lightDir[2]=lz/ll;
            }
        }
        underwaterFrame.sunElevation=underwaterFrame.lightDir[0]*underwaterFrame.up[0]
            +underwaterFrame.lightDir[1]*underwaterFrame.up[1]
            +underwaterFrame.lightDir[2]*underwaterFrame.up[2];
    }
    uwPublished=underwaterFrame.published?1:0;
    underwaterFrame.published=false;
}

// The wave field for the full-screen pass, built for the very camera the pass
// will reconstruct world positions against. The pass shades a bed point from its
// camera-relative world offset; the field's phases carry an anchor camera's own
// place in the lattice, so offset-plus-phase lands on an absolute world cell -
// but ONLY when the anchor is that same camera. The field used to be published
// by the last water draw (anchored to the draw's camera, possibly a frame of
// swimming away) or, on frames that drew no liquid, rebuilt here at Present for
// the frame that was ENDING and then consumed by the next frame's composite -
// which runs before this Present does. Either way the net was shaded against a
// camera it was not built for and crawled across the bed by exactly the
// difference, which is why the caustics moved with the camera. There is now
// exactly one place the field comes from, this builds it, and the composite
// calls it with the frame context it shades from. The motion clock is the same
// integrated one every draw built its field from, so the surface and the net it
// casts still move as one.
inline renderer::underwater::Field UnderwaterFieldAt(float cameraX,float cameraY,
    bool cameraValid) noexcept {
    // The same freshness rule as the pivot below: a stale anchor's phases belong to a
    // place the player has left (a teleport), and building the net from them slides it
    // across the bed by the distance between the two places until the next draw.
    const bool anchorValid=cameraValid||LastWaveAnchorFresh();
    const float anchorX=cameraValid?cameraX:lastWaveAnchor.camera[0];
    const float anchorY=cameraValid?cameraY:lastWaveAnchor.camera[1];
    const auto wave=waveMotion.Build(anchorX,anchorY,anchorValid,wavesEnabled);
    renderer::underwater::Field field;
    for(int i=0;i<4;++i)field.lattice[i]=wave.lattice[i];
    field.chopPhase[0]=wave.ripplePhase[0];
    field.chopPhase[1]=wave.ripplePhase[1];
    // The fine octave's phase: the chop phase doubled, exactly the frequency the
    // net applies to the cell, so fineSample = 2*(cell + phase) sits on the bed
    // like the base octave does. Not the surface fine ripple's 2.17/1.31 spelling:
    // under that one the two octaves of the net move at two different rates as
    // the camera moves, and the net crawls (CausticNet).
    field.finePhase[0]=wave.ripplePhase[0]*2.f;
    field.finePhase[1]=wave.ripplePhase[1]*2.f;
    field.warpPhase[0]=wave.warpPhase[0];
    field.warpPhase[1]=wave.warpPhase[1];
    field.seconds=underwaterSeconds;
    return field;
}

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
// When a failed CreatePixelShader/CreateVertexShader may be tried again. A device
// can refuse a perfectly good blob once - a cold driver JIT, a device that is
// mid-Reset, a probe sharing the same frame - and the old code latched that
// refusal until a device reset, which is exactly the report "it works after a
// reload but not before one". A cooldown retries instead, and still keeps a
// permanent refusal from recompiling on every draw.
inline std::uint64_t shaderFailureRetryAt = 0;
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
unsigned frameMatches=0,frameCandidates=0,frameSceneMatches=0,frameNearMisses=0,frameBlendOverrides=0;
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
// x=how opaque the surface is at least (WATER OPACITY, 0..1). The client's
// liquid alpha plus depth absorption is all this stage had, and in shallow,
// clear water that is nearly nothing: the bed read straight through the sea.
// This is a floor on the coverage, not a second picture of it - the column's
// own extinction still decides everything above it, so deep water and a
// reflection still converge where they did.
float4 opacityControl:register(c199);
// The client's own fog curve, exactly: x=start, y=end, z=exponent, w=usable.
// Evaluated at THIS pixel's distance instead of read off the interpolant, so a
// five-hundred-yard water tile cannot bend the curve across its own face and two
// sibling tiles cannot disagree about where the fog ends.
float4 fogCurve:register(c219);
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
// rgb=the light's colour, w=the exit-wash turbidity (renderer::underwater::ExitWash
// with its 1 removed, folded on the CPU from the submersion this draw measured):
// the lane the native diffuse left unread is where the medium's dirt reaches the
// liquid's own pixels, the ones the full-screen pass leaves to this stage.
float4 lightColor:register(c202);
// x=depth unpack A,y=depth unpack B with the viewport MaxZ folded in, so a
// captured depth costs one subtraction and one divide; z,w=screen scale.
float4 reflectionProjection:register(c203);
// x=SSR strength, y=screen-space reflection march steps, z=celestial occlusion march
// steps. Both counts come from the WATER QUALITY ladder in src/Water/WaterQuality.h and
// are trip counts, not gates: 0 skips the loop through the same test that already
// stopped it, and the loops are written against 12 so a level can hand them any count
// up to that without a second compiled shader. w=the depth fog factor on the column
// seen looking down into the water (DEPTH FOG; the capture depth range this lane once
// carried unread is folded into c203 on the CPU).
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
// xy=chop phase in cells, wrapped into the noise lattice; zw=the same-draw,
// camera-compensated phase for the broad foam warp, wrapped in that field's own
// frequency and period. Long swell A/B arrive through spare UV lanes.
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
// "no", so a level below it cannot move a shoreline); w=the camera's side of this draw's
// liquid plane in units of the wave's own lift (controls[75]). The two shortest trains get
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
// Half the baseline the bed slope below is measured over, in world yards either side of
// the pixel: the same +-1.5 yards tools/WaterPreview.cpp's FieldSlope samples the
// authored bed over, so the shoal and beach foam read the number the look was tuned
// against at any resolution, distance or depth-buffer precision.
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
    // One world-anchored lattice carries every scale. The swell is analytic - rounded
    // forms, exact slopes for free, and the same waves the vertex stage displaces, so a
    // crest and the light on it cannot drift apart - and only the chop on top of it is
    // noise. The lattice coordinate and the pixel's size in it are taken here, above the
    // sheet test, because that test is only a fact while a pixel resolves the wave it
    // sits in, which is the footprint's own question.
    float2 plane=float2(dot(worldRel.xy,waveLattice.xy),dot(worldRel.xy,waveLattice.zw));
    float2 footprint=fwidth(plane);
    // The pixel's own size in lattice cells, squared. The chop fade below reads it as a
    // fade; the shore block further down reads the SAME number, divided back through the lattice's
    // own cell size, as the pixel's footprint in world yards, which is what sizes the
    // bed-slope taps' world span so the slope does not move with the resolution or the
    // camera. Two derivatives, computed once.
    float footprintSquared=dot(footprint,footprint);
    // ONE SIDE OF THE SHEET, decided before any of the work below is paid for. The liquid is
    // drawn two-sided and writes no depth, so a ray that crosses a crest produces TWO
    // fragments for that crest: the face it enters by and the far face it would leave by. The
    // far one is not a second view of the water - it is the wave seen from inside itself,
    // carrying the same foam and the same glint as the face in front of it, and blended over
    // (or under) that face in whatever order the mesh happened to be indexed in. That second
    // sheet is the "front face of the wave visible through the back of it": raising its alpha
    // (the coverage fix at the end of this stage) cannot remove it, because alpha only says
    // how strongly the wrong sheet paints when it is drawn after the right one. It has to not
    // be drawn.
    //
    // Which side is the far one is a fact about the MESH, so it is read off the mesh: the
    // screen-space derivatives of the displaced position give this triangle's own plane (the
    // bed slope below is the same move at a yard scale, a cross of reconstructed tangents),
    // oriented to the surface's top by world up, so neither triangle winding nor a mirrored
    // reflection view can turn the
    // answer around - a cross product flips under a mirror and re-orienting by up flips it
    // back. The interpolated vertex normal cannot answer this question: it is the flat tile's
    // up vector, which knows nothing about the wave the vertex stage displaced, and the
    // shading normal cannot either, because its slope is scaled by NORMAL STRENGTH rather
    // than by the lift that is actually in the geometry.
    //
    // `wrongSide` is positive when this fragment is the far side of its own wave from where
    // the camera sits: the underside as seen from the air, or the top as seen from inside the
    // medium (which is the same statement - the part of the wave that is underwater from the
    // camera's side of it). It fades over about fifteen degrees past the silhouette instead
    // of cutting on the sign, so the boundary is the soft edge of a turning surface rather
    // than a triangle-sized notch, and it is exactly zero at the middle of the camera-side
    // band (waveSwell2.w), where the eye is in the surface and both sides are legitimately
    // in front of it. A facet standing near vertical - a sheet the up vector cannot orient -
    // fades the test out rather than guessing at it. Past the fade the fragment is discarded
    // outright, not blended at zero alpha: the discard also stops it writing the mask and
    // the surface-depth outputs, and it stops the eight trains, three noise octaves and two
    // ray marches below being paid for on a sheet nobody may see, which at a low camera is
    // near half the sea. Debug modes keep every fragment, because a debug view that hides
    // half the draws is not a debug view.
    float3 faceCross=cross(ddx(viewPos),ddy(viewPos));
    float3 faceNormal=faceCross*(dot(faceCross,upWind.xyz)<0?-1:1);
    faceNormal=safeNormalize(faceNormal);
    float faceSide=dot(faceNormal,safeNormalize(-viewPos));
    float wrongSide=lerp(-faceSide,faceSide,smoothstep(-1.0,1.0,waveSwell2.w))
        *saturate(dot(faceNormal,upWind.xyz)*4.0);
    // Only while the pixel resolves the wave. Past a few lattice cells a pixel spans
    // whole triangles of the liquid grid (some four yards, 1.6 cells at the shipped cell)
    // and at a grazing view whole wave faces, so which sheet covers the pixel centre is
    // sampling luck - and a pixel whose only fragment is a far slope would be discarded
    // into a hole with the sky and the land behind the sea showing through it, which at
    // the horizon is most of the far water. There the two sheets shade within a fog
    // colour of each other anyway and the column's coverage at the end of this stage is
    // already opaque, so the discard fades out with the footprint, on the same ground
    // the unresolved chop fades on below. Where the waves are resolved the factor is
    // exactly one and nothing about the near sea moves.
    wrongSide*=1.0-smoothstep(1.2,3.6,max(footprint.x,footprint.y));
    clip(flowControl.w<.5?.25-wrongSide:1.0);
    // Preserve the original alpha: surfaceTexture never contributes alpha.
    // uv0.zw/uv1.zw carry long-wave values; native texture addressing remains xy.
    float4 base=tex2D(baseTexture,uv0.xy);
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
    // Which side of the interface the CAMERA is on is a property of the camera, not of the
    // triangle this pixel happens to belong to, and asking a triangle for it is what made
    // the front faces of waves show through the backs of them. A facing test
    // (dot(normal,view)<0) is true of every face near edge-on, and at a low camera above
    // water the whole sea out toward the horizon is within a degree or two of edge-on, so
    // half of it took the underwater optical path while the camera stood in the air: the
    // water-to-air critical-angle mirror, the camera-column absorption over the full
    // distance to that face, and the coverage floor that goes with them - a dark sheet with
    // the crest in front of it composited on top, twice, one face through the other.
    // c218.w is the camera's own answer instead, in units of the wave's own lift: this
    // draw's liquid plane over the camera, read on the CPU from this draw's own matrices
    // and divided by the band the sea is rough enough to need (see
    // renderer::underwater::PlaneOverCamera and the controls[75] upload). It is the same
    // number for every face of the tile, it is -1 or lower in air and +1 or higher under
    // water, and it fades across the band rather than snapping, which is what a swimmer's
    // eye crossing the surface does. Above water this is now exactly zero for every pixel
    // of every tile, which is the identity the whole underwater path is documented to keep.
    float underwater=smoothstep(-1.0,1.0,waveSwell2.w);
    float flip=cameraSide<0?-1:1;
    n*=flip;
    float2 tilt=-slopes*(controls.z/jacobian)*flip;
    float3 wave=safeNormalize(n+waveTangent*tilt.x+waveBitangent*tilt.y);
    float3 l=safeNormalize(-lightDirection.xyz);
    // Taken here rather than down with the Fresnel because the optical column
    // below needs it first: from inside the medium this cosine is the ray's
    // answer to Snell's window, and the column a transmitted picture may use
    // depends on it (renderer::underwater::InterfaceYards). One dot, two readers.
    float NoV=max(dot(wave,v),1e-4);
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
    // Keep the optical column and contact refraction in view-depth units below. Foam
    // coverage is different: it belongs to the world, not to the camera ray or pixel grid.
    float openOcean=step(99999,behindZ);
    float effectiveDepth=lerp(waterDepth,9999.0,openOcean);
    // THE MURK. The original game covers what is under the surface: a stone's throw
    // out, the bottom is not visible at all, and it gets there as a gradient rather
    // than at a line. This shader had the optics of the column (transmittance, which
    // CLARITY drives and which a clear tropical shelf barely touches) but no range at
    // which the water simply hides its own floor - so the bed stayed legible to the
    // horizon and the sea read as a window rather than as water. Distance from the eye
    // is the honest variable: it is what the player judges the depth by, and unlike a
    // depth reading it cannot be missing.
    float viewDistance=length(viewPos);
    float murk=smoothstep(opacityControl.y*.22,max(opacityControl.y,.001),viewDistance)*(1-underwater);
    // The camera-to-fragment ray length, capped the way the far column is capped. Computed
    // here rather than down with the camera-column absorption because the column below now
    // needs it too, and the two have to be the same number: one is the length of water this
    // ray crosses, the other is what that length extinguishes.
    float cameraColumn=min(length(viewPos),250.0);
    // Under the surface, what is "behind" this fragment is AIR - the sky, a shore, a hull,
    // a sail - and the column measured from the surface down to the captured depth behind
    // it is a length of air being mistaken for a length of water. It is a hundred yards of
    // murk when the camera is three yards under the surface, so every upward view collapsed
    // onto the body's deep colour and there was nothing left of the world above to see
    // through it. The water this ray really travels through is the camera-to-surface
    // segment, so that is the column: looking up from just under the surface shows the sky
    // and the shore almost undimmed, and the murk grows as the swimmer goes down, which is
    // both the honest integral and the one the eye expects. Above water the lerp takes the
    // captured-depth branch it always took, scaled by DEPTH FOG: the captured bed was lit
    // by the client as though in air, so the one-way column only ever charged the light's
    // trip back UP to the camera and never the sunlight's trip DOWN to the bed - which is
    // why a submerged hillside could stand dark and crisp under water that was shading
    // every other term as a body. The factor is authored in yards of that missing leg:
    // 1 is the one-way column this stage always marched, 2 charges both legs of the path
    // (the shipped look), and past that it is simply murk a player may ask a lake for.
    // It multiplies yards, not colour, so ankle-deep water at a shoreline stays clear at
    // any setting and only depth earns the fog; the submerged branch is the medium's own
    // column, which the underwater pass already marches in full on every pixel EXCEPT
    // these ones, the liquid's own - and the pass's column is not the plain segment any
    // more (renderer::underwater::InterfaceYards): inside Snell's window the exiting
    // picture pays the turbidity wash including the near-surface layer's equivalent
    // yards (lightColor.w, folded on the CPU), and outside the window there is no
    // transmitted picture at all, so the column is the far one and the body, the
    // in-scatter and the coverage below all converge on the medium instead of on
    // whatever the client drew beyond a mirror. Letting these pixels ride the plain
    // segment while their neighbours marched the washed one was the far shore standing
    // crisp through the plane of a dirty lake. The Snell band is the Fresnel's own,
    // taken here because the column needs it first.
    float totalInternal=1-smoothstep(.63,.69,NoV);
    float interfaceYards=lerp(250.0,cameraColumn*(1.0+clamp(lightColor.w,0.0,15.0)),1.0-totalInternal);
    float column=min(lerp(effectiveDepth*reflectionControl.w,interfaceYards,underwater),400.0);
    // Reconstruct the captured seabed point in this draw's view space. screenUV is the
    // inverse projection of viewPos, so the same pixel ray at behindZ gives the actual
    // world point under this water fragment. Projecting the point-to-point vector onto
    // world up yields vertical water depth; unlike view-Z separation it does not change
    // when the camera pitches or moves.
    float3 bottomView=float3((2*screenUV.x-1)*behindZ/max(reflectionProjection.z,1e-6),
                             (1-2*screenUV.y)*behindZ/max(reflectionProjection.w,1e-6),behindZ);
    float foamDepthSigned=dot(viewPos-bottomView,upWind.xyz);
    float foamDepth=max(foamDepthSigned,0);
    float foamDepthValid=captureValid*step(behindZ,99999)*step(0,foamDepthSigned);
    // Recover the seabed's plane normal from reconstructed bed points, then measure its
    // inclination to world-up. The points stand a fixed WORLD distance apart -
    // kBedSlopeSpanYards either side of this pixel, along its own screen axes - and not
    // one pixel apart. The one-pixel stencil this used to read (ddx/ddy of bottomView)
    // divides the captured depth's own storage step by a bed footprint that is inches
    // across near a shore; the ratio is exact for a plane in exact arithmetic, but the
    // depth that reaches this stage is not exact. On hardware whose INTZ fetch hands
    // back a floating-point depth with a short mantissa instead of the 24-bit fixed
    // layout, the step is a sizeable fraction of the bed's own rise between neighbouring
    // pixels, and the normal came back as 2x2-quad speckle: over any shallow bed whose
    // true slope sits near the shoal threshold below (a sand bar, a beach's underwater
    // apron) that speckle switched the shoal term and the beach/wall split on and off a
    // quad at a time, and the foam printed it as rows of dark ticks along the depth
    // contours. Over a three-yard baseline the same storage step is a rounding error at
    // any precision the capture can produce, and the reading matches the CPU preview's
    // world-space FieldSlope the look was tuned against. The taps march along the
    // derivatives of screenUV - this pixel's own texel steps, which stay smooth whatever
    // the depth does - far enough out to span the yards (the footprint converted back
    // through the lattice's own cell size and grown by the bed's extra distance along
    // the ray), and never more than sixteen texels: by the range where that cap binds a
    // single texel is already most of the span wide on the bed. A tap that lands on sky
    // falls back to the centre point, which turns that axis into a one-sided reading
    // instead of a cliff invented out of the far sentinel.
    float2 bedStepX=float2(ddx(screenUV.x),ddx(screenUV.y));
    float2 bedStepY=float2(ddy(screenUV.x),ddy(screenUV.y));
    float bedPixelYards=max(footprint.x,footprint.y)/max(length(waveLattice.xy),1e-6)
        *clamp(behindZ/max(viewPos.z,.05),1.0,64.0);
    float bedTap=clamp(kBedSlopeSpanYards/max(bedPixelYards,1e-4),1.0,16.0);
    float2 bedUvE=screenUV+bedTap*bedStepX,bedUvW=screenUV-bedTap*bedStepX;
    float2 bedUvS=screenUV+bedTap*bedStepY,bedUvN=screenUV-bedTap*bedStepY;
    float bedRawE=tex2Dlod(sceneDepth,float4(bedUvE,0,0)).r;
    float bedRawW=tex2Dlod(sceneDepth,float4(bedUvW,0,0)).r;
    float bedRawS=tex2Dlod(sceneDepth,float4(bedUvS,0,0)).r;
    float bedRawN=tex2Dlod(sceneDepth,float4(bedUvN,0,0)).r;
    float bedZE=bedRawE>=.9999?100000:reflectionProjection.y/(bedRawE-reflectionProjection.x);
    float bedZW=bedRawW>=.9999?100000:reflectionProjection.y/(bedRawW-reflectionProjection.x);
    float bedZS=bedRawS>=.9999?100000:reflectionProjection.y/(bedRawS-reflectionProjection.x);
    float bedZN=bedRawN>=.9999?100000:reflectionProjection.y/(bedRawN-reflectionProjection.x);
    float3 bedE=float3((2*bedUvE.x-1)*bedZE/max(reflectionProjection.z,1e-6),(1-2*bedUvE.y)*bedZE/max(reflectionProjection.w,1e-6),bedZE);
    float3 bedW=float3((2*bedUvW.x-1)*bedZW/max(reflectionProjection.z,1e-6),(1-2*bedUvW.y)*bedZW/max(reflectionProjection.w,1e-6),bedZW);
    float3 bedS=float3((2*bedUvS.x-1)*bedZS/max(reflectionProjection.z,1e-6),(1-2*bedUvS.y)*bedZS/max(reflectionProjection.w,1e-6),bedZS);
    float3 bedN=float3((2*bedUvN.x-1)*bedZN/max(reflectionProjection.z,1e-6),(1-2*bedUvN.y)*bedZN/max(reflectionProjection.w,1e-6),bedZN);
    bedE=bedZE>=99999?bottomView:bedE;
    bedW=bedZW>=99999?bottomView:bedW;
    bedS=bedZS>=99999?bottomView:bedS;
    bedN=bedZN>=99999?bottomView:bedN;
    float3 bedNormal=cross(bedE-bedW,bedS-bedN);
    float bedNormalUp=dot(bedNormal,upWind.xyz);
    float bedSlope=length(bedNormal-upWind.xyz*bedNormalUp)/max(abs(bedNormalUp),1e-6);
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
    // A BEND NEEDS A COLUMN TO BEND THROUGH. The old expression floored this at a
    // quarter of full strength, so even an inch of water over a pond's floor pulled
    // the sample by 25% - and at that depth the "bed" is close enough to the eye that
    // a quarter of the offset is many pixels on screen. Anything dark under a shallow
    // sheet (a figure's shadow, a stone, a weed bed) came up twice: once where it is,
    // once where the offset put it. Refraction is proportional to the water the ray
    // crosses, so it now starts at nothing and reaches full at about fourteen yards.
    float bend=waterStyle.x*saturate(column*.07)*contact;
    float2 refractUV=screenUV+(depthValid?slopes*flowControl.yz*bend:float2(0,0));
    // Reject distortion across silhouettes, rather than pulling a bridge or a
    // character into the water. Missing depth keeps the native body.
    //
    // The old test only rejected what is IN FRONT of the water - a rail, a bridge -
    // because it compared the offset sample against the water surface's own depth.
    // It cannot see the case that actually happens: a figure, a rock or a post
    // standing IN the water is BEHIND the surface, so its own dark pixels are a
    // perfectly legal refraction sample and the stage painted them into the water a
    // few texels to the side, while the alpha blend showed the true silhouette at the
    // same 85% coverage. Two copies of one object - reported as "duplicated shadow on
    // the water". Refraction bends the BED, so the sample is kept only when it landed
    // on the same surface this pixel is already looking at: the two depths must agree
    // to within 8% (and never less than a yard and a half, so a real bed gradient at
    // range still bends). Anything nearer is an occluder in the column and the offset
    // collapses back to the unrefracted pixel.
    float refractRaw=tex2Dlod(sceneDepth,float4(refractUV,0,0)).r;
    float refractZ=refractRaw>=.9999?100000:reflectionProjection.y/(refractRaw-reflectionProjection.x);
    float sameSurface=step(abs(refractZ-behindZ),max(behindZ*.08,1.5));
    refractUV=lerp(screenUV,refractUV,
        sameSurface*step(reflectionProjection.x+reflectionProjection.y*depthScale,refractRaw));
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
    float swashDepth=max(foamDepth-sin01.x*runUp,0);
    float shallowEdge=1-smoothstep(.08,.08+max(waterStyle.w,.1)*swashWidth,swashDepth);
    float shore=foamDepthValid*shallowEdge;
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
    // Sample the broad foam field at this pixel, not from the vertex-interpolated
    // warp used to bend the swell. Linear interpolation across liquid triangles
    // exposed polygon seams in large bright foam patches, and those seams shifted
    // on screen as the camera changed the projected mesh. The same wrapped phase
    // and world lattice keep this exact sample nailed to the terrain; the existing
    // footprint filter below handles unresolved detail without triangle facets.
    float2 foamWarpCoord=float2(plane.x*.8253356+plane.y*.5646425,
                                -plane.x*.5646425+plane.y*.8253356)*.05+wavePhase.zw;
    float3 foamWarp=noiseGradient(foamWarpCoord);
    // Broad warp noise gives foam its tongue-scale shape; the already drifting
    // chop sample tears the edges into smaller flecks. The threshold breaks the stripe.
    float foamPattern=saturate(.45*foamWarp.x+.55*detail.x);
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
    float shoal=foamDepthValid*saturate(bedSlope*bodyStyle.w*2.0-.3)*saturate(1-foamDepth*.05);
    float crestMask=smoothstep(.60,.94,detail.y+length(ripple.yz)*.05*filter)
        *(.35+.65*foamPattern);
    // FOAM LACE, at WATER QUALITY 7: a mask decides WHERE foam is and says nothing about
    // what it looks like, and one scalar of coverage can only ever be a grey sheet - which is
    // what "the foam is nice but flat" was. Real foam is a lace: bright filaments with dark,
    // water-filled gaps between them, at three scales. Fine chop is the fleck and an exact
    // per-pixel sample of the broad warp is the tongue both are torn out of; the latter costs
    // one noise evaluation but avoids exposing vertex-triangle interpolation in bright foam.
    // The level-7 mid octave remains out of foam coverage. Lace multiplies crest terms whole,
    // because a whitecap IS a broken streak, and only .45 of the shore term, because a sheet
    // running up a beach is continuous in a way a cap on a crest is not.
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
        // The fleck is NOT the fine chop octave. That octave is counter-phased - plane*2.17
        // against the phase lanes swapped and scaled by 1.31 - so its coordinate at a fixed
        // world point still moves with the camera. As a wave slope that is a surface grain
        // drifting under a drifting sea; as this bright foam mask over a stable bed it made
        // the flecks crawl across shoal foam as hash. The fleck below is the absolute cell
        // (plane+wavePhase.xy, the same anchoring the base octave uses) under the integer
        // matrix [[2,1],[-1,2]] - a scaled rotation, sqrt(5) times finer than the base and
        // exact under the 64-cell wrap - riding the swell by the fine octave's own offset.
        // Its fade is its own too: sqrt(5) times the larger footprint component is fleck
        // cells per pixel, and the fleck returns to neutral between .18 and .45 of those
        // (five pixels per cell down to two), where a point sample stops being foam texture
        // and starts breaking into ticks. The broad warp keeps the slower filter fade, so
        // the tongues outlive the flecks. WaterNoise.h FoamLaceFineValue/FoamLaceFilter are
        // this same sample and fade for the preview and the anchoring regression.
        float2 laceAbsolute=plane+wavePhase.xy;
        float2 laceCell=float2(laceAbsolute.x*2+laceAbsolute.y,
                               -laceAbsolute.x+laceAbsolute.y*2)+swell*float2(.9,.47);
        float laceFade=1-smoothstep(.18,.45,2.2360679*max(footprint.x,footprint.y));
        float fine=lerp(.5,noiseGradient(laceCell).x,laceFade);
        // The mid octave is deliberately NOT a foam input. It is the level-7 value that
        // brought the visible lattice back even after the clipped saturate was removed;
        // its screen projection is exactly the grid reported at quality 7 and above.
        // Exact pixel-space broad warp prevents triangle interpolation seams; this octave
        // stays low-frequency and world-anchored, then fades only when the footprint cannot
        // resolve it.
        float broad=lerp(.5,foamWarp.x,filter);
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
        // foamDepthValid gates this like it gates the shore and shoal terms: without it a
        // pixel whose captured depth is sky (open sea running to the horizon, no bed in
        // the capture at all) clamps its foam depth to zero, reads itself as ankle-deep
        // swash, and paints backwash streaks across deep water whenever the sheet drains.
        backwash=foamDepthValid*saturate(.55+.45*sin(swashDepth*2.4-sin01.x*2.1))*(.30+.70*foamPattern)
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
    float foreverFresnel=saturate(1-NoV);
    float fresnel=(.02+.98*pow(foreverFresnel,5.0))*foreverStyle.z;
    // Water-to-air rays from below undergo total internal reflection above the critical
    // angle (cos(theta_c)=sqrt(1-(1/1.333)^2) ~= .661). A Schlick curve alone tops out
    // below one, so it incorrectly kept showing the sky/terrain through a grazing underside.
    // Smooth only across a narrow angular band to avoid a visible branch boundary;
    // totalInternal itself was taken up with the column, which needs it first.
    fresnel=max(fresnel,underwater*totalInternal*foreverStyle.z);
    // 2. Screen-space reflections against the pre-water copy, perturbed by the
    // wave normal. WATER QUALITY hands this loop its trip count - 0 skips it through
    // the same test that already stopped it, 12 is the ceiling the loop is written
    // against, 8 is what this shader always ran. Fewer steps is a coarser hit, and a
    // coarse hit on a surface this rough is indistinguishable from a sharp one once
    // the roughness mix below has spread it back toward the sky.
    // The environment a MISS reflects has to be the side of the interface the camera
    // is on. From below, the reflected ray points back down into the medium - rayUp
    // saturates at zero and `sky` collapses to the bare fog colour - so handing the
    // underside the sky term paints it whatever colour the air's horizon happens to
    // be (a warm evening made a blue lake's underside orange from below while its
    // surface stayed blue from above). What a mirrored underside with nothing in
    // reach actually shows is the water around it: the body's own asymptote, the
    // deep colour with this column's in-scatter in it, tinted the way the body is
    // tinted below. Above water the lerp is the sky it always was.
    float3 underEnvironment=(deepTint.rgb+inScatter)*waterTint.rgb;
    float3 environment=lerp(sky,underEnvironment,underwater);
    float3 reflected=environment;
    float hit=0;
    float hitDistance=0;
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
                if(hit<=0)hitDistance=traceDistance;
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
    // A hit found from BELOW is a piece of the underwater world - the bed, a cliff
    // foot, a wreck - sampled out of the pre-water copy, which is the client's own
    // in-air shading of it. The direct view of that same rock gets the medium from
    // the full-screen pass; its reflection in the underside got nothing, because
    // these are the liquid's own pixels and the pass leaves them alone - which is
    // how a mirrored underside showed the bed in its dry colours (Mulgore's orange
    // dirt) over water that was blue in every other pixel. The reflected leg is a
    // water column like any other: march the hit through the segment it travelled
    // (the surface-to-camera leg is charged once, to the whole pixel, at the end).
    float3 reflectionTransmittance=exp(-bodyStyle.x*waterStyle.y*hitDistance*absorbTint.rgb);
    reflected=lerp(reflected,reflected*reflectionTransmittance+deepTint.rgb*(1-reflectionTransmittance),underwater);
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
    // The colour an infinitely thick column converges on. It is named once here
    // because it is also what the water FOGS into: a far sheet that converges on
    // `deepTint` alone and a near one that converges on `deepTint + inScatter` are
    // two different seas meeting at one line on screen, and that line is the flat
    // "chunk over the horizon" that survived every colour change - the sea at the
    // fog's limit was not the same water as the sea in front of it.
    float3 seaBody=deepTint.rgb+inScatter;
    // Gated on the capture, exactly as before: without a depth/colour copy there is
    // no column to absorb through, and inventing one would break the promise that an
    // unmatched or uncaptured water pixel keeps the client's own colour.
    if(captureValid>.5) {
        // Refraction strength scales the BLEND, never the colour: with refraction off
        // this has to fall back to the client's own pixels, not to a black bottom.
        // WATER OPACITY hides the bed from the COLOUR as well as from the blend, and that
        // is the half that was missing. The alpha floor only decides how much of the scene
        // the sheet lets past; the body's own colour is built from `refracted`, the captured
        // seabed attenuated by the column, so at a clear shallow shelf the surface painted
        // the bed back into the frame at full coverage - the slider moved the alpha and the
        // picture did not change, which is exactly the report "opacity does nothing".
        // The captured bottom, unchanged - this is the optics the sheet is for.
        float3 bottom=refracted*transmittance+inScatter+deepTint.rgb*absorption;
        // REFRACTION is how much of that bottom replaces the client's own liquid colour.
        float3 body=lerp(rgb,bottom,depthValid*saturate(waterStyle.x));
        // WATER OPACITY then fades the WHOLE body toward the sea, and this is the line
        // that finally makes the slider do something you can see. It used to be applied
        // inside the captured-bottom term above, i.e. to the part REFRACTION scales -
        // 14% at the shipped setting - so 85% of the surface stayed the client's own pale
        // liquid colour no matter where the slider was dragged. "Opacity does nothing"
        // was exactly true: it moved a sixth of the picture. Now the surface converges on
        // the colour an infinitely thick column converges on, in proportion to the water
        // really over the pixel (so a shoreline's first yard still shows its sand).
        // ...and it may never be switched off by a depth reading that is missing,
        // implausible, or the sheet's own surface. `depthValid` is false both when
        // the copy has no depth and when the depth it has is not behind this
        // fragment; treating either as "ankle deep" turned the slider off over
        // exactly the open water it is for. An unknown bed is not a shallow one,
        // so the fade falls back to the slider's full authority. A genuine,
        // measured shallow shelf still keeps its sand.
        float bedTrusted=depthValid*step(viewPos.z+.25,behindZ);
        float bedFade=lerp(1.0,saturate(effectiveDepth*.5),bedTrusted);
        // THE SHALLOW CARVE-OUT IS A BEACH, NOT A THING STANDING IN THE WATER.
        // `bedFade` fades with the depth of the bed so a shoreline's first yard can
        // still show its sand - but an upright surface under the surface is not a
        // shallow bed, it is an object INSIDE the water: a figure, a rock, a post.
        // There `effectiveDepth` is the few inches from the surface to that object,
        // so the gate released most of the slider and the stage refracted the object's
        // own silhouette back up through the plane, distorted by the wave offset - a
        // dark second copy of whatever stands in the pond standing next to it, which
        // reads exactly as a duplicated shadow. `beach` is the slope the stage already
        // reads off the captured bed (about twelve degrees is a beach, forty is a
        // wall), so a steep occluder takes the slider's full authority instead.
        float steep=1-saturate(beach);
        bedFade=lerp(bedFade,1.0,steep*bedTrusted);
        body=lerp(body,seaBody,opacityControl.x*bedFade);
        // ...and then the water's own range closes over it, at the slider's authority
        // rather than the depth reading's: this is the original's underwater cover, and
        // it is what makes the sea read as water instead of a window onto the floor.
        body=lerp(body,seaBody,murk);
        // And the depth transition stays independent of it, so a lake with refraction
        // switched off still gets dark the further the bottom is away.
        waterBody=lerp(body,seaBody,saturate(absorption*(.85-.22*fresnel)));
    } else {
        // No scene copy at all: there is no bed to refract and no column to absorb through,
        // and the old fallthrough kept the CLIENT's own liquid colour. Over open ocean that
        // surface is a pale, nearly transparent sheet - the colour of the sky it sits under,
        // which is the "still completely transparent water" the screenshots show. Coverage is
        // already forced to 1 for this case (there is nothing behind to blend), so the only
        // honest thing for it to show is the body itself: the colour an infinitely thick
        // column converges on, which is what the covered path below is heading for anyway.
        waterBody=seaBody;
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
    // the authored deep colour as the surface gets farther away. cameraColumn was computed
    // with the optical column above, where the underwater branch of that column needs it.
    // The transmittance is only TAKEN here - it is spent once, after the reflection,
    // the local lights and the foam are in, because every one of those leaves this
    // surface toward the same submerged camera and owes the same segment. Spending
    // it on the body alone let the underside's reflection arrive as though the
    // water between the surface and the eye were air.
    float3 cameraTransmittance=exp(-bodyStyle.x*waterStyle.y*cameraColumn*absorbTint.rgb);
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
    // The camera's own segment, charged once to everything this surface sends
    // toward a submerged eye - body, reflection, glint and foam alike (see
    // cameraTransmittance above). Above water `underwater` is exactly 0 and
    // this is the identity it always was.
    float3 underwaterBody=rgb*cameraTransmittance+deepTint.rgb*(1-cameraTransmittance);
    rgb=lerp(rgb,underwaterBody,underwater);
    if(flowControl.w>1.5&&flowControl.w<2.5){result_.color=float4(saturate(pathEnergy*3).xxx,1);return result_;}
    // Two more modes, because a duplicated image on a water surface can only come from
    // two places and they need different fixes: the MIRROR (the screen-space reflection
    // sampling an occluder that stands at the surface's own depth) or the BED (the
    // refracted sample pulling a silhouette sideways). Mode 3 shows the reflection
    // term alone, mode 4 the refracted sample alone; whichever carries the copy names
    // the culprit in one screenshot instead of another round of inference.
    if(flowControl.w>3.5&&flowControl.w<4.5){result_.color=float4(saturate(refracted),1);return result_;}
    if(flowControl.w>2.5&&flowControl.w<3.5){result_.color=float4(saturate(reflected),1);return result_;}
    // Modes 5 and 6 answer the one question the shading modes cannot: is the
    // thing being looked at even a water pixel? 5 removes the plane outright -
    // the frame is exactly what the client drew, no water stage at all - and 6
    // keeps the pixels but paints the captured scene through them untouched,
    // so the water is a plain window with no colour, no fog and no foam. A
    // copy that survives 5 was never drawn here; one that survives 6 but not
    // the live frame is this stage's shading sitting on top of it.
    if(flowControl.w>5.5){result_.color=float4(tex2Dlod(sceneTexture,float4(screenUV,0,0)).rgb,1);return result_;}
    if(flowControl.w>4.5){discard;}
    if(flowControl.w>.5){result_.color=float4(1,0,1,1);return result_;}
    float baseAlpha=color.a*base.a;
    // Coverage is the column's own extinction, not a hand-tuned fraction of it. Water
    // writes no depth and blends, so whatever is behind a water face shows through by
    // exactly (1-alpha) - and behind the face of a wave is very often another face of the
    // same wave. The .78 ceiling leaked 22% of the sea behind into the sea in front at
    // every depth past the first e-fold, and the fresnel term took that to 42% at the
    // grazing angles where wave faces overlap most, which is half of the double wave a
    // player reads as seeing the front of one through the back of the next (the other half
    // is the far sheet itself, discarded at the top of this stage: coverage decides how
    // strongly a face paints, it cannot decide that a face should not have painted). 1-T is
    // the fraction of what
    // is behind that this column really extinguishes, and it is the same number the colour
    // above converged to the deep tint by, so deep water is now the deep colour rather than
    // the deep colour over a fifth of whatever was standing behind it. Shallow water is
    // untouched: there T is near one and the bottom still shows through, which is the only
    // translucency this pass owes.
    float depthAlpha=absorption;
    float surfaceAlpha=saturate(baseAlpha+(1-baseAlpha)*depthAlpha);
    // `rgb` already contains the water/reflection mix. If alpha stays at the native
    // translucent base while reflectionAmount is high, the fixed-function blend mixes
    // the captured scene a second time and exposes the opposite liquid face through this
    // one - the underwater silhouette/seam seen while crossing the interface. Keep the
    // output coverage at least as strong as the reflection we already placed in rgb.
    surfaceAlpha=max(surfaceAlpha,reflectionAmount);
    // From inside the medium, coverage is also an optics statement, and reflection
    // strength is not one: turning REFLECTIONS down authors how bright the mirror
    // is, not whether the world above may leak through it. Outside Snell's window
    // nothing is transmitted at all, so the sheet is opaque there whatever the
    // native alpha or the reflection dial say; inside it, coverage is what the
    // interface column - wash, near-surface layer and all - actually extinguishes
    // (transmittance above is that column's own), so a dirty lake fogs the shore
    // overhead out of its plane exactly as the pass fogs it out of the gaps.
    // Riding the native alpha alone left three quarters of the live scene blended
    // through a shallow sheet at grazing angles, mirror or no mirror.
    surfaceAlpha=max(surfaceAlpha,underwater*max(totalInternal,1-transmittance.g));
    // The authored floor. Applied after the optical terms and before the wrong-side
    // fade below, so a far sheet still dissolves at the silhouette instead of being
    // held opaque by the floor: the floor says how much sea a pixel is, the fade says
    // that this fragment is not one.
    //
    // A floor is the wrong answer where there is nothing BEHIND the sheet. The capture
    // writes no geometry over open water - the depth is the cleared far value, which is
    // already `openOcean` above - so the only thing a partial alpha can reveal there is
    // the clear colour the copy was never asked to replace. At 85% that is 15% of empty
    // sky blended into every open-sea pixel: a sheet of glass over the void off
    // Stormwind's southern barrier and Booty Bay's mouth, reported as "still transparent
    // where the seabed has run out". Coverage is complete there, because the sea is the
    // only thing in the column and there is no refraction it owes anyone. Under the
    // surface the rule is the opposite - `behindZ` is the sky through Snell's window, and
    // forcing coverage there would paint the window shut - so the term is above-water only.
    // A frame whose scene copy never landed has no picture behind the surface at all:
    // `captureValid` is 0, the "behind" a partial alpha would blend is whatever the
    // client left in the target, and over water that is the clear colour. Nothing
    // captured is not something to see through, so the sheet covers in full - the same
    // reasoning as open water, taken to its limit. This is the safety net behind the
    // report "the whole sea is still a sheet of glass out there": without it, a missing
    // copy silently turned every water pixel into baseAlpha over an empty background.
    float openOceanCoverage=max(openOcean*(1-underwater),1-captureValid);
    surfaceAlpha=max(surfaceAlpha,lerp(opacityControl.x,1.0,openOceanCoverage));
    // Past the visibility range there is nothing to see through, so the sheet is the
    // surface: a world that is masked in colour must not still show the raw scene
    // through 15% of alpha, which on a bright seabed is most of what a player sees.
    surfaceAlpha=max(surfaceAlpha,murk);
    // The far side of this fragment's own wave, in its fifteen degrees of fade before the
    // discard at the top of the stage removes it entirely: what survives the fade is the
    // silhouette itself turning away, not a second sheet behind the first.
    surfaceAlpha*=1-smoothstep(0.0,.25,wrongSide);
    // And the fog this sheet fades into is the medium it is in: the client's fog
    // colour is the air's, and fading a far underside into it handed the horizon
    // of the plane back to the sky's palette (the same orange the reflection
    // fallback used to supply). Under water the sheet converges on the body's
    // own asymptote instead, which is also what the non-liquid pixels around it
    // converge on, so the plane and its gaps meet at one colour at range.
    // A sea's horizon is not the sky. The light reaching the eye from a distant water
    // pixel has crossed the body, and the body's own colour is what makes the far sea read
    // as a surface instead of a hole in the world: the air fog alone painted the open sea
    // the exact colour of the sky it fades into, which is the "nothingness in the distance"
    // report. The tint is weighted by the column's own saturation, so a shallow shore
    // (where absorption is small) still fades exactly as it did, and only water that the
    // body has really swallowed takes the deep colour. Under the surface the medium is the
    // body, so that branch keeps the medium's own asymptote unchanged.
    // A fogged sea must not become the fog. At 320 yards the air's own haze is close to
    // opaque, so a target that is mostly `fogColor` erases the water into the sky colour
    // and the far sea reads as a hole - which is what survives every coverage fix. The
    // share therefore rises with view distance: near water keeps the air's colour (the
    // horizon still meets the sky), and past a few hundred yards the sheet converges on
    // the body instead, so distance deepens it rather than deleting it.
    // THE HANDOVER TO AIR. Two things have to be true at once at the horizon: the
    // sea keeps its own colour through the middle distance (a sea that turns into
    // the sky early is the "nothingness" report), and by the time the plane actually
    // stops the air has taken it over completely, so its edge is not a line against
    // the sky. So the sea's share of the target RISES with distance and then FALLS
    // back to nothing as the haze finishes, and the fog factor is driven by the same
    // distance per pixel - not by the client's per-vertex curve, which different
    // tiles of the same sea resolve differently (that is the rectangular step in the
    // middle of the water in the screenshots).
    float farSea=smoothstep(40.0,max(opacityControl.z,.001)*.6,viewDistance)*(1-underwater);
    float airShare=smoothstep(max(opacityControl.z,.001)*.6,max(opacityControl.z,.001),viewDistance)*(1-underwater);
    float seaShare=max(saturate(absorption)*(1-underwater)*.55,farSea*.85)*(1-airShare);
    // THE FOG IS THE CLIENT'S OWN CURVE, EVALUATED AT THIS PIXEL'S DISTANCE.
    // `fog` arrives interpolated from the vertex stage, and a water tile is five
    // hundred yards across: a nonlinear curve evaluated at three corners and blended
    // across the face is not the curve, and two sibling tiles that resolve it
    // differently show their shared edge - one of the ways one continuous sea came
    // out in pieces. The curve's own parameters are the ones the draw carries in c4,
    // so this is the same function the client uses, just sampled where the pixel is:
    // clamp(((end - d) / (end - start)) ^ power), which is exactly
    // min(pow(max(d*x + y, 0), z), 1) with start=(1-y)/x and end=-y/x.
    float fogSpan=max(fogCurve.y-fogCurve.x,1.0);
    float fogShape=saturate((fogCurve.y-viewDistance)/fogSpan);
    float fogPixel=pow(fogShape,clamp(fogCurve.z,.05,8.0));
    // Under the surface the client's own underwater fog is the right one and it is
    // kept exactly; above it, the pixel factor and the horizon handover decide. A
    // draw whose c4 did not hold a curve has fogCurve.w=0 and keeps the interpolant.
    float fogAir=lerp(max(fogPixel,airShare),fog,underwater);
    float fogFactor=fogCurve.w>.5?fogAir:lerp(max(fog,airShare),fog,underwater);
    // `seaBody`, not `deepTint`: the target a fully fogged sheet converges on IS the
    // body the near sheet converges on, so the fog distance stops being a visible
    // edge in the middle of one continuous plane. The remaining air share keeps the
    // horizon from becoming a hard line against the sky.
    float3 fogTarget=lerp(lerp(fogColor.rgb,seaBody,seaShare),underEnvironment,underwater);
    result_.color=float4(fogFactor*(rgb-fogTarget)+fogTarget,surfaceAlpha);
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
    depthEnabled=ReadTuning(L"DepthEnabled",1)!=0;depthFogEnabled=ReadTuning(L"DepthFogEnabled",1)!=0;foamEnabled=ReadTuning(L"FoamEnabled",1)!=0;sunGlintEnabled=ReadTuning(L"SunGlintEnabled",1)!=0;
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
    // Not "clarity": clarity is how far light reaches THROUGH the water and is
    // a property of the body. This is how much of the scene the surface is
    // allowed to leave showing, which is a property of the sheet.
    opacityPercent=float(std::clamp(ReadTuning(L"OpacityPercent",85),0,100));
    // The two distances that make the sea behave like water at range. Both are per
    // location like every other water key, and both are yards because that is the
    // unit a player can picture.
    waterVisibilityYards=float(std::clamp(ReadTuning(L"VisibilityYards",90),5,400));
    horizonFogYards=float(std::clamp(ReadTuning(L"HorizonFogYards",900),100,3000));
    depthFogStrength=std::clamp(ReadTuning(L"DepthFogPercent",200),100,400)*.01f;
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
    waterDebugMode=float(std::clamp(ReadTuning(L"WaterDebugMode",0),0,6));
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
            <<'('<<renderer::water::QualityName(waterQuality)<<')'<<" ripple="<<strength<<" normal="<<normalStrength<<" specular="<<specularStrength<<" reflection="<<reflectionStrength<<" environment="<<environmentStrength<<" distance="<<reflectionDistance<<" thickness="<<reflectionThickness<<" flow="<<flowSpeed<<','<<flowRotation<<" refract="<<refractionStrength<<" absorption="<<absorptionStrength<<" opacity="<<opacityPercent<<" foam="<<shoreFoamStrength<<','<<shoreFoamWidth<<" forever="<<swellStrength<<','<<sunGlintStrength<<','<<fresnelStrength<<','<<crestFoamStrength<<" waves="<<waveScalePercent<<','<<waveSofteningPercent<<" debug="<<waterDebugMode<<'\n';
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
    // Prefer the main camera; waves-only/no-depth frames can use a draw anchor from
    // THIS place. It chooses only the turn pivot, never the per-draw phase (reflection
    // draws retain their own camera below).
    //
    // The anchor must be fresh, not merely valid. A zone change or a teleport leaves the
    // previous zone's anchor behind - a loading screen draws nothing, and the first
    // frames of the new zone capture no camera - so "the most recent accepted draw
    // anchor" is a point thousands of yards away, in another zone, from before the
    // teleport. Every heading or WAVE SIZE edit since then is compensated about the
    // pivot, and compensating about a point in another zone shifts the field's phase by
    // the distance between the two places: the crests cross the surface at many times
    // the sea's own speed and then slow down as the heading finishes easing. With no
    // fresh pivot the lattice simply holds where it was for those frames, which is the
    // correct behaviour - an unknown camera cannot re-anchor a sea, and the travel
    // integration below (which moves the field at the sea's own speed) is unaffected.
    const bool anchorFresh=LastWaveAnchorFresh();
    const bool pivotValid=frame.cameraValid||anchorFresh;
    const float pivotX=frame.cameraValid?frame.cameraPosition.x:lastWaveAnchor.camera[0];
    const float pivotY=frame.cameraValid?frame.cameraPosition.y:lastWaveAnchor.camera[1];
    waveMotion.Advance(dt,flowSpeed*seaMotion,std::clamp(flowRotation*(1.f+seaShear),0.f,1.f),
        waveScalePercent,frame.wind,pivotX,pivotY,pivotValid,wavesEnabled);
}
void Present() {
    if(!enabled)return;
    presentSerial++;
    const auto& wind=renderer::FrameContext::Current().wind;
    const float dt=wind.sequence?wind.deltaSeconds:
        seaFrameClock.Observe(renderer::PerformanceCounter(),renderer::PerformanceFrequency());
    AdvanceSea(dt);
    // Every liquid draw of the frame that is over has reported the camera against its own
    // plane by now, so this is where the frame's answer is settled and smoothed. One frame
    // of latency is the price of asking draws instead of asking the client, and it is not
    // visible: the ramp it feeds crosses in a third of a second.
    AdvanceUnderwater(dt);
    const bool down=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
    if(hotkey&&down&&!keyDown&&renderer::FrameContext::Current().wowFocused) {active=!active;std::ofstream(std::filesystem::path(logPath),std::ios::app)<<"active="<<active<<'\n';}
    keyDown=down;
    static unsigned frames=0;
    if(++frames%600==0)
        std::ofstream(std::filesystem::path(logPath),std::ios::app)
            <<"water frame candidates="<<frameCandidates<<" replaced="<<frameMatches
            <<" active="<<active<<" enabled="<<effectEnabled<<" sceneDraws="<<frameSceneMatches
            <<" liquidLayoutsIgnored="<<frameNearMisses<<" blendOverridden="<<frameBlendOverrides
            <<" shaderFailure="<<shaderCreationFailed
            <<" sea="<<seaAgitation<<","<<seaMotion<<","<<seaShear<<" weather="<<seaWeather.kind
            <<','<<seaWeather.strength
            <<" underwater="<<submersionTracker.yards<<","<<submersionTracker.ramp
            <<(submersionTracker.submerged?" submerged":" surfaced")
            <<" clarityYards="<<underwaterFrame.medium.clarityYards
            // The diagnosis half of the line: how many liquid draws reported a plane this
            // interval and, of the ones that could not, which refusal fired - degenerate(1),
            // sheared(3), range(4), off-screen target, or no vertex height readable (still a
            // reading, from the draw's origin) - plus the last reading itself, the vertex
            // height behind it, how many readings carried a negative-determinant basis (the
            // sign an earlier version wrongly refused whole draws over), the lane the last
            // draw uploaded (c218.w), whether a frame published the medium, and whether the
            // medium it published extinguishes anything at all. With these, "nothing happens"
            // on any client says exactly where it stopped.
            <<" uwPlane="<<uwPlaneValid<<"/"<<uwPlaneRigid<<"/"<<uwPlaneRot<<"/"<<uwPlaneRange
            <<"/"<<uwPlaneOffscreen<<"/"<<uwPlaneNoVertex<<"/"<<uwPlaneFar<<"/"<<uwPlaneOutside
            <<" yards="<<uwLastYards<<" horiz="<<uwLastHorizontal<<" foot="<<uwLastFootprint
            <<" cover="<<uwLastCoverage<<" contains="<<uwLastContains
            <<" vertexZ="<<uwLastVertexZ<<" detNeg="<<uwPlaneDetNeg
            <<" lane="<<uwLastLane<<" published="<<uwPublished
            <<" mediumValid="<<(underwaterFrame.medium.valid?1:0)<<'\n';
    frameMatches=frameCandidates=frameSceneMatches=frameNearMisses=frameBlendOverrides=0;
    uwPlaneValid=uwPlaneRigid=uwPlaneRot=uwPlaneRange=uwPlaneOffscreen=uwPlaneNoVertex=uwPlaneDetNeg=uwPlaneFar=uwPlaneOutside=0;
}
inline void Reset(IDirect3DDevice9* d) {
    // Drop cached texture references before D3D9 Reset: keeping even a lookup
    // texture alive here can make the device reject reset of default-pool assets.
    renderer::water::ResetObservationCache(d);
    ResetMrtCapabilities(d);
    // The vertex-height cache holds the client's own buffers; an extra reference
    // to a default-pool buffer here would make the client's Reset fail outright.
    ClearLiquidVertexZCache();
    mainTargetSurface=nullptr;
    waterTargetDesc = {}; waterTargetDescValid = false;
    // Wind, sea state and accumulated phases survive GPU loss. They are CPU
    // state, not device resources; forgetting them would pop every crest.
    lastWaveAnchor.valid=false;
    lastWaveAnchorSeconds=-1.0;
    localLightConstantCache.valid=false;
    // The hook is invoked for the active device, including replacement
    // devices whose pointer differs from the cached shader owner. These
    // singleton-owned COM references must not survive that transition.
    cachedReplacementPS.Reset();
    cachedReplacementVS.Reset();
    cachedShaderDevice = nullptr;
    shaderCreationFailed=false;
    shaderFailureRetryAt=0;
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
// The local positions of the vertices this draw actually consumes: its
// first and last, and - when the whole draw is small enough, which a liquid
// chunk's few hundred indices always are - every emitted vertex in order,
// so the plane reading can measure the camera against the triangles the
// draw emits rather than against the rectangle two corners imply (a chunk
// enables its tiles one by one; the rectangle covers dry shore the draw
// never drew). The read itself is LocalLightManager's torch pattern, which
// has years on real clients: the declaration names the position, every
// offset is bounds-checked against the buffer's own descriptor, the locks
// are read-only - one range per buffer for the coverage read, single-vertex
// for the first/last fallback - and any failure simply means the reading
// falls back to the draw origin's plane. The plane reading answers WHERE
// the measured water is, sideways, as well as how high (PlaneOverCamera):
// a height alone cannot tell the lake the camera is in from a lake the same
// height over it somewhere else. Runs inside Scope, where the
// stream and index state is still the draw's own. Returns true when the first
// vertex was read; `haveLast` in the samples says whether the second was too.
inline UINT LiquidElementCount(D3DPRIMITIVETYPE type,UINT primitiveCount) noexcept {
    // In 64-bit, under a ceiling. Spelled in UINT, 3*primitiveCount wraps
    // past about 1.4e9 primitives - and a count wrapped small would have
    // the coverage read below swear that the first few emitted vertices
    // were the whole draw, footprint and all. No liquid draw comes within
    // orders of magnitude of the ceiling; past it the read samples the
    // first vertex only, which is the fail-safe answer it gives for a
    // primitive type it does not know.
    const UINT64 count=primitiveCount;
    UINT64 elements=0;
    switch(type) {
        case D3DPT_POINTLIST:elements=count;break;
        case D3DPT_LINELIST:elements=2*count;break;
        case D3DPT_LINESTRIP:elements=count?count+1:0;break;
        case D3DPT_TRIANGLELIST:elements=3*count;break;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN:elements=count?count+2:0;break;
        default:return 0;
    }
    return elements<=(UINT64(1)<<24)?static_cast<UINT>(elements):0;
}
inline bool LiquidCoverageTopology(D3DPRIMITIVETYPE type,
    renderer::underwater::LiquidTopology& out) noexcept {
    switch(type) {
        case D3DPT_TRIANGLELIST:out=renderer::underwater::LiquidTopology::TriangleList;return true;
        case D3DPT_TRIANGLESTRIP:out=renderer::underwater::LiquidTopology::TriangleStrip;return true;
        case D3DPT_TRIANGLEFAN:out=renderer::underwater::LiquidTopology::TriangleFan;return true;
        default:return false;
    }
}
inline bool ReadLiquidVertices(IDirect3DDevice9* d,LiquidVertexSamples& out) noexcept {
    if(!d||!nextDrawHintValid)return false;
    const bool indexed=nextDrawIndexed;
    const int baseVertexIndex=nextDrawBaseVertexIndex;
    const UINT startVertex=nextDrawStartVertex,startIndex=nextDrawStartIndex;
    const void* verticesUP=nextDrawVerticesUP;
    const void* indicesUP=nextDrawIndicesUP;
    const D3DFORMAT indexFormatUP=nextDrawIndexFormatUP;
    const UINT strideUP=nextDrawStrideUP;
    const D3DPRIMITIVETYPE primitiveType=nextDrawPrimitiveType;
    const UINT primitiveCount=nextDrawPrimitiveCount;
    const UINT consumedElements=LiquidElementCount(primitiveType,primitiveCount);
    nextDrawHintValid=false;
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    if(FAILED(d->GetVertexDeclaration(declaration.GetAddressOf()))||!declaration)return false;
    bool havePosition=false;
    UINT positionStream=0,positionOffset=0;
    if(liquidDeclarationCacheValid&&liquidDeclarationCache.Get()==declaration.Get()) {
        // The declaration this parse already answered for (liquid draws
        // share one, frame after frame): where its position lives is a
        // fact about the declaration, not about the draw.
        havePosition=liquidDeclarationHavePosition;
        positionStream=liquidDeclarationPositionStream;
        positionOffset=liquidDeclarationPositionOffset;
    } else {
        D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};
        UINT elementCount=MAXD3DDECLLENGTH+1;
        if(FAILED(declaration->GetDeclaration(elements,&elementCount)))return false;
        for(UINT i=0;i<elementCount&&i<std::size(elements);++i) {
            const auto& e=elements[i];
            if(e.Stream==0xFF)break;
            if(e.Usage==D3DDECLUSAGE_POSITION&&e.UsageIndex==0&&
               (e.Type==D3DDECLTYPE_FLOAT3||e.Type==D3DDECLTYPE_FLOAT4)) {
                havePosition=true;positionStream=e.Stream;positionOffset=e.Offset;break;
            }
        }
        liquidDeclarationCache=declaration;
        liquidDeclarationPositionStream=positionStream;
        liquidDeclarationPositionOffset=positionOffset;
        liquidDeclarationHavePosition=havePosition;
        liquidDeclarationCacheValid=true;
    }
    if(!havePosition)return false;
    const auto sanePosition=[](const float* p){
        return std::isfinite(p[0])&&std::isfinite(p[1])&&std::isfinite(p[2])
            &&std::fabs(p[0])<=20000.f&&std::fabs(p[1])<=20000.f&&std::fabs(p[2])<=20000.f;};
    renderer::underwater::LiquidTopology coverageTopology=
        renderer::underwater::LiquidTopology::TriangleList;
    const bool wantCoverage=LiquidCoverageTopology(primitiveType,coverageTopology)
        &&consumedElements>=3&&consumedElements<=kMaxLiquidCoverageElements;
    out.coverageTopology=coverageTopology;
    if(verticesUP) {
        // User-pointer draws hand the memory over directly; there is no buffer
        // descriptor to bounds-check against, so the stride itself must at
        // least contain the position being read, and every vertex number is
        // kept under a hard cap: a liquid chunk is thousands of vertices, so
        // anything past a million is a misread index array.
        if(!strideUP||UINT64(positionOffset)+3*sizeof(float)>strideUP)return false;
        const auto readOneUP=[&](UINT element,float* target)->bool {
            long long vertexIndex=startVertex;
            if(indexed) {
                if(!indicesUP)return false;
                const auto* bytes=static_cast<const unsigned char*>(indicesUP);
                UINT index=0;
                if(indexFormatUP==D3DFMT_INDEX32)std::memcpy(&index,bytes+UINT64(startIndex+element)*4,4);
                else if(indexFormatUP==D3DFMT_INDEX16){unsigned short s16=0;std::memcpy(&s16,bytes+UINT64(startIndex+element)*2,2);index=s16;}
                else return false;
                vertexIndex=static_cast<long long>(baseVertexIndex)+static_cast<long long>(index);
            } else vertexIndex+=element;
            if(vertexIndex<0||vertexIndex>=1000000)return false;
            std::memcpy(target,static_cast<const unsigned char*>(verticesUP)+
                UINT64(vertexIndex)*strideUP+positionOffset,3*sizeof(float));
            return sanePosition(target);
        };
        if(wantCoverage) {
            bool coverageOk=true;
            for(UINT element=0;element<consumedElements;++element) {
                if(!readOneUP(element,&liquidCoverageScratch[element].x)) {
                    if(!element)return false;
                    coverageOk=false;break;
                }
            }
            if(coverageOk) {
                for(int axis=0;axis<3;++axis){
                    out.first[axis]=(&liquidCoverageScratch[0].x)[axis];
                    out.last[axis]=(&liquidCoverageScratch[consumedElements-1].x)[axis];
                }
                out.haveFirst=true;out.haveLast=true;
                out.coverage=liquidCoverageScratch;
                out.coverageCount=consumedElements;
                out.haveCoverage=true;
                MeasureLiquidCoverageBounds(liquidCoverageScratch,consumedElements,
                    out.coverageMinX,out.coverageMaxX,out.coverageMinY,out.coverageMaxY);
                out.haveCoverageBounds=true;
                return true;
            }
        }
        if(!readOneUP(0,out.first))return false;
        out.haveFirst=true;
        out.haveLast=consumedElements>1&&readOneUP(consumedElements-1,out.last);
        return true;
    }
    ComPtr<IDirect3DVertexBuffer9> vertexBuffer;
    UINT streamOffset=0,stride=0;
    if(FAILED(d->GetStreamSource(positionStream,vertexBuffer.GetAddressOf(),&streamOffset,&stride))||
       !vertexBuffer||!stride)
        return false;
    ComPtr<IDirect3DIndexBuffer9> indexBuffer;
    if(indexed&&(FAILED(d->GetIndices(indexBuffer.GetAddressOf()))||!indexBuffer))return false;
    for(const auto& entry:liquidVertexZCache) {
        if(entry.valid&&entry.vertices.Get()==vertexBuffer.Get()&&
           entry.indices.Get()==indexBuffer.Get()&&entry.indexed==indexed&&
           entry.streamOffset==streamOffset&&entry.stride==stride&&
           entry.baseVertexIndex==baseVertexIndex&&entry.startIndex==startIndex&&
           entry.startVertex==startVertex&&
           entry.primitiveType==primitiveType&&entry.primitiveCount==primitiveCount&&
           entry.positionOffset==positionOffset) {
            for(int axis=0;axis<3;++axis){out.first[axis]=entry.first[axis];out.last[axis]=entry.last[axis];}
            out.haveFirst=true;out.haveLast=entry.haveLast;
            if(entry.haveCoverage) {
                out.coverage=entry.coverage;
                out.coverageCount=entry.coverageCount;
                out.haveCoverage=true;
                out.coverageMinX=entry.coverageMinX;out.coverageMaxX=entry.coverageMaxX;
                out.coverageMinY=entry.coverageMinY;out.coverageMaxY=entry.coverageMaxY;
                out.haveCoverageBounds=entry.haveCoverageBounds;
            }
            return true;
        }
    }
    D3DVERTEXBUFFER_DESC desc{};
    if(FAILED(vertexBuffer->GetDesc(&desc)))return false;
    // From here the read locks. Spend this frame's one lock budget (see the
    // serial above) or leave the chunk for a later frame. Every pool and usage
    // is read now: an earlier version skipped dynamic default-pool buffers
    // outright, reasoning that liquid chunks are static - and wherever a
    // client's chunks ARE dynamic (or merely flagged so), that skip made every
    // reading answer from the draw origin's plane forever, which is one way a
    // whole zone's water never engages the medium at all. The budget is what
    // keeps a first dive from waiting on dozens of locks; the pool was never
    // the safety, and cached answers make each chunk pay it once.
    if(liquidVertexZLockSerial==presentSerial)return false;
    liquidVertexZLockSerial=presentSerial;
    D3DINDEXBUFFER_DESC indexDesc{};
    UINT indexSize=0;
    if(indexed) {
        if(FAILED(indexBuffer->GetDesc(&indexDesc)))return false;
        indexSize=indexDesc.Format==D3DFMT_INDEX32?4u:indexDesc.Format==D3DFMT_INDEX16?2u:0u;
        if(!indexSize)return false;
    }
    LiquidVertexSamples samples;
    bool coverageRead=false;
    if(wantCoverage) {
        // The whole draw, under one range lock per buffer: the index range
        // this draw consumes, then the vertex span those indices actually
        // touch. That is two locks where first/last took four, it stays
        // inside the one-new-chunk-per-frame budget above, and the span cap
        // keeps a pathological merged buffer on the first/last fallback
        // instead of mapping a megabyte to find a shoreline.
        bool ok=stride>=positionOffset+3*sizeof(float);
        long long minVertex=0,maxVertex=0;
        if(ok&&indexed) {
            const UINT64 indexOffset=UINT64(startIndex)*indexSize;
            const UINT64 indexBytes=UINT64(consumedElements)*indexSize;
            void* locked=nullptr;
            if(indexOffset+indexBytes>indexDesc.Size||
               FAILED(indexBuffer->Lock(static_cast<UINT>(indexOffset),
                   static_cast<UINT>(indexBytes),&locked,D3DLOCK_READONLY))||!locked)
                ok=false;
            if(ok) {
                const auto* bytes=static_cast<const unsigned char*>(locked);
                for(UINT element=0;element<consumedElements;++element) {
                    UINT index=0;
                    if(indexSize==4)std::memcpy(&index,bytes+UINT64(element)*4,4);
                    else{unsigned short s16=0;std::memcpy(&s16,bytes+UINT64(element)*2,2);index=s16;}
                    const long long vertexIndex=static_cast<long long>(baseVertexIndex)
                        +static_cast<long long>(index);
                    if(vertexIndex<0){ok=false;break;}
                    liquidCoverageIndexScratch[element]=vertexIndex;
                    if(!element||vertexIndex<minVertex)minVertex=vertexIndex;
                    if(!element||vertexIndex>maxVertex)maxVertex=vertexIndex;
                }
                indexBuffer->Unlock();
            }
        } else if(ok) {
            minVertex=startVertex;
            maxVertex=static_cast<long long>(startVertex)+consumedElements-1;
            for(UINT element=0;element<consumedElements;++element)
                liquidCoverageIndexScratch[element]=minVertex+static_cast<long long>(element);
        }
        if(ok) {
            const UINT64 firstOffset=UINT64(streamOffset)+UINT64(minVertex)*stride+positionOffset;
            const UINT64 endOffset=UINT64(streamOffset)+UINT64(maxVertex)*stride
                +positionOffset+3*sizeof(float);
            void* locked=nullptr;
            if(endOffset>desc.Size||endOffset-firstOffset>(1u<<20)||
               FAILED(vertexBuffer->Lock(static_cast<UINT>(firstOffset),
                   static_cast<UINT>(endOffset-firstOffset),&locked,D3DLOCK_READONLY))||!locked)
                ok=false;
            if(ok) {
                const auto* bytes=static_cast<const unsigned char*>(locked);
                for(UINT element=0;element<consumedElements;++element) {
                    float* target=&liquidCoverageScratch[element].x;
                    std::memcpy(target,bytes+UINT64(liquidCoverageIndexScratch[element]-minVertex)*stride,
                        3*sizeof(float));
                    if(!sanePosition(target)){ok=false;break;}
                }
                vertexBuffer->Unlock();
            }
        }
        if(ok) {
            coverageRead=true;
            for(int axis=0;axis<3;++axis){
                samples.first[axis]=(&liquidCoverageScratch[0].x)[axis];
                samples.last[axis]=(&liquidCoverageScratch[consumedElements-1].x)[axis];
            }
            samples.haveFirst=true;samples.haveLast=true;
            MeasureLiquidCoverageBounds(liquidCoverageScratch,consumedElements,
                samples.coverageMinX,samples.coverageMaxX,
                samples.coverageMinY,samples.coverageMaxY);
            samples.haveCoverageBounds=true;
        }
    }
    const auto readOne=[&](UINT element,float* target)->bool {
        long long vertexIndex=startVertex;
        if(indexed) {
            const UINT64 indexOffset=UINT64(startIndex+element)*indexSize;
            if(indexOffset+indexSize>indexDesc.Size)return false;
            void* locked=nullptr;
            if(FAILED(indexBuffer->Lock(static_cast<UINT>(indexOffset),indexSize,&locked,D3DLOCK_READONLY))||!locked)
                return false;
            UINT index=0;
            if(indexSize==4)std::memcpy(&index,locked,4);
            else{unsigned short s16=0;std::memcpy(&s16,locked,2);index=s16;}
            indexBuffer->Unlock();
            vertexIndex=static_cast<long long>(baseVertexIndex)+static_cast<long long>(index);
            if(vertexIndex<0)return false;
        } else vertexIndex+=element;
        const UINT64 offset=UINT64(streamOffset)+UINT64(vertexIndex)*stride+positionOffset;
        if(offset+3*sizeof(float)>desc.Size)return false;
        void* locked=nullptr;
        if(FAILED(vertexBuffer->Lock(static_cast<UINT>(offset),3*sizeof(float),&locked,D3DLOCK_READONLY))||!locked)
            return false;
        std::memcpy(target,locked,3*sizeof(float));
        vertexBuffer->Unlock();
        return sanePosition(target);
    };
    if(!coverageRead) {
        if(!readOne(0,samples.first))return false;
        samples.haveFirst=true;
        samples.haveLast=consumedElements>1&&readOne(consumedElements-1,samples.last);
    }
    auto& entry=liquidVertexZCache[liquidVertexZNext++%64];
    // Field by field, not a whole-entry assignment: an entry now carries a
    // 48 KB coverage array, and default-constructing a temporary to clear
    // the slot would put that array on this function's stack frame.
    entry.vertices.Reset();entry.indices.Reset();
    entry.coverageCount=0;entry.haveCoverage=false;entry.haveCoverageBounds=false;
    entry.valid=false;
    entry.vertices=vertexBuffer;entry.indices=indexBuffer;
    entry.streamOffset=streamOffset;entry.stride=stride;
    entry.baseVertexIndex=baseVertexIndex;entry.startIndex=startIndex;entry.startVertex=startVertex;
    entry.primitiveType=primitiveType;entry.primitiveCount=primitiveCount;
    entry.positionOffset=positionOffset;entry.indexed=indexed;
    for(int axis=0;axis<3;++axis){entry.first[axis]=samples.first[axis];entry.last[axis]=samples.last[axis];}
    entry.haveLast=samples.haveLast;entry.valid=true;
    if(coverageRead) {
        for(UINT element=0;element<consumedElements;++element)
            entry.coverage[element]=liquidCoverageScratch[element];
        entry.coverageCount=consumedElements;
        entry.haveCoverage=true;
        entry.coverageMinX=samples.coverageMinX;entry.coverageMaxX=samples.coverageMaxX;
        entry.coverageMinY=samples.coverageMinY;entry.coverageMaxY=samples.coverageMaxY;
        entry.haveCoverageBounds=samples.haveCoverageBounds;
    }
    out=samples;
    if(coverageRead) {
        out.coverage=entry.coverage;
        out.coverageCount=entry.coverageCount;
        out.coverageTopology=coverageTopology;
        out.haveCoverage=true;
    }
    return true;
}
struct Scope {
    ComPtr<IDirect3DPixelShader9> original,replacement;
    ComPtr<IDirect3DVertexShader9> originalVertex,replacementVertex;
    IDirect3DDevice9* device=nullptr;
    // Save each touched constant span as one block. Pixel c180..c218 includes
    // local lights, the c197/c198 gap, the WATER OPACITY floor at c199, and all
    // water controls, so setup and restoration each cross the D3D9 driver once.
    float oldPixelConstants[160]{},oldVertexControls[20]{},oldVertexFog[4]{};
    bool pixelConstantsSaved=false,vertexFogFallback=false;
    ComPtr<IDirect3DBaseTexture9> oldExtraTextures[2];
    DWORD oldSampler[2][6]{};bool extraState=false;
    ComPtr<IDirect3DSurface9> oldMaskRT;bool maskBound=false;
    ComPtr<IDirect3DSurface9> oldDepthRT;bool depthBound=false;
    D3DVIEWPORT9 oldViewport{};
    DWORD oldColorWrite[2]{};
    // The four states the replacement's coverage depends on. Saved outside a
    // state block (the legacy path), restored in Restore(); with a block the
    // block's own Capture/Apply owns them.
    DWORD oldBlend[5]{};
    bool blendStateSaved=false;
    bool blendSeparateSaved=false;
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
    // A liquid draw whose native blend cannot show coverage at all. Reported once
    // per distinct combination: this is the state that decided whether any alpha
    // the water stage writes could ever be seen, so a log that names it settles
    // "the slider does nothing to the water I can see through" without a capture.
    static void ReportBlendOverride(DWORD enable,DWORD src,DWORD dst,DWORD op) {
        ++frameBlendOverrides;
        static std::unordered_set<unsigned long long> reported;
        const unsigned long long key=(static_cast<unsigned long long>(enable)<<48)|
            (static_cast<unsigned long long>(src)<<32)|
            (static_cast<unsigned long long>(dst)<<16)|static_cast<unsigned long long>(op);
        if(reported.size()>=8||!reported.insert(key).second)return;
        char line[224];
        std::snprintf(line,sizeof(line),
            "liquid draw composited with blend enable=%lu src=%lu dst=%lu op=%lu;"
            " the water stage forces an alpha blend for this draw so coverage means something",
            static_cast<unsigned long>(enable),static_cast<unsigned long>(src),
            static_cast<unsigned long>(dst),static_cast<unsigned long>(op));
        if(!logPath.empty())std::ofstream(std::filesystem::path(logPath),std::ios::app)<<line<<'\n';
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
        // A shader-creation failure skips the Observe + constant reads + all CPU
        // math for every draw until its cooldown elapses - not for the rest of the
        // session. A device that refused a blob once (a cold JIT, a Reset in flight)
        // is asked again instead of leaving native water until something re-creates
        // the device, which a player experiences as "it only works after a reload".
        if(shaderCreationFailed&&GetTickCount64()<shaderFailureRetryAt)return;
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
            if(FAILED(d->GetPixelShaderConstantF(180,oldPixelConstants,40)))return;
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
            const bool fogCurveRead=SUCCEEDED(d->GetVertexShaderConstantF(4,oldVertexFog,1));
            if(fogCurveRead&&!renderer::water::HasFogCurve(oldVertexFog)) {
                vertexFogFallback=true;
                ReportFogFallback(observation,oldVertexFog);
            }
            // Reduce this draw's fog curve to the one distance the surface needs: where
            // it has all but finished (95%). Solved once per FRAME - not per draw - so
            // every liquid draw in a frame agrees even if the client hands sibling tiles
            // slightly different curves; that disagreement was one of the ways one body of
            // water came out in pieces. A curve that does not solve to a usable distance
            // leaves the authored horizon distance in charge.
            {
                const unsigned frameIndex=renderer::FrameContext::Current().frameIndex;
                if(frameIndex!=fogCurveFrame) {
                    fogCurveFrame=frameIndex;
                    // The colour this sea fades into at range is the SAME whether the far
                    // pixels are liquid or the empty place where the liquid tiles stopped,
                    // and only this stage knows it: it is the pixel c4 the client set for
                    // this draw. Publish it for the atmosphere composite, which is the pass
                    // that owns every pixel nothing was drawn into. Without it that pass
                    // paints the void below the waterline with the SKY's haze, and the two
                    // colours meet on the water's silhouette: the band across the horizon.
                    {
                        float fogColour[4]{};
                        if(SUCCEEDED(d->GetPixelShaderConstantF(4,fogColour,1))) {
                            bool plausible=true;
                            for(int i=0;i<3;++i)
                                if(!std::isfinite(fogColour[i])||fogColour[i]<0.f||fogColour[i]>2.f)
                                    plausible=false;
                            auto& frame=renderer::FrameContext::Current();
                            if(plausible) {
                                frame.horizonWaterColour[0]=fogColour[0];
                                frame.horizonWaterColour[1]=fogColour[1];
                                frame.horizonWaterColour[2]=fogColour[2];
                                frame.horizonWaterValid=true;
                            }
                        }
                    }
                    if(fogCurveRead&&renderer::water::HasFogCurve(oldVertexFog)) {
                        const float exponent=std::max(oldVertexFog[2],.001f);
                        const float solved=(std::pow(0.95f,1.f/exponent)-oldVertexFog[1])/oldVertexFog[0];
                        if(std::isfinite(solved)&&solved>1.f) fogCurveYards=solved;
                        // The whole curve, not just a scale: start, end and the
                        // exponent, which is what lets the pixel stage evaluate the
                        // client's own function at the pixel's own distance.
                        const float endDistance=-oldVertexFog[1]/oldVertexFog[0];
                        const float startDistance=(1.f-oldVertexFog[1])/oldVertexFog[0];
                        if(std::isfinite(endDistance)&&std::isfinite(startDistance)&&
                           endDistance>startDistance+1.f&&endDistance<8000.f) {
                            fogCurveStart=startDistance;
                            fogCurveEnd=endDistance;
                            fogCurvePower=std::clamp(oldVertexFog[2],.05f,8.f);
                            fogCurveValid=true;
                        } else fogCurveValid=false;
                    }
                }
            }
            float pixelConstants[40][4];
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
            // c204.w once carried the depth range unread (it is folded into c203
            // above); it now carries DEPTH FOG - the second leg of the bed's path,
            // 1 with the toggle off, which makes the column below bit-identical.
            controls[19]=depthFogEnabled?depthFogStrength:1.f;
            // c199 (pixelConstants index 19) is inside the block this draw uploads
            // and is not declared by the client's own stage; our pixel stage reads
            // it as the opacity floor. The other three lanes are cleared so a
            // leftover from the saved client block can never leak into them.
            pixelConstants[19][0]=std::clamp(opacityPercent*.01f,0.f,1.f);
            // c199's spare lanes carry the two range parameters the same stage reads:
            // how far the bed stays visible, and where the far sea has finished
            // dissolving into the air. They ride this register because it is already
            // inside the block this draw uploads and restores - no new span, no new
            // state to keep in step with the surface shader.
            pixelConstants[19][1]=waterVisibilityYards;
            pixelConstants[19][2]=horizonFogYards;
            // The fog's own scale for this frame: the client's curve where it gave us a
            // usable one, the authored horizon distance otherwise.
            pixelConstants[19][3]=fogCurveYards>1.f?fogCurveYards:horizonFogYards;
            // c219: the whole curve, so the pixel stage can evaluate the client's own
            // fog where the pixel is rather than where the triangle's corners are.
            pixelConstants[39][0]=fogCurveValid?fogCurveStart:0.f;
            pixelConstants[39][1]=fogCurveValid?fogCurveEnd:0.f;
            pixelConstants[39][2]=fogCurveValid?fogCurvePower:1.f;
            pixelConstants[39][3]=fogCurveValid?1.f:0.f;
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
            // c5..c8 may be VIEW alone or VIEW*WORLD for a translated liquid tile.
            // In the latter case the camera extracted from modelView is tile-local and
            // cancels the tile origin when added to rel, restarting foam at every tile.
            // Query the live VIEW transform on this draw (including reflection views) and
            // use its global camera only when the rigid bases match the vertex matrix.
            D3DMATRIX liveView{};
            float viewRows[4][4]{};
            if(SUCCEEDED(d->GetTransform(D3DTS_VIEW,&liveView))) {
                std::memcpy(viewRows,&liveView,sizeof(viewRows));
                anchor=renderer::water::AnchorForWaterDraw(modelView,viewRows);
            }
            // The camera against THIS tile's liquid plane, in yards, positive under
            // it - from this draw's own matrix and one of its own vertices, nothing
            // else (renderer::underwater::PlaneOverCamera derives it, and says why
            // the live VIEW state and the determinant's sign are no longer asked).
            // Which pass the draw belongs to is read off the render target instead:
            // the client's reflection pass draws the same water under a camera
            // mirrored about the plane, and ITS reading is the player's own depth
            // with the wrong sign, so only a draw aimed at the player's own view
            // may speak. A target query that fails answers "main": treating an
            // unqueryable driver as a reflection would silence every draw, which
            // is the failure this replaced.
            bool mainTarget=true;
            {
                ComPtr<IDirect3DSurface9> currentTarget;
                if(SUCCEEDED(d->GetRenderTarget(0,currentTarget.GetAddressOf()))&&currentTarget) {
                    mainTarget=mainTargetSurface&&currentTarget.Get()==mainTargetSurface;
                    if(!mainTarget) {
                        // The backbuffer is always the player's view, published
                        // target or not; aliasing it with a reflection's target
                        // is not possible, and the alias keeps a stale published
                        // pointer (volume toggled off mid-session) from reading
                        // as "off-screen" forever.
                        ComPtr<IDirect3DSurface9> backBuffer;
                        if(SUCCEEDED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,backBuffer.GetAddressOf()))&&backBuffer)
                            mainTarget=currentTarget.Get()==backBuffer.Get();
                        else if(!mainTargetSurface)mainTarget=true;
                    }
                }
            }
            LiquidVertexSamples vertexSamples;
            const bool haveVertices=mainTarget&&ReadLiquidVertices(d,vertexSamples);
            const renderer::underwater::Vec3 firstVertex{vertexSamples.first[0],
                vertexSamples.first[1],vertexSamples.first[2]};
            const renderer::underwater::Vec3 lastVertex{vertexSamples.last[0],
                vertexSamples.last[1],vertexSamples.last[2]};
            const renderer::underwater::LiquidCoverage liquidCoverage{
                vertexSamples.coverage,vertexSamples.coverageCount,
                vertexSamples.coverageTopology,
                vertexSamples.coverageMinX,vertexSamples.coverageMaxX,
                vertexSamples.coverageMinY,vertexSamples.coverageMaxY,
                vertexSamples.haveCoverageBounds};
            const auto planeReading=mainTarget
                ?renderer::underwater::PlaneOverCamera(modelView,
                    haveVertices?&firstVertex:nullptr,
                    (haveVertices&&vertexSamples.haveLast)?&lastVertex:nullptr,
                    (haveVertices&&vertexSamples.haveCoverage)?&liquidCoverage:nullptr)
                :renderer::underwater::PlaneReading{};
            if(planeReading.valid&&planeReading.local) {
                submersionTracker.Observe(planeReading);uwPlaneValid++;
                uwLastYards=planeReading.yards;
                uwLastHorizontal=planeReading.horizontalYards;
                uwLastFootprint=planeReading.footprintYards;
                uwLastCoverage=planeReading.coverageKnown?1:0;
                uwLastContains=planeReading.containsCamera?1:0;
                if(planeReading.yards>renderer::underwater::Tracker::kExitYards
                   &&planeReading.footprintKnown&&!planeReading.containsCamera)uwPlaneOutside++;
                if(planeReading.negativeDeterminant)uwPlaneDetNeg++;
                if(haveVertices)uwLastVertexZ=vertexSamples.first[2];else uwPlaneNoVertex++;
            }
            else if(planeReading.valid) {
                // A real height, but the water it belongs to is somewhere else: a
                // raised lake in view over dry land. Counted apart so the log shows
                // the locality gate working (and how far away that water was).
                uwPlaneFar++;
                uwLastYards=planeReading.yards;
                uwLastHorizontal=planeReading.horizontalYards;
                uwLastFootprint=planeReading.footprintYards;
                uwLastCoverage=planeReading.coverageKnown?1:0;
                uwLastContains=planeReading.containsCamera?1:0;
            }
            else if(!mainTarget)uwPlaneOffscreen++;
            else if(planeReading.reason==1)uwPlaneRigid++;
            else if(planeReading.reason==3)uwPlaneRot++;
            else uwPlaneRange++;
            if(!anchor.valid) {
                // No rigid basis on this draw (an unfilled constant register, a fixture):
                // fall back to the captured camera, which is at least the right world, and
                // to the captured up axis - taken across the columns, which is the same
                // convention the vertex stage uses for world +Z in view space.
                anchor.camera[0]=waveFrame.cameraPosition.x;
                anchor.camera[1]=waveFrame.cameraPosition.y;
                anchor.valid=waveFrame.cameraValid;
                // World +Z in view space is ROW 2 of the captured view, which is the
                // convention AnchorFromModelView documents ("world axis i arrives in view
                // space as row i") and the one the renderer's world-space helpers use.
                // Reading the components across the rows instead yields the
                // view axes expressed in world coordinates - the exact mistake that
                // comment names - so this fallback used to hand the pixel stage, and the
                // medium published below, an up axis with the camera's rotation applied
                // twice over, on the rare draw with no rigid basis of its own.
                const bool viewValid=waveFrame.viewRawValid;
                anchor.up[0]=viewValid?waveFrame.viewRaw.m[2][0]:0.f;
                anchor.up[1]=viewValid?waveFrame.viewRaw.m[2][1]:0.f;
                anchor.up[2]=viewValid?waveFrame.viewRaw.m[2][2]:1.f;
            }
            if(anchor.valid){
                lastWaveAnchor=anchor;
                lastWaveAnchorSeconds=renderer::PerformanceSeconds();
            }
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
            // Pixel c210.zw were spare once the long swell moved to UV lanes. Use them
            // for the exact per-pixel foam warp; its transformed camera phase is reduced
            // in the warp's own period, never borrowed from the chop's c210.xy phase.
            controls[42]=wave.warpPhase[0];controls[43]=wave.warpPhase[1];
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
            const float clarityMultiplier=zoneWavesEnabled
                ?std::clamp(waveFrame.environment[renderer::WaterClarity],.1f,4.f):1.f;
            controls[52]=.088f*(100.f/(clarityPercent*clarityMultiplier));
            controls[53]=.25f*roughnessPercent*.01f;
            controls[54]=.02f+.06f*sheenPercent*.01f;
            controls[55]=shoalFoamPercent*.01f;
            // Tint is a multiply (0x808080 neutral) so the zone's own painted water
            // survives coloured rather than replaced; the deep colour is absolute,
            // because it is what the column converges to, and the absorption triple is
            // per-channel rate: a red-heavy value kills red first, which is why a deep
            // pool reads teal rather than as a darker grey.
            const auto colourSet=ResolveWaterColourSet(waveFrame.environment.water);
            controls[56]=colourSet.tint[0];controls[57]=colourSet.tint[1];
            controls[58]=colourSet.tint[2];controls[59]=sunDiscPercent*.004f;
            controls[60]=colourSet.deep[0];controls[61]=colourSet.deep[1];
            controls[62]=colourSet.deep[2];controls[63]=foamBrightPercent*.01f;
            controls[64]=colourSet.absorb[0];controls[65]=colourSet.absorb[1];
            controls[66]=colourSet.absorb[2];controls[67]=sparklePercent*.01f;
            // Per-zone colours (EnvironmentProfiles.ini [Zone.N]/[Area.N], resolved and
            // blended by the environment manager) sit between the two authored layers:
            // they replace the global GraphicsEffects colours above, because azure in
            // Booty Bay is the zone's look, not a user preference - but an explicit
            // per-location colour in LocationGraphics.ini is a deliberate override of
            // exactly this place, so it still wins, key by key. The resolved values
            // are already in register space (tint/absorption over 128, deep over 255),
            // so this is a copy, not a second conversion that could drift from it.
            // (The zone layer itself moved into ResolveWaterColourSet above, which is the
            // same three-way precedence it had here, so that the underwater medium and this
            // draw cannot resolve their colours differently.)
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
            // c218.w: the camera's side of this draw's liquid plane, in units of the wave's
            // own lift. Dividing by the lift is what makes the classification fade across
            // the band the sea is actually rough enough to need instead of snapping at the
            // mean plane: on a flat lake that band is a third of a yard, and on a swell
            // lifting three yards a camera floating at the surface spends seconds crossing
            // it, which is what a swimmer's eye does. -1 or below is air, +1 or above is
            // water, and everything between is the transition. When this draw cannot answer
            // (a mirrored reflection view, a fixture, a rotated tile) the tracker's held
            // value is used instead of zeroing the lane, so one unreadable draw cannot
            // blink the whole sea back to "above water" for a frame. ClassifiedYards spends
            // the tracker's own entry rule here too: a local sheet the camera is beside
            // and below the level of reads positive yards, and spending those yards raw
            // shaded that sheet as seen from under water by a camera on dry ground -
            // the full-screen gate's twin, in this stage.
            const float planeBand=std::max(renderer::underwater::kShaderBandYards,
                geometry.amplitude*renderer::water::kGroupMax);
            const float submersion=renderer::underwater::ClassifiedYards(
                planeReading,submersionTracker);
            controls[75]=std::clamp(submersion/planeBand,-2.f,2.f);
            uwLastLane=controls[75];
            // c202.w: the exit-wash turbidity for this draw's own pixels - the
            // player's [Underwater] dial as the compositor published it
            // (underwaterFrame.turbidity), scaled by this frame's zone multiplier
            // behind the same ZoneWaves gate as the clarity above, then folded by
            // ExitWash against the very submersion this draw measured. The pixel
            // stage spends it on the interface column, so the plane and the gaps
            // in the plane wash the world above out by the same dirt.
            const float turbidityMultiplier=zoneWavesEnabled
                ?(std::isfinite(waveFrame.environment[renderer::UnderwaterTurbidity])
                    ?std::clamp(waveFrame.environment[renderer::UnderwaterTurbidity],0.f,4.f):1.f)
                :1.f;
            const float effectiveTurbidity=std::clamp(
                underwaterFrame.turbidity*turbidityMultiplier,0.f,3.f);
            controls[11]=renderer::underwater::ExitWash(effectiveTurbidity,submersion,true)-1.f;
            // Publish the medium this draw was handed, for the full-screen pass that renders
            // the world the camera is inside. It reads these rather than resolving a second
            // set of colours, so a zone's authored water is the same water under the surface
            // as in it, and the line where a seabed meets the sea above it is not a line. An
            // off-screen draw is skipped: its light and up axes are the reflection camera's,
            // and its medium would be right for a camera that does not exist. The wave field
            // is NOT published (see UnderwaterFrame): the composite builds it against the
            // camera it reconstructs with, because this draw's anchor is another camera.
            if(mainTarget) {
                auto& published=underwaterFrame;
                published.medium=UnderwaterMediumForFrame(colourSet,clarityMultiplier);
                for(int i=0;i<3;++i)published.up[i]=controls[48+i];
                // c201 is uploaded pre-negated, because the pixel stage negates it to get a
                // direction pointing at the source; the pass wants that direction directly.
                const float lx=-controls[4],ly=-controls[5],lz=-controls[6];
                const float ll=std::sqrt(lx*lx+ly*ly+lz*lz);
                if(ll>1e-5f) {
                    published.lightDir[0]=lx/ll;published.lightDir[1]=ly/ll;published.lightDir[2]=lz/ll;
                }
                published.lightColour[0]=controls[8];published.lightColour[1]=controls[9];
                published.lightColour[2]=controls[10];
                // Elevation of the source over this draw's own horizon: the same dot the
                // pixel stage takes to gate its in-scatter and its sky gradient, so the light
                // under the surface is the light the surface is in.
                published.sunElevation=published.lightDir[0]*published.up[0]
                    +published.lightDir[1]*published.up[1]+published.lightDir[2]*published.up[2];
                published.published=true;
            }
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
            if(shaderCreationFailed) {
                if(GetTickCount64() < shaderFailureRetryAt) return;
                // The cooldown has elapsed: the blob and the device may both be fine
                // now, so make one more attempt rather than keeping native water for
                // the rest of the session.
                shaderCreationFailed=false;
            }
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
                    shaderFailureRetryAt=GetTickCount64()+renderer::kOptionalPassRetryMs;
                    D3DCAPS9 caps{};d->GetDeviceCaps(&caps);
                    std::ofstream(std::filesystem::path(logPath),std::ios::app)
                        <<"water shader creation failed hr="<<std::hex<<hr<<std::dec
                        <<" PS30 slots="<<caps.MaxPixelShader30InstructionSlots
                        <<"; keeping native water and retrying in "
                        <<renderer::kOptionalPassRetryMs<<" ms\n";
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
            // COVERAGE IS ONLY MEANINGFUL UNDER AN ALPHA BLEND. The whole contract
            // of the replacement pixel stage is "this fragment is this much sea":
            // `result_.color.a` is the column's extinction, and the fixed-function
            // blend is what lets the world below show through by exactly (1-a).
            // The native liquid material composites however the client authored it,
            // and a client that draws its ocean additively - or with ONE as the
            // source factor - makes alpha a no-op: the seabed stays at full strength
            // through the sheet at any coverage we write, while changing the body
            // colour still visibly replaces the client's own liquid texture. That is
            // the report "opacity changes the old texture but not how much water I
            // can see through", and no amount of tuning inside the shader could have
            // reached it. The block captured above restores these with everything
            // else; on a device without one they are saved here and written back in
            // Restore(). Only the states that decide coverage are touched.
            {
                // The four core states are saved as one unit and are the ones whose
                // restore must not be lost; SEPARATEALPHABLENDENABLE is optional in
                // D3D9 (D3DPMISCCAPS_SEPARATEALPHABLEND) and is handled separately, so
                // a device that does not know it neither fails the draw nor leaks the
                // state it did have.
                blendStateSaved=
                    SUCCEEDED(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&oldBlend[0]))&&
                    SUCCEEDED(d->GetRenderState(D3DRS_SRCBLEND,&oldBlend[1]))&&
                    SUCCEEDED(d->GetRenderState(D3DRS_DESTBLEND,&oldBlend[2]))&&
                    SUCCEEDED(d->GetRenderState(D3DRS_BLENDOP,&oldBlend[3]));
                blendSeparateSaved=SUCCEEDED(d->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,&oldBlend[4]));
                if(blendStateSaved&&
                   (oldBlend[0]!=TRUE||oldBlend[1]!=D3DBLEND_SRCALPHA||
                    oldBlend[2]!=D3DBLEND_INVSRCALPHA||oldBlend[3]!=D3DBLENDOP_ADD))
                    ReportBlendOverride(oldBlend[0],oldBlend[1],oldBlend[2],oldBlend[3]);
                const HRESULT blendHr=
                    d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
                if(SUCCEEDED(blendHr))d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
                if(SUCCEEDED(blendHr))d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
                if(SUCCEEDED(blendHr))d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
                // Best effort: alpha and colour share one blend unless the device says
                // otherwise, and asking is harmless where it is unknown.
                d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
                if(FAILED(blendHr)){Restore();ReportFailure("water blend state",blendHr);return;}
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
                // Preserve c197/c198 and the c199 opacity floor from the block
                // built above, splice in the cached lights and water controls,
                // then upload c180..c218 in one call.
                std::memcpy(pixelConstants,cache.values,sizeof(cache.values));
                if(FAILED(d->SetPixelShaderConstantF(180,pixelConstants[0],40))){Restore();return;}
            }
            static bool reported=false;
            if(!reported){std::ofstream log(std::filesystem::path(logPath),std::ios::app);log<<"matched structural water texture layout; replacement active (shader variants accepted)\n";log<<"direction="<<controls[4]<<','<<controls[5]<<','<<controls[6]<<" color="<<controls[8]<<','<<controls[9]<<','<<controls[10]<<" specular="<<specularStrength<<'\n';reported=true;}
            // Which water the client actually has, named once per session and per
            // shader family. Every "the sliders do nothing to THIS water" report is
            // answered by this line: a pond drawn by a family that is not listed here
            // is water this stage never touched, and the near-miss lines above it name
            // the identity to add. Cheap - one small set, hit once per family.
            {
                static std::unordered_set<unsigned long long> loggedFamilies;
                if(loggedFamilies.size()<32&&loggedFamilies.insert(renderer::g_trackedState.psHash).second&&!logPath.empty())
                    std::ofstream log(std::filesystem::path(logPath),std::ios::app)
                        <<"water shader family replaced: ps=0x"<<std::hex<<renderer::g_trackedState.psHash<<std::dec
                        <<"\n";
            }
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
            if(pixelConstantsSaved)device->SetPixelShaderConstantF(180,oldPixelConstants,40);
            device->SetVertexShaderConstantF(200,oldVertexControls,5);
            renderer::detail::EndPassState();
            blockState=false;
        } else {
            device->SetPixelShader(original.Get());
            device->SetVertexShader(originalVertex.Get());
            if(pixelConstantsSaved)device->SetPixelShaderConstantF(180,oldPixelConstants,40);
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
            if(blendStateSaved) {
                device->SetRenderState(D3DRS_ALPHABLENDENABLE,oldBlend[0]);
                device->SetRenderState(D3DRS_SRCBLEND,oldBlend[1]);
                device->SetRenderState(D3DRS_DESTBLEND,oldBlend[2]);
                device->SetRenderState(D3DRS_BLENDOP,oldBlend[3]);
                // The pass turns separate alpha factors off so SRCALPHA/INVSRCALPHA
                // govern the whole output; the native draw may have had them on.
                if(blendSeparateSaved)
                    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,oldBlend[4]);
            }
        }
        // c4 is outside the block's documented water spans. Restore it after either
        // state-block or manual restoration, including every early-return path.
        if(vertexFogFallback)device->SetVertexShaderConstantF(4,oldVertexFog,1);
        device=nullptr;extraState=pixelConstantsSaved=vertexFogFallback=blendStateSaved=false;
        blendSeparateSaved=false;
    }
    ~Scope(){Restore();}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
}
