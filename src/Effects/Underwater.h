#pragma once
// The world seen from inside the water.
//
// What this is
// ------------
// watereffect draws the liquid: it shades the surface, marches the water column between
// the camera and that surface, and reports - once per frame, from the matrices of the draws
// themselves - where the camera is relative to the plane those draws belong to
// (src/Water/UnderwaterMedium.h). This pass is the other half of that answer. When the
// camera is under the plane, everything the client drew between the camera and the seabed -
// terrain, a wreck, a murloc, the shore a swimmer is kicking toward - is inside the same
// medium the surface is standing in, and has to be lit through it.
//
// Why it is a full-screen pass and not more water shader
// ------------------------------------------------------
// The client's own answer is a flat underwater fog: one colour, lerped in by distance, the
// same for a pebble at your feet and a wreck at forty yards, with no red in it and no light
// in it. It is applied per object at draw time, so nothing downstream can fix it up, and the
// water stage cannot fix it either: the water pixel stage only runs on the liquid's own
// pixels, and a diver looking at the seabed is looking at pixels that are not water at all.
// The medium has to be applied to the frame, after the frame is drawn, which is where every
// other whole-frame term in this renderer lives (AO, the cloud deck, the atmosphere).
//
// What it computes, and why each term is there
// --------------------------------------------
// Per pixel, over the camera-to-point column, in exactly the order and with exactly the
// literals of renderer::underwater::ShadePixel in src/Water/UnderwaterMedium.h - which is
// the same function the water pixel stage's own bottom path computes
// (`bottom=refracted*transmittance+inScatter+deepTint.rgb*absorption`), so the seabed seen
// through the water and the water seen through itself converge on the same colour at the
// same depth instead of meeting at a line:
//
// The column itself ends where the water does. It is the distance to the point the ray ends
// at, capped at the far cap the water stage caps its own camera ray at, and then cut back to
// the liquid plane on any ray that reaches the plane first - because the segment past the
// surface is air, and counting it as water would extinguish a diver's brightest view by a
// column that is not there. Both halves state the same thing about their own pixels, though
// they read the plane from different places: the water stage bands the raw per-draw reading
// (falling back to the tracker's held yards when a frame draws no liquid), and this pass uses
// the tracker's settled yards, which is the height the ramp, the beam march and the net's
// depth fade are already built on. The two differ by at most the smoothing lag, and both sit
// inside the band the classification crosses over.
//
//   1. Beer-Lambert per channel over the column, with the SAME per-yard extinction the
//      surface uses (bodyStyle.x * waterStyle.y * absorbTint.rgb). Red dies first, which is
//      why a deep pool reads teal rather than as a darker grey.
//   2. Caustics, applied to the world BEFORE the medium eats it: a net thirty yards away is
//      a net the water has already taken the red out of. The net is the two octaves the
//      surface's own chop is made of, crossed as ridges and sharpened, normalized so its
//      mean is exactly one - caustics move light around on the bed, they do not create it.
//   3. Henyey-Greenstein in-scatter, with the light arriving through the surface carrying
//      the broad warp's own structure: the beams a swimmer sees are the patches of surface
//      the light came through, sampled where each ray meets the plane.
//   4. The sky's downwelling glow on up-looking rays, which is what keeps a pixel the client
//      left as sky - no liquid tile drawn there - reading as water overhead rather than as a
//      flat deep colour. With the column cut back to the plane, that pixel is the sky seen
//      through exactly the water over the diver's head: bright and still blue from just under
//      the surface, and from down deep almost entirely the water's own scattered light, which
//      is the honest answer and the one this term exists to give.
//   5. The column's asymptote: the authored deep colour and nothing else.
//
// Plus the one term that is not a light term: the whole frame wobbles, by at most a texel
// and a half, because a camera inside the medium is looking through a moving interface it
// cannot focus past. That cue is worth having precisely because it does not depend on
// anything being visible in the water.
//
// What it deliberately does not do
// --------------------------------
// It does not touch the liquid's own pixels. The water mask marks them, the water stage has
// already marched that column with the same coefficient, and applying the medium twice would
// be applying it twice. It does wobble them, uniformly with everything else, so the
// waterline is not a seam where the frame moves on one side and holds still on the other.
//
// Above water this pass is an exact identity: with the submersion ramp at zero the wobble is
// zero, so the sample lands on the pixel it came from, and the CPU skips the draw entirely.
// There is no branch inside the shader that can make a pixel above the surface differ from
// the pixel the compositor handed it.
//
// Known limitation, stated rather than hidden: the camera's side of the interface is read
// from the liquid draws' own matrices, so a camera underneath a liquid plane it cannot see
// through - a cave with a lake over it - reads as submerged, because there is no per-pixel
// or per-frame signal that distinguishes the two. UNDERWATER in F7 (and Enabled in
// [Underwater]) turns the whole thing off, and the tracker's camera-jump reset drops the
// state on a teleport or a zone change.
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include "../Core/DeviceHealth.h"
#include "../Core/FrameContext.h"
#include "../D3D9/ScopedPassState.h"
#include "../D3D9/ShaderBytecodeCache.h"
#include "../Environment/LocationTuning.h"
#include "../Water/UnderwaterMedium.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
    // Everything the pass needs that is not already in the FrameContext: the medium the
    // frame's water draws resolved, the wave field they published, and the light they were
    // handed. The camera's own submersion is in FrameContext (underwaterRamp/underwaterYards),
    // because the atmosphere and the cloud deck fade out on the same number, and the pass's
    // tuning is its own, from [Underwater] in GraphicsEffects.ini. Filled by the compositor
    // out of watereffect's published state, so this header has no dependency on the water
    // stage and can be included in any order.
    struct UnderwaterInputs
    {
        underwater::Medium medium;
        underwater::Field field;
        float up[3] = { 0.f, 0.f, 1.f };          // world +Z, in view space
        float lightDir[3] = { 0.f, 0.f, 1.f };    // toward the source, in view space
        float lightColour[3] = { 0.f, 0.f, 0.f };
        float sunElevation = 0.f;
        // The place's own underwater defaults, as multipliers on the player's
        // [Underwater] dials: a swamp is siltier and a tropical shallows carries
        // more net and beam than the shipped frame, without either becoming a
        // second set of user settings. 1 everywhere a zone authored nothing.
        float turbidityMultiplier = 1.f;
        float causticMultiplier = 1.f;
        float shaftMultiplier = 1.f;
        float glowMultiplier = 1.f;
    };

    namespace underwaterpass
    {
        constexpr uint64_t kSetupRetryMs = 2000;
        // Presented frames that must pass before Prewarm may ask the driver to build
        // the device shader: three seconds of login screen the player can SEE, rather
        // than a white window while an optional effect prepares itself. The bytecode
        // itself is compiled on a worker from Configure (StartPrepare), so by the
        // time this gate opens the render thread owes only CreatePixelShader.
        constexpr unsigned kPrewarmFrames = 180;
    }

    const char* kUnderwaterSource = R"HLSL(
sampler2D sceneColor:register(s0);
sampler2D sceneDepth:register(s1);
sampler2D waterMask:register(s2);
float4 projection:register(c0);     // x=A, y=B, z=P00, w=P11 (the frame's projUnpack)
float4 screen:register(c1);         // x=1/width, y=1/height, z=viewport MaxZ, w=submersion ramp
float4 extinction:register(c2);     // rgb=per-yard extinction per channel, w=yards of water overhead
float4 deepColour:register(c3);     // rgb=the body's deep colour, w=medium strength
float4 waterTint:register(c4);      // rgb=the body's tint, w=surface glow
float4 lightDirection:register(c5); // xyz=toward the source in view space, w=the source's elevation
float4 lightColor:register(c6);     // rgb=the source's colour, w=caustic amount
float4 invView0:register(c7);       // xyz=the view X axis in world yards, w=shaft amount
float4 invView1:register(c8);       // xyz=the view Y axis, w=wobble amount
float4 invView2:register(c9);       // xyz=the view Z axis, w=debug mode
float4 upSeconds:register(c10);     // xyz=world +Z in view space, w=the wrapped wobble clock
float4 lattice:register(c11);       // xy=world->lattice row 0, zw=row 1
float4 chopPhase:register(c12);     // xy=the chop octave's wrapped camera phase, zw=the fine one's
float4 warpPhase:register(c13);     // xy=the broad warp's phase, z=turbidity, w=exit-wash turbidity

// projection.y and (norm-projection.x) are both negative for every real surface - that
// quotient is what makes the view depth positive - so the guard is a floor on the magnitude,
// not on the signed value. Both helpers are the ambient occlusion stage's, letter for letter:
// one reconstruction of the depth buffer for the whole renderer, so a pixel this pass shades
// is the same point in the world the occlusion pass darkened.
float ViewZ(float raw) {
    float norm=saturate(raw/max(screen.z,1e-4));
    return projection.y/min(norm-projection.x,-1e-6);
}
float3 ViewPos(float2 uv,float z) {
    return float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*z;
}
// The rows of the inverse view are the view axes in world yards, so a view-space vector
// becomes a world-space one by scaling each row by that vector's component - the same
// spelling the atmosphere and the cloud deck use.
float3 ToWorld(float3 v) {
    return v.x*invView0.xyz+v.y*invView1.xyz+v.z*invView2.xyz;
}

// The renderer's one value-noise lattice, spelled exactly as the water's two stages spell it
// (and as src/Water/WaterNoise.h mirrors it on the CPU): the lattice index is wrapped into
// [0,64) BEFORE it is hashed, because an unwrapped index at ordinary world distances
// exhausts the float32 fraction bits and the noise collapses onto its own lattice. The
// caustic net has to sit on the surface's own chop, so it has to be the surface's own hash.
// Value only: the water stages' twin also returns the value's analytic gradient, which this
// pass never reads - it folds values into ridges - and in a shader a handful of slots from
// its budget ceiling the unread derivatives are slots the anchored dirt now needs.
float noiseValue(float2 p) {
    float2 i=floor(p),f=p-i;
    i-=floor(i*(1.0/64.0))*64.0;
    float4 h=frac((i.x+i.y*64.0+float4(0,1,64,65))*.1031);
    h*=h+33.33;
    h=frac(h*(h+h));
    float2 u=f*f*(3-2*f);
    float k=h.x-h.y-h.z+h.w;
    return h.x+(h.y-h.x)*u.x+(h.z-h.x)*u.y+k*u.x*u.y;
}

// The same noise with its CORNER indices wrapped at 64 as well. The plain form wraps the
// cell index but hashes cell 63's far corner as 64 while the cell that wrapped to 0 hashes
// the same corner as 0, so the field has a hard seam every 64 cells in each axis - invisible
// where the noise bends normals, but printed as a drawn line across the bed by a sharpened,
// five-times-gained caustic net (seen live: a giant 2x2 grid with a crossing, round 13).
// Wrapped corners make the field exactly 64-periodic, which is what the camera-phase wrap
// assumed all along. Only the net pays for this; the warp's seams sit 1280 cells apart under
// a low-contrast beam average, and the dirt sits far inside one period by an offset. The
// core spells it identically (NoiseValue64 in src/Water/UnderwaterMedium.h).
float causticNoise(float2 p) {
    float2 i=floor(p),f=p-i;
    i-=floor(i*(1.0/64.0))*64.0;
    float2 e=i+1.0;
    e-=(e>=64.0)?64.0:0.0;
    float4 ids=float4(i.x,e.x,i.x,e.x)+float4(i.y,i.y,e.y,e.y)*64.0;
    float4 h=frac(ids*.1031);
    h*=h+33.33;
    h=frac(h*(h+h));
    float2 u=f*f*(3-2*f);
    float k=h.x-h.y-h.z+h.w;
    return h.x+(h.y-h.x)*u.x+(h.z-h.x)*u.y+k*u.x*u.y;
}

// Two octaves of that lattice, each folded into a ridge and multiplied: where both nets
// cross, light that the surface bent twice lands on the bed. Both octaves sample the
// ABSOLUTE cell (the camera-relative cell plus the camera's own phase, which the
// composite builds for this very camera), the fine one at exactly twice the frequency
// with the doubled phase in chopPhase.zw - an integer frequency, because the phase wraps
// at the lattice's own period and anything fractional would pop the fine net at every
// wrap the swimmer crosses (the core derives both constraints at CausticNet; a rotated
// doubling measured five slots dearer here for identical statistics). The 2.2 exponent
// and the 5.1034 divisor are measured, not chosen (tools/UnderwaterRegression.cpp
// re-measures both): the sharpened net averages .195948 over four million samples, so
// that divisor makes the modulation's mean exactly one, and 0.0% of sampled cells land
// on a plateau - the failure mode that printed a lattice onto the foam.
float causticNet(float2 cell) {
    float a=causticNoise(cell+chopPhase.xy);
    float b=causticNoise(cell*2.0+chopPhase.zw);
    float ridgeA=1-abs(2*a-1);
    float ridgeB=1-abs(2*b-1);
    return pow(saturate(ridgeA*ridgeB),2.2);
}

// The broad, fifty-yard warp the surface bends its crests with, at one lattice cell, in the
// same rotated and scaled coordinate both of the water's stages sample it from.
float broadWarp(float2 cell) {
    float2 q=float2(dot(cell,float2(.8253356,.5646425)),
                    dot(cell,float2(-.5646425,.8253356)))*.05+warpPhase.xy;
    return noiseValue(q);
}

// Where the light reaching a point entered the water, and what the surface was doing there.
// This is the term that makes a beam a beam instead of a noise pattern on the fog: the light
// arriving at this point came through THAT patch of surface, so a bright patch of surface
// makes a bright column under it and the columns lean with the sun. Three samples along the
// ray, one octave each - a beam is a broad feature, and three samples of the broad warp cost
// what one sample of the crest family would.
float shaftDensity(float3 world,float3 lightWorld) {
    // The entry point is affine in the march parameter (entry(t) = world*t +
    // L*((W-world.z*t)/rise)), so its lattice cell is a lerp of the two ends' cells -
    // one entry evaluation per end instead of one per tap, the same numbers to float
    // rounding, and about a dozen slots back for the net's seamless noise above. The core
    // spells it identically (ShaftDensity in src/Water/UnderwaterMedium.h).
    float rise=max(lightWorld.z,.08);
    float3 entry0=lightWorld*(extinction.w/rise);
    float3 entry1=world+lightWorld*((extinction.w-world.z)/rise);
    float2 cell0=float2(dot(entry0.xy,lattice.xy),dot(entry0.xy,lattice.zw));
    float2 cell1=float2(dot(entry1.xy,lattice.xy),dot(entry1.xy,lattice.zw));
    float total=0;
    [unroll] for(int sample=1;sample<=3;++sample) {
        total+=broadWarp(lerp(cell0,cell1,float(sample)/3.0));
    }
    // Measured against the octave's own spread: one sample of (value-.5)*2 has a standard
    // deviation of .429 and three along a ray about .25, so this gain puts a beam's peak
    // near one and its duty cycle near a fifth of the frame. The centring is done once
    // over the mean (4.4 is the 2 of the centring folded into the 2.2 gain), which is six
    // slots this stage is too close to the 512 floor to spend per sample.
    return saturate((total/3.0-.5)*4.4);
}

float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float ramp=saturate(screen.w);
    // The eye's own wobble, applied to the sample rather than to the result, so the depth and
    // the colour move together and nothing shears. Uniform across the frame and independent
    // of any surface: it belongs to the eye, not to the world. Zero at ramp zero, which is
    // what makes the pass above water an exact identity rather than a cheap one.
    float peakTexels=1.6;   // kMaxDistortionTexels: the texel budget at full distortion
    float sinePeak=1.6;     // kWobblePeak: what the two-term sine sum below peaks at
    float wobble=saturate(invView1.w)*peakTexels*ramp/sinePeak;
    float clock=upSeconds.w;
    float2 sampleUv=uv+float2(sin(uv.y*38.0+clock*1.7)+.6*sin(uv.y*17.0-clock*1.1),
                              sin(uv.x*34.0-clock*1.3)+.6*sin(uv.x*15.0+clock*.9))*wobble*screen.xy;
    float4 scene=tex2D(sceneColor,sampleUv);
    // No [branch] on any test below that reaches a tex2D, and that is not an oversight: fxc
    // rejects a forced dynamic branch whose body samples with an implicit LOD outright on
    // ps_3_0 - X3528, "can't force branch with gradients on non-inputs" - because derivatives
    // are undefined inside flow control. VolumeEffects.h documents the same lesson for its
    // anti-aliasing taps. A plain if costs nothing here: the first two conditions are shader
    // constants or single fetches, and the compiler is free to branch on them itself. The only
    // two [branch] attributes this stage keeps are on shader constants wrapping bodies that
    // are pure arithmetic, which is where forcing the branch actually saves work.
    if(ramp<=0.0) return scene;
    // The liquid owns its own pixels: the water stage marched that column with this same
    // coefficient and added this same in-scatter, so shading them here would apply the
    // medium twice. The mask's red channel is "a water tile was drawn here", which is the
    // same test the occlusion and atmosphere passes use.
    if(tex2D(waterMask,sampleUv).r>.25) return scene;

    float raw=tex2D(sceneDepth,sampleUv).r;
    float strength=saturate(deepColour.w)*ramp;
    float3 viewRay;
    float distance;
    float3 world;
    float facing=0;
    if(raw>=.9999) {
        // Nothing was drawn here, so there is no surface to shade and no distance to
        // measure. The column starts at the same far cap the water stage caps its own at and
        // is then cut back to the plane by step 0 below, so an opening in the liquid - a gap
        // between tiles, a surface past the client's own draw distance - reads as sky seen
        // through exactly the water that is over the diver's head: bright and tinted from
        // just under the surface, murky from down deep, and never the flat 250-yard column a
        // cap on its own would give it.
        viewRay=normalize(ViewPos(sampleUv,1.0));
        distance=250.0;
    } else {
        float z=max(ViewZ(raw),1e-3);
        float3 p=ViewPos(sampleUv,z);
        distance=length(p);
        viewRay=p/max(distance,1e-4);
        // Depth-derived normal, the occlusion stage's way: on each axis keep the neighbour
        // whose depth is continuous with this pixel, so a silhouette never tilts the normal
        // and a wreck's edge does not grow a caustic net down its side.
        float zl=ViewZ(tex2D(sceneDepth,sampleUv-float2(screen.x,0)).r);
        float zr=ViewZ(tex2D(sceneDepth,sampleUv+float2(screen.x,0)).r);
        float zd=ViewZ(tex2D(sceneDepth,sampleUv-float2(0,screen.y)).r);
        float zu=ViewZ(tex2D(sceneDepth,sampleUv+float2(0,screen.y)).r);
        float3 px=abs(zl-z)<abs(zr-z)?ViewPos(sampleUv-float2(screen.x,0),zl):ViewPos(sampleUv+float2(screen.x,0),zr);
        float3 py=abs(zd-z)<abs(zu-z)?ViewPos(sampleUv-float2(0,screen.y),zd):ViewPos(sampleUv+float2(0,screen.y),zu);
        float3 n=normalize(cross(px-p,py-p));
        if(dot(n,p)>0) n=-n;
        // Only an upward-facing bed catches the net: dot with world +Z expressed in view
        // space, which is the same dot the water stage takes for its sky gradient.
        facing=saturate(dot(n,upSeconds.xyz)*2.0-.2);
    }

    // 0. The medium ends at the surface. A ray looking up reaches the liquid plane before it
    // reaches whatever the client drew beyond it - sky through an opening, a sail, a cliff face
    // the water was never tiled over - and the segment past the plane is AIR, so counting it as
    // water extinguishes the diver's brightest view by a column that is not there. extinction.w
    // is the plane's height over the camera, from the same tracker the water stage classifies
    // with, and this is the same statement that stage makes about its own pixels
    // (column=min(lerp(effectiveDepth,interfaceYards,underwater),400.0)): the water a submerged
    // ray crosses is the camera-to-surface segment, and no more. Downward and level rays never
    // reach the plane, and a grazing upward ray meets it so far away that min() picks the real
    // distance, so both are untouched.
    float rayUp=dot(viewRay,upSeconds.xyz);
    // Written as a select rather than as a guarded assignment so that the divide is safe even
    // if the compiler evaluates both sides: a level ray would divide by zero and a downward one
    // would come out negative, and neither may reach min().
    float surfaceHit=(rayUp>1e-3&&extinction.w>0.0)?extinction.w/max(rayUp,1e-3):distance;
    float exited=surfaceHit<distance?1.0:0.0;
    distance=min(distance,surfaceHit);
    // The point the net and the beam march read is the end of the WATER column, not the end of
    // the ray: for a pixel the client left as sky that is where the ray leaves the liquid, and
    // marching a beam from a point in the air above the surface would march it from nowhere.
    world=ToWorld(viewRay)*distance;

    // 0b. Water is not uniformly clear: silt hangs in the column, so murk grows with the
    // water over the shaded point, and it arrives in patches about a hundred yards across
    // rather than everywhere at once. Both are meant to be properties of the place - the
    // world coordinate and the depth - not of the camera, so swimming through a patch
    // changes the water the way swimming into a shadow changes the light. warpPhase.z is
    // TURBIDITY; the patch field is the renderer's one noise lattice at a fortieth of the
    // caustic frequency, and the product scales the column's exponent, which is scaling
    // the extinction per yard. Known debt, stated plainly: this sample is camera-relative
    // where the net and the shafts are world-anchored, so the patches breathe slowly with
    // the swimmer. The anchored form (the broad warp at the bed point) was built and
    // measured at 522 of this stage's 512 slots; a phase of the patch's own would need a
    // constant lane the stage does not have. The core spells it identically (step 0b of
    // ShadePixel), and TODO.md carries the item.
    float under=extinction.w-world.z;
    // Offset by a non-integer number of patch cells: unshifted, this camera-relative
    // field keeps a lattice VERTEX at the point directly under the camera forever, and
    // the four cells meeting there interpolate as four flat wedges pinned to the
    // swimmer's feet - one big 2x2 cross in the middle of every downward view (round 13).
    // The offset parks the nearest vertex about fifty yards away and keeps the visible
    // window far inside one wrap period, so the old noise's wrap seam cannot enter either.
    float2 dirtCell=float2(dot(world.xy,lattice.xy),dot(world.xy,lattice.zw))*(1.0/40.0)+float2(37.31,57.17);
    float silt=(.65+.7*noiseValue(dirtCell))*(1.0+.9*saturate(under/18.0));
    float dirt=max(1.0+warpPhase.z*(silt-1.0),0.0);

    // 0c. Snell's window: a ray cut back to the plane only leaves the water inside the
    // critical cone (cos .6613); outside it the surface is a mirror from below and what the
    // client drew beyond it - horizon land, sky past the draw distance - cannot arrive. Such
    // a ray is marched as though it never leaves: its effective column goes to the far cap
    // and the in-scatter and the asymptote below fill it. INSIDE the window there is one more
    // truth to honour: the client drew a crisp sky as though through a glass pane, but in
    // real water the IMAGE of the world above dies faster than its LIGHT - scattering
    // scrambles direction as well as dimming, so past a turbid column what survives is the
    // downwelling glow (steps 3-4), not a readable picture of clouds. Exiting rays therefore
    // pay an extra wash of the water overhead scaled by turbidity: warpPhase.w is that wash's
    // turbidity with the near-surface layer's equivalent yards already folded in on the CPU
    // (renderer::underwater::ExitWash), so a shallow swimmer in dirty water trades the view
    // for the lamp too, while crystal water at turbidity 0 keeps its pane. Rays that end
    // under water are untouched. The core: step 0c of ShadePixel.
    float exitWindow=lerp(1.0,smoothstep(.6013,.7213,rayUp),exited);
    float exitWash=1.0+warpPhase.w*exited;

    // 1. What survives of the world between here and the camera. STRENGTH interpolates the
    // transmittance's effect rather than the coefficient, so 0 is exactly the frame this
    // pass was handed and 1 is the column the surface would have marched.
    float3 column=exp(-extinction.xyz*min(lerp(250.0,distance*dirt*exitWash,exitWindow),250.0));
    float3 transmittance=lerp(float3(1,1,1),column,strength);
    float absorption=1-transmittance.y;
    float sunGate=saturate(lightDirection.w*2.5+.2);
    float3 colour=scene.rgb*transmittance;

    // 2. Caustics on the bed, before the medium eats them, at the same strength the
    // transmittance is scaled by - at STRENGTH 0 the modulation is exactly 1, because "no
    // medium" is the frame this pass was handed and not that frame with a net drawn on it.
    // The fade is the physics of it:
    // the net needs water over it to focus through and dies as that water deepens and
    // blurs it out, so it is a shallow-water phenomenon and stays one.
    float caustic=0;
    float modulation=1.0;
    // The gate is uniform: the authored amount, the source's elevation and the frame's strength
    // are all constants for this draw, so a whole tile takes the branch together and none of the
    // work below is done at all when caustics are off, when the sun is under the horizon or when
    // STRENGTH is 0 - the same shape as the water stage's own quality gates, which are all
    // comparisons against a constant register for exactly this reason.
    float causticScale=saturate(lightColor.w)*sunGate*strength;
    [branch] if(causticScale>0.0) {
        float depthFade=saturate(under*1.5)*saturate(1.0-under/28.0);
        caustic=causticScale*facing*depthFade;
        // Skipped, not multiplied by zero: lerp(1,net*N,0) is exactly 1 for a bounded net, so
        // this gate changes no pixel and saves the two noise evaluations on every wall, every
        // point past the net's reach, every frame with no medium and every pixel of sky -
        // which is most of most underwater frames. A [branch] is legal around it because
        // nothing inside samples with an implicit LOD (see the X3528 note above).
        [branch] if(caustic>0.0) {
            float2 cell=float2(dot(world.xy,lattice.xy),dot(world.xy,lattice.zw));
            modulation=lerp(1.0,causticNet(cell)*5.1034,caustic);
        }
    }
    colour*=modulation;

    // 3. Light scattered back out of the column toward the camera, on the same
    // Henyey-Greenstein lobe, the same .17 normalization and the same 1+2.2 backlit shape
    // the water stage gives its crest, so the medium and the surface agree about which side
    // of the water the source is on.
    float vDotL=-dot(viewRay,lightDirection.xyz);
    float g=.3+.34*saturate(lightDirection.w);
    float phaseDen=max(1+g*g+2*g*vDotL,1e-3);
    float lobe=(1-g*g)*.17/(phaseDen*sqrt(phaseDen))*sunGate;
    float shaft=0;
    [branch] if(invView0.w>0.0&&lightDirection.w>0.0) {
        shaft=shaftDensity(world,ToWorld(lightDirection.xyz))
            *saturate(invView0.w)*saturate(-vDotL*1.4-.25);
    }
    float3 scatter=(deepColour.rgb*1.7+waterTint.rgb*.12)*lightColor.rgb;
    float3 inScatter=scatter*(absorption*lobe*(1+2.2*shaft)*strength);
    // 4. And the whole sky over the surface, on up-looking rays: the body's own colour
    // lifted by its WaterColour, never by a fixed blue (SkyGlowColour in the core) - a
    // fixed lift kept the glow overhead blue in water that is red on the surface.
    float upLook=saturate(dot(viewRay,upSeconds.xyz));
    inScatter+=(deepColour.rgb*1.6+waterTint.rgb*.18)*lightColor.rgb
        *(absorption*saturate(waterTint.w)*upLook*sunGate*strength);
    colour+=inScatter;
    // 5. The column's own asymptote.
    colour+=deepColour.rgb*(absorption*strength);

    if(invView2.w>3.5) return float4(saturate(modulation).xxx,1);
    if(invView2.w>2.5) return float4(saturate(absorption).xxx,1);
    if(invView2.w>1.5) return float4(inScatter,1);
    if(invView2.w>.5) return float4(saturate(caustic),saturate(shaft),saturate(upLook),1);
    // The last line of defence, and the reason this pass can be trusted with the finished
    // frame: a NaN compares false against everything, so `x*x<1e30` is false for a NaN and
    // for an infinity alike, and the frame this pass was handed comes back untouched instead
    // of one bad constant poisoning every pixel of it. The CPU validates the same values
    // before it uploads them (Finite, and the guards in Render), so this should be
    // unreachable; it is here because "should" is not a property of a driver.
    float finiteCheck=dot(colour,colour);
    if(!(finiteCheck<1e30)) return scene;
    return float4(colour,scene.a);
}
)HLSL";

    // The medium the camera is inside, drawn over the finished world. One ps_3_0 pass, one
    // full-screen quad, no history and no intermediate target: it reads the compositor's
    // scene copy and writes the mixed frame back into the target with blending off.
    class Underwater
    {
    public:
        static Underwater& Instance()
        {
            static Underwater instance;
            return instance;
        }

        void Configure(const std::wstring& basePath)
        {
            const std::wstring ini = basePath + L"GraphicsEffects.ini";
            m_logPath = basePath + L"Underwater.log";
            m_shaderCacheDir = shaderbytecode::Dir(basePath);
            // Fingerprint the raw inputs so a location crossing that resolves the same values
            // skips the re-apply and its log write, exactly as the other passes do.
            locationtuning::SettingsFingerprint fingerprint;
            auto read = [&](const wchar_t* key, int fallback) -> int {
                const int value = int(locationtuning::ReadInt(L"Underwater", key, fallback, ini.c_str()));
                locationtuning::MixInt(fingerprint, value);
                return value;
            };
            underwater::Settings settings;
            settings.enabled = read(L"Enabled", 1) != 0;
            settings.strength = std::clamp(read(L"StrengthPercent", 100), 0, 100) * .01f;
            settings.caustics = std::clamp(read(L"CausticPercent", 55), 0, 100) * .01f;
            settings.shafts = std::clamp(read(L"ShaftPercent", 60), 0, 100) * .01f;
            settings.distortion = std::clamp(read(L"WobblePercent", 50), 0, 100) * .01f;
            settings.surfaceGlow = std::clamp(read(L"SurfaceGlowPercent", 55), 0, 100) * .01f;
            // Not a clarity key: the zone still resolves the medium, and this multiplies it -
            // the silt and the patches scale with it, and at 100 the water is exactly the
            // water the zone authored. It is the dirt knob, for a server whose lakes are
            // quarries and a player who wants them to look it.
            settings.turbidity = std::clamp(read(L"TurbidityPercent", 100), 0, 300) * .01f;
            const int debugMode = std::clamp(read(L"DebugMode", 0), 0, 4);
            settings.debug = debugMode != 0;
            m_debugMode = float(debugMode);
            // There is deliberately no clarity key here. The medium's clarity IS the water's
            // own WATER CLARITY resolved per zone (UnderwaterMediumForFrame in WaterEffect.h),
            // because a diver and the surface above the diver are in one body of water: a
            // second number would let the two resolve their own murk and the seam where a
            // seabed meets the sea above it would be a line a player could put a finger on.
            // What this section authors is the light inside that medium, and the switch.
            m_settings = settings;
            // The bytecode is prepared here, on a worker, the moment the pass is
            // configured on: fxc is a standalone CPU pass that needs no device, and
            // the render thread - which at this point in a launch has not yet
            // presented the login screen - must never be the thread that pays it.
            if (IsActive()) StartPrepare();
            if (locationtuning::SameSettings(m_settingsFingerprint, fingerprint)) return;
            char message[160];
            sprintf_s(message, sizeof(message),
                      "tuning enabled=%d strength=%.2f caustics=%.2f shafts=%.2f "
                      "wobble=%.2f glow=%.2f debug=%.0f",
                      settings.enabled ? 1 : 0, settings.strength, settings.caustics,
                      settings.shafts, settings.distortion, settings.surfaceGlow, m_debugMode);
            Log(message);
        }

        void Reset(IDirect3DDevice9*)
        {
            m_shader.Reset();
            m_setupRetry.Succeed();
            m_errorLogged = false;
            m_prewarmFrames = 0;
        }

        // Builds the DEVICE shader at a moment when the cost is invisible. The
        // compositor calls this on the frames it prepares anyway, long before anyone
        // goes under - but the first of those frames is the first frame of the
        // process, and this used to run a full compiler pass plus the driver's own
        // JIT inside it, synchronously, before the client had cleared or drawn
        // anything: a white login window with the audio already running, for as
        // long as fxc and the driver between them took, on exactly the launches
        // whose bytecode cache was cold (every new build's first launch), and a
        // restart "fixed" it because the cache it left behind was warm. Neither
        // half of that cost belongs in a frame any more: the bytecode is compiled
        // on a worker from Configure (StartPrepare), and CreatePixelShader - the
        // one part that needs the device - waits here until the game has actually
        // been presenting frames for a few seconds. A diver who goes under before
        // the gate opens gets the shader from Render instead, from bytecode the
        // worker has had the whole login to finish. Failure handling is Render's
        // own gate, so a driver that refuses the shader retries at the same
        // bounded rate instead of compiling every frame.
        void Prewarm(IDirect3DDevice9* device)
        {
            if (!IsActive() || !device || m_shader) return;
            if (m_prewarmFrames < underwaterpass::kPrewarmFrames) { ++m_prewarmFrames; return; }
            if (!m_setupRetry.Open(GetTickCount64())) return;
            if (!EnsureShader(device) && !ShaderPending()) NoteFailure("Underwater shader failed");
        }

        // True when the pass should run. The compositor consults this for HasActiveEffects,
        // which is what allocates the water mask this pass reads: without that, a diver with
        // every other effect switched off would have no mask to skip the liquid's own pixels
        // with, and the surface would be shaded twice.
        bool IsActive() const
        {
            return m_settings.enabled && m_settings.strength > 0.f;
        }

        const underwater::Settings& Settings() const { return m_settings; }
        float DebugMode() const { return m_debugMode; }

        // Shades `targetSurface` in place, reading the frame from `frameContext.sceneColor`
        // (which the compositor publishes as a copy of that same target) and writing the
        // mixed frame back with blending off. Returns true when the pass drew, so the caller
        // refreshes the scene copy every downstream pass samples.
        bool Render(IDirect3DDevice9* device, const FrameContext& frameContext,
                    IDirect3DSurface9* targetSurface, IDirect3DTexture9* depthTexture,
                    const UnderwaterInputs& inputs)
        {
            if (!IsActive()) { NoteIdle(1, "the pass is not active (Enabled, configuration or shader)"); return false; }
            if (!device || !targetSurface) { NoteIdle(1, "no device or target surface"); return false; }
            if (!depthTexture) { NoteIdle(1, "no captured depth texture - depth capture is off (MSAA refuses INTZ, or the capture never latched)"); return false; }
            const float ramp = std::clamp(frameContext.underwaterRamp, 0.f, 1.f);
            // Not submerged: the pass would return the frame it was handed, pixel for pixel,
            // so it does not run at all. This is the identity the whole underwater path is
            // documented to keep above the surface.
            if (ramp <= 0.f || !inputs.medium.valid)
            {
                NoteIdle(2, "ramp " + std::to_string(ramp) + ", medium valid "
                    + std::to_string(inputs.medium.valid ? 1 : 0)
                    + " - the tracker does not put the camera in the medium; the water log's uwPlane counters say which refusal the draws report");
                return false;
            }
            if (!frameContext.sceneColor) { NoteIdle(3, "no scene copy published by the compositor"); return false; }
            if (!m_setupRetry.Open(GetTickCount64())) return false;

            // Reconstruction has to be finite this frame or the pass cannot tell a pebble at
            // the diver's feet from the far plane.
            const float* unpack = frameContext.projUnpack;
            if (!std::isfinite(unpack[0]) || !std::isfinite(unpack[1]) || unpack[1] == 0.f ||
                unpack[2] <= 0.f || unpack[3] <= 0.f || frameContext.depthMaxZ <= 0.f)
            { NoteIdle(5, "the projection unpack pair or the far Z is not usable this frame"); return false; }
            if (!frameContext.viewRawValid) { NoteIdle(6, "no raw view matrix captured this frame"); return false; }
            // `cameraValid` is the flag the camera capture publishes the inverse view under,
            // along with projUnpack, depthMaxZ and the camera position this pass reconstructs
            // from, so it is the flag that says those three rows are this frame's. A frame
            // whose draws carried no fresh eye - looking up at the surface from under it, a
            // wall or a menu with no terrain in view - leaves it false while the retained rows
            // are a frame or two old, and refusing the pass on those frames is what made the
            // medium vanish at camera ANGLES rather than at the surface: a diver who turned to
            // look up lost the fog, the caustics, the beams and the grade until some terrain
            // draw re-published the pose. Hold the retained pose for the same short window the
            // lamps use, and refuse it past that: a stale camera is a wrong world position,
            // a second-old one is the swimmer's own movement.
            const bool poseRetained = !frameContext.cameraValid && frameContext.viewRawValid &&
                frameContext.cameraEyeAgeMs <= FrameContext::kCameraEyeHoldMs;
            if (!frameContext.cameraValid && !poseRetained) { NoteIdle(7, "cameraValid is false and the retained pose is past its hold - the inverse view and the camera position are not this frame's"); return false; }
            // The rows of the inverse view turn a view-space ray into world yards, and every
            // world-space term reads the result: the caustic net's lattice coordinate, the
            // beam march's entry point, the bed's facing test. A NaN in one row is a NaN in
            // every pixel of the frame, and a collapsed row puts the whole world at the
            // origin, so both are refused here rather than uploaded and guarded per pixel.
            float axes[3][3];
            for (int row = 0; row < 3; ++row)
            {
                float lengthSquared = 0.f;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float value = frameContext.inverseView.m[row][axis];
                    if (!std::isfinite(value))
                    { NoteIdle(8, "inverse view row " + std::to_string(row) + " is not finite"); return false; }
                    axes[row][axis] = value;
                    lengthSquared += value * value;
                }
                if (!(lengthSquared > 1e-8f))
                { NoteIdle(8, "inverse view row " + std::to_string(row) + " is collapsed"); return false; }
            }

            if (!EnsureShader(device))
            {
                // Still compiling on the worker is not a failure: the frame keeps
                // the client's own underwater fog and the pass joins when the
                // bytecode lands, instead of the dive paying fxc to catch up.
                if (!ShaderPending()) NoteFailure("Underwater shader failed");
                return false;
            }
            D3DSURFACE_DESC desc{};
            if (FAILED(targetSurface->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0)
            { NoteIdle(10, "the target surface has no usable size"); return false; }

            // The camera-to-plane reading is the one number this pass cannot reconstruct: it
            // came from the liquid draws' own matrices. Without a finite up axis the sky glow
            // and the bed's facing test have nothing to point at, so the frame is left alone.
            float up[3], light[3];
            if (!Finite(inputs.up, up) || !Finite(inputs.lightDir, light))
            { NoteIdle(11, "the up axis or the light direction is not finite"); return false; }

            ScopedPassState saved(device);
            bool ok = true;
            auto check = [&](HRESULT hr) { if (FAILED(hr)) ok = false; };

            check(device->SetRenderTarget(0, targetSurface));
            check(device->SetDepthStencilSurface(nullptr));
            D3DVIEWPORT9 viewport{ 0, 0, desc.Width, desc.Height, 0, 1 };
            check(device->SetViewport(&viewport));
            check(device->SetVertexShader(nullptr));
            check(device->SetPixelShader(m_shader.Get()));
            check(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
            for (D3DRENDERSTATETYPE state : { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE,
                                              D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE,
                                              D3DRS_LIGHTING, D3DRS_SRGBWRITEENABLE, D3DRS_ALPHABLENDENABLE })
                check(device->SetRenderState(state, FALSE));
            check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
            check(device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
            check(device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0));
            // RGB only: the target's alpha belongs to whatever the client and the water stage
            // left in it, and this pass has no opinion about it.
            check(device->SetRenderState(D3DRS_COLORWRITEENABLE, 7));

            check(device->SetTexture(0, frameContext.sceneColor));
            check(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
            check(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
            check(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
            check(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE));
            check(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
            check(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
            check(device->SetTexture(1, depthTexture));
            check(device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT));
            check(device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
            check(device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
            check(device->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE));
            check(device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
            check(device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
            // No mask bound this frame samples as zero, which means "no water anywhere": the
            // pass would then shade every pixel including the liquid's own, and the surface
            // would carry the medium twice. Two things make that unreachable rather than
            // merely unlikely. The mask is allocated whenever any effect consumes it, and
            // IsActive is part of HasActiveEffects, so an active pass always has one. And the
            // one case where a mask exists but is not written - an MSAA target, where
            // WaterEffect.h refuses the MRT - is also the case where DepthCapture gets no INTZ
            // depth at all ("MSAA prevents INTZ depth capture"), so depthTexture is null and
            // this pass returned false above, exactly as the occlusion pass does. Were it ever
            // reached it would still be the safe direction to fail in: a double-shaded surface
            // for a frame, rather than an unshaded world.
            check(device->SetTexture(2, frameContext.waterMaskTexture));
            check(device->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_POINT));
            check(device->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
            check(device->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
            check(device->SetSamplerState(2, D3DSAMP_SRGBTEXTURE, FALSE));
            check(device->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
            check(device->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));

            const float projectionConstants[4] = { unpack[0], unpack[1], unpack[2], unpack[3] };
            const float screenConstants[4] = { 1.f / float(desc.Width), 1.f / float(desc.Height),
                                               frameContext.depthMaxZ, ramp };
            // The yards of water over the camera: the plane the beam march walks each ray back
            // up to, and the depth the caustic net fades out under. Guarded rather than clamped,
            // because std::max(NaN, 0) is not specified to be 0 - a NaN would sail straight
            // through the clamp and into every beam and every net in the frame.
            const float yardsOverhead = std::isfinite(frameContext.underwaterYards)
                ? std::max(frameContext.underwaterYards, 0.f) : 0.f;
            const float extinctionConstants[4] = { inputs.medium.extinction.x, inputs.medium.extinction.y,
                                                   inputs.medium.extinction.z, yardsOverhead };
            // The frame's effective tuning: the player's [Underwater] dials, scaled
            // by the place the frame was settled for. Clamped to the same ranges
            // the core spells, so a zone can lean on a dial but never past it.
            underwater::Settings settings = m_settings;
            const auto scaled = [](float value, float multiplier, float hi) {
                return std::clamp(value * (std::isfinite(multiplier) ? multiplier : 1.f), 0.f, hi);
            };
            settings.caustics = scaled(settings.caustics, inputs.causticMultiplier, 1.f);
            settings.shafts = scaled(settings.shafts, inputs.shaftMultiplier, 1.f);
            settings.surfaceGlow = scaled(settings.surfaceGlow, inputs.glowMultiplier, 1.f);
            settings.turbidity = scaled(settings.turbidity, inputs.turbidityMultiplier, 3.f);
            const float deepConstants[4] = { inputs.medium.deep.x, inputs.medium.deep.y,
                                             inputs.medium.deep.z, settings.strength };
            const float tintConstants[4] = { inputs.medium.tint.x, inputs.medium.tint.y,
                                             inputs.medium.tint.z, settings.surfaceGlow };
            const float lightConstants[4] = { light[0], light[1], light[2], inputs.sunElevation };
            const float lightColourConstants[4] = { std::max(inputs.lightColour[0], 0.f),
                                                    std::max(inputs.lightColour[1], 0.f),
                                                    std::max(inputs.lightColour[2], 0.f),
                                                    settings.caustics };
            const float axis0[4] = { axes[0][0], axes[0][1], axes[0][2], settings.shafts };
            const float axis1[4] = { axes[1][0], axes[1][1], axes[1][2], settings.distortion };
            const float axis2[4] = { axes[2][0], axes[2][1], axes[2][2], m_debugMode };
            // The wobble clock is wrapped on the CPU into the one period all four of its rates
            // share, so the argument never grows past the point where a float32 stops having
            // fraction bits - the same reason the water field wraps its phases.
            const float seconds = inputs.field.seconds;
            const float wrapped = std::isfinite(seconds)
                ? seconds - std::floor(seconds / underwater::kWobblePeriodSeconds) * underwater::kWobblePeriodSeconds
                : 0.f;
            const float upConstants[4] = { up[0], up[1], up[2], wrapped };
            const float latticeConstants[4] = { inputs.field.lattice[0], inputs.field.lattice[1],
                                                inputs.field.lattice[2], inputs.field.lattice[3] };
            const float chopConstants[4] = { inputs.field.chopPhase[0], inputs.field.chopPhase[1],
                                             inputs.field.finePhase[0], inputs.field.finePhase[1] };
            // z carries TURBIDITY and w its exit-wash form: the two spare lanes of the
            // warp register, which the broad warp leaves empty, are the dirt's home
            // rather than a ninth register. The w lane is ExitWash with its 1 removed,
            // folded here from the submersion this frame already knows, because the
            // stage has neither the slots nor a per-pixel cosine to spare for the
            // near-surface layer's slant yards (see kInterfaceScatterYards).
            const float exitTurbidity = underwater::ExitWash(settings.turbidity, yardsOverhead, true) - 1.f;
            const float warpConstants[4] = { inputs.field.warpPhase[0], inputs.field.warpPhase[1],
                                             settings.turbidity, exitTurbidity };
            check(device->SetPixelShaderConstantF(0, projectionConstants, 1));
            check(device->SetPixelShaderConstantF(1, screenConstants, 1));
            check(device->SetPixelShaderConstantF(2, extinctionConstants, 1));
            check(device->SetPixelShaderConstantF(3, deepConstants, 1));
            check(device->SetPixelShaderConstantF(4, tintConstants, 1));
            check(device->SetPixelShaderConstantF(5, lightConstants, 1));
            check(device->SetPixelShaderConstantF(6, lightColourConstants, 1));
            check(device->SetPixelShaderConstantF(7, axis0, 1));
            check(device->SetPixelShaderConstantF(8, axis1, 1));
            check(device->SetPixelShaderConstantF(9, axis2, 1));
            check(device->SetPixelShaderConstantF(10, upConstants, 1));
            check(device->SetPixelShaderConstantF(11, latticeConstants, 1));
            check(device->SetPixelShaderConstantF(12, chopConstants, 1));
            check(device->SetPixelShaderConstantF(13, warpConstants, 1));

            struct V { float x, y, z, w, u, v; };
            const float quadWidth = float(desc.Width) - .5f, quadHeight = float(desc.Height) - .5f;
            const V quad[] = { { -.5f, -.5f, 0, 1, 0, 0 }, { quadWidth, -.5f, 0, 1, 1, 0 },
                               { -.5f, quadHeight, 0, 1, 0, 1 }, { quadWidth, quadHeight, 0, 1, 1, 1 } };
            if (ok) check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V)));
            if (ok) NoteIdle(0, std::string());

            for (DWORD slot = 0; slot < 3; ++slot) check(device->SetTexture(slot, nullptr));
            if (!ok)
            {
                NoteFailure("Underwater pass failed");
                return false;
            }
            return true;
        }

    private:
        Underwater() = default;

        static bool Finite(const float in[3], float out[3])
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                if (!std::isfinite(in[axis])) return false;
                out[axis] = in[axis];
            }
            return true;
        }

        void Log(const std::string& text)
        {
            if (m_logPath.empty()) return;
            std::ofstream(std::filesystem::path(m_logPath), std::ios::app) << text << '\n';
        }

        // Prepares the bytecode on a worker: a disk-cache read on a warm launch, a
        // full fxc run on a cold one, and in neither case a render-thread cost.
        // D3DCompile needs no device and no COM apartment of its own here (the water
        // replacement stages have compiled exactly this way since they grew a
        // worker); what it must never again do is run inside the first frame of a
        // launch, which is where Prewarm used to put it - before the client's first
        // clear, with the login screen still undrawn behind it. Publication is one
        // lock: blob, error and elapsed time land together, and EnsureShader is the
        // only consumer. The singleton outlives any worker it starts.
        void StartPrepare()
        {
            bool forceCompile = false;
            {
                std::lock_guard<std::mutex> lock(m_prepareMutex);
                if (m_prepareStarted || m_prepareDone || m_preparedBytecode || m_bytecode) return;
                m_prepareStarted = true;
                m_prepareError.clear();
                forceCompile = m_forceNextPrepare;
                m_forceNextPrepare = false;
            }
            const std::wstring cacheDir = m_shaderCacheDir;
            const uint64_t began = GetTickCount64();
            try
            {
                std::thread([this, cacheDir, forceCompile, began] {
                    Microsoft::WRL::ComPtr<ID3DBlob> blob;
                    std::string error;
                    bool ok = false;
                    try
                    {
                        ok = shaderbytecode::GetBytecode(cacheDir, kUnderwaterSource, "main", "ps_3_0",
                                                          blob, forceCompile, &error);
                    }
                    catch (...) { error = "shader bytecode preparation raised an exception"; }
                    std::lock_guard<std::mutex> lock(m_prepareMutex);
                    if (ok) m_preparedBytecode = blob;
                    m_prepareError = error;
                    m_prepareElapsedMs = GetTickCount64() - began;
                    m_prepareStarted = false;
                    m_prepareDone = true;
                }).detach();
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(m_prepareMutex);
                m_prepareError = "could not start the shader preparation thread";
                m_prepareElapsedMs = GetTickCount64() - began;
                m_prepareStarted = false;
                m_prepareDone = true;
            }
        }

        bool ShaderPending()
        {
            std::lock_guard<std::mutex> lock(m_prepareMutex);
            return m_prepareStarted;
        }

        bool EnsureShader(IDirect3DDevice9* device)
        {
            if (m_shader) return true;
            if (!m_bytecode)
            {
                Microsoft::WRL::ComPtr<ID3DBlob> prepared;
                std::string error;
                uint64_t elapsedMs = 0;
                bool started = false, done = false;
                {
                    std::lock_guard<std::mutex> lock(m_prepareMutex);
                    prepared = m_preparedBytecode;
                    error = m_prepareError;
                    elapsedMs = m_prepareElapsedMs;
                    started = m_prepareStarted;
                    done = m_prepareDone;
                    if (prepared) { m_preparedBytecode.Reset(); m_prepareDone = false; }
                }
                if (prepared)
                {
                    m_bytecode = prepared;
                    if (!m_prepareLogged)
                    {
                        m_prepareLogged = true;
                        Log("underwater shader bytecode ready in " + std::to_string(elapsedMs)
                            + " ms (worker thread; no frame waited on fxc)");
                    }
                }
                else if (started)
                {
                    return false;   // pending, not failed: the caller leaves the frame alone
                }
                else if (done)
                {
                    // A finished preparation with no blob is a failure to consume once:
                    // clear it so the caller's bounded retry can start another.
                    std::lock_guard<std::mutex> lock(m_prepareMutex);
                    m_prepareDone = false;
                    m_prepareError.clear();
                    if (!m_errorLogged)
                    {
                        m_errorLogged = true;
                        Log("Underwater shader unavailable, will retry: " + error);
                    }
                    return false;
                }
                else
                {
                    StartPrepare();
                    return false;
                }
            }
            // The one device call, on the render thread, from bytecode that is
            // already in hand. A blob the driver refuses is recompiled once - on
            // the worker again, not here - exactly as the synchronous path did.
            const HRESULT created = device->CreatePixelShader(
                static_cast<DWORD*>(m_bytecode->GetBufferPointer()), m_shader.GetAddressOf());
            if (SUCCEEDED(created) && m_shader)
            {
                m_errorLogged = false;
                return true;
            }
            m_shader.Reset();
            if (!m_forceCompileTried)
            {
                m_forceCompileTried = true;
                m_bytecode.Reset();
                {
                    std::lock_guard<std::mutex> lock(m_prepareMutex);
                    m_preparedBytecode.Reset();
                    m_prepareDone = false;
                    m_forceNextPrepare = true;
                }
                StartPrepare();
                if (!m_errorLogged)
                {
                    m_errorLogged = true;
                    Log("the driver refused the cached underwater bytecode; recompiling on the worker");
                }
                return false;
            }
            if (!m_errorLogged)
            {
                m_errorLogged = true;
                Log("Underwater shader unavailable, will retry: CreatePixelShader failed");
            }
            return false;
        }

        // Which of the refusals above is the one firing, logged on transition only: a pass
        // that never draws is invisible in-game, and the difference between "the tracker says
        // the camera is not in the water" and "the depth capture never latched" is the whole
        // diagnosis. One line per change of state, so a log from a player reads as a story:
        // idle (2) while on the shore, drew when they jumped in, idle (2) when they climb out.
        void NoteIdle(int code, const std::string& detail)
        {
            if (m_idleReason == code) return;
            m_idleReason = code;
            if (code == 0) Log("underwater pass drawing");
            else Log("underwater pass idle (" + std::to_string(code) + "): " + detail);
        }

        void NoteFailure(const std::string& reason)
        {
            if (m_setupRetry.Fail(GetTickCount64(), underwaterpass::kSetupRetryMs) && !m_errorLogged)
            {
                m_errorLogged = true;
                Log(reason + "; the frame is left as the compositor built it and the pass is retried");
            }
        }

        underwater::Settings m_settings;
        float m_debugMode = 0.f;
        uint64_t m_settingsFingerprint = 0;
        // Kept so a device reset recreates the pixel shader from the blob it already has
        // instead of asking fxc (or the on-disk cache) for it again.
        Microsoft::WRL::ComPtr<ID3DBlob> m_bytecode;
        Microsoft::WRL::ComPtr<IDirect3DPixelShader9> m_shader;
        // The worker's publication (StartPrepare): everything under this mutex is
        // written by the preparation thread and consumed once by EnsureShader.
        mutable std::mutex m_prepareMutex;
        Microsoft::WRL::ComPtr<ID3DBlob> m_preparedBytecode;
        std::string m_prepareError;
        uint64_t m_prepareElapsedMs = 0;
        bool m_prepareStarted = false;
        bool m_prepareDone = false;
        bool m_forceNextPrepare = false;
        bool m_forceCompileTried = false;
        bool m_prepareLogged = false;
        unsigned m_prewarmFrames = 0;
        RetryGate m_setupRetry;
        bool m_errorLogged = false;
        int m_idleReason = -1;
        std::wstring m_logPath;
        std::wstring m_shaderCacheDir;
    };
}
