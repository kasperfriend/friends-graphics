#include "../Environment/LocationTuning.h"
#include "VolumetricClouds.h"
#include "CelestialDiscGate.h"
#include "CloudNoise.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include "../D3D9/CelestialStability.h"
#include "../D3D9/DepthCapture.h"
#include "../D3D9/CelestialTracker.h"
#include "../D3D9/ScopedPassState.h"
#include "../D3D9/ShaderBytecodeCache.h"
#include "../Diagnostics/PerformanceProfiler.h"
#include "EnvironmentFogCapture.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
namespace
{
// Volumetric cloud deck.
//
// The visible deck occupies a fixed world-Z interval, independent of the
// camera position or orientation, so it can be flown into and above.
// Density comes from three offline-generated, tiling fields that
// `CloudNoise.h` builds once and caches next to the DLL:
//
//   shape   64^3 RGBA  R = Perlin-Worley base, GBA = Worley erosion octaves
//   detail  32^3 RGB   three Worley octaves for the cauliflower edge
//   weather 512^2 RGBA R = coverage, G = cloud type, B = variation
//
// Lighting is the standard participating-medium pair - a short Beer march
// toward the tracked disc plus a dual-lobe Henyey-Greenstein phase - with
// Wrenninge's three-octave multiple-scattering approximation on top, which
// is what keeps a thick cumulus from turning into a black cut-out, and a
// powder term for the dark edge Beer alone cannot produce.
//
// Scene depth clips the march: clouds in front of terrain remain visible
// when flying inside or above the layer. Near geometry occludes the clouds.
const char* kCloudMarchSource = R"HLSL(
sampler2D depthMap:register(s0); sampler3D shapeMap:register(s1);
sampler3D detailMap:register(s2); sampler2D weatherMap:register(s3);
float4 cameraPos:register(c0);      // xyz=camera world position, w=unused
float4 celestial:register(c1);      // xyz=world direction TOWARDS the tracked disc, w=intensity
float4 invView0:register(c2); float4 invView1:register(c3); float4 invView2:register(c4);
float4 projection:register(c5);     // x=A, y=B (raw->view z), z=P00, w=P11
float4 depthRange:register(c6);     // x=viewport MinZ, y=depth range, z=viewport MaxZ, w=march steps
float4 layer:register(c7);          // x=world base Z, y=world top Z, z=1/thickness, w=max distance
float4 region:register(c8);         // w=camera region (0 inside, 1 below, 2 above)
float4 window:register(c9);         // x=coverage low edge, y=1/(high-low), z=cloud type offset, w=aerial opacity
float4 scales:register(c10);        // x=1/feature size, y=shape z scale, z=1/detail size, w=detail strength
float4 motion:register(c11);        // xy=shape uv offset, z=1/weather size, w=light reach in steps
float4 motion2:register(c12);       // xy=weather uv offset, zw=shear (world units per height fraction)
float4 stepping:register(c13);      // x=base step, y=step growth, z=coarse scale, w=jitter amount
float4 lighting:register(c14);      // x=extinction, y=light absorption, z=direct gain, w=ambient gain
float4 sunColor:register(c15);      // rgb=direct colour, w=powder strength
float4 skyColor:register(c16);      // rgb=zenith ambient, w=silver lining intensity
float4 groundColor:register(c17);   // rgb=ground bounce ambient, w=horizon fade width
float4 lobe:register(c18);          // x=forward g, y=backward g, z=ms eccentricity, w=debug mode
float4 octaves:register(c19);       // x=a^1, y=a^2 (extinction), z=b^1, w=b^2 (contribution)
float4 detailMotion:register(c20);  // xy=detail uv offset, z=light step length, w=light steps
float4 haze:register(c22);          // rgb=aerial perspective colour, w=1/fade distance
float4 evolution:register(c23);     // x=shape Z phase, y=detail Z phase, z=weather variation, w=type scale
float4 moonDisc:register(c24); // xy=tracked screen centre, z=disc view depth
float4 moonControl:register(c25);   // x=vertical UV radius, y=depth tolerance, z=valid, w=horizontal scale
// xy=the sprite's own normalized raw depth and how far it may drift across the
// disc, z=rim tap radius, w=usable. A camera-facing billboard is ONE depth
// across its whole face; an angled floor, wall or roof at the same distance is
// not. See src/Effects/CelestialDiscGate.h - change both in the same commit.
float4 moonSprite:register(c26);
float4 sunDisc:register(c27); // xy=tracked sun centre, z=radius squared (negative when invalid)
float4 sunDepth:register(c28); // x=horizontal scale, y=minimum qualifying raw depth
float SunDiscPixel(float2 uv,float rawDepth) {
 float2 delta=float2((uv.x-sunDisc.x)*sunDepth.x,uv.y-sunDisc.y);
 float disc=step(dot(delta,delta),sunDisc.z);
 // Normal D3D depth grows with distance: clear sky and the far sun are at or
 // beyond the tracked depth; nearer scene geometry remains an occluder. The
 // depth threshold and disc radius squared are precomputed by the host.
 float depthOpen=step(sunDepth.y,rawDepth);
 return disc*depthOpen;
}
float SpriteVerified(float2 centre) {
 if(moonSprite.w<.5) return 0;
 float2 h=float2(moonSprite.z/max(moonControl.w,1e-3),0),v=float2(0,moonSprite.z);
 // Raw viewport depth, compared the same way as renderer::discgate::IsFlatSprite:
 // one flat, camera-facing value across the sprite's face.
 float c=tex2D(depthMap,centre).r;
 float2 rim=float2(tex2D(depthMap,centre+h).r,tex2D(depthMap,centre+v).r);
 float tol=max(moonSprite.y,1e-5);
 return step(abs(c-moonSprite.x),tol)*step(max(abs(rim.x-c),abs(rim.y-c)),tol);
}

// Analytic vertical profile per cloud type: this is what separates a flat
// overcast lid from towering cumulus with flat bottoms and round tops.
float Gradient(float h,float type) {
 float stratus=saturate(h*14.2857)*saturate((.32-h)*8.3333);
 float cumulus=saturate((h-.08)*4.1667)*saturate((.96-h)*2.6316);
 float tower=saturate((h-.04)*7.1429)*saturate((1-h)*5.5556);
 return stratus*saturate(1-type*2)+cumulus*saturate(1-abs(type-.5)*2)+tower*saturate(type*2-1);
}

float HeightFraction(float3 p) { return saturate((p.z-layer.x)*layer.z); }

// `erosion` is 0 for the light march and the coarse probe, which lets the
// compiler drop the detail fetch from those two inlined copies entirely.
float Density(float3 p,float h,float erosion) {
 float3 w=tex2Dlod(weatherMap,float4(p.xy*motion.z+motion2.xy,0,0)).rgb;
 // The slider slides a window along the weather map's (histogram-equalised)
 // coverage channel. Both ends matter: the low edge decides how much sky
 // gets any cloud at all, the high edge how solid a mass becomes.
 float cov=saturate((w.r+(w.b-.5)*evolution.z-window.x)*window.y);
 float g=Gradient(h,saturate(w.g*evolution.w+window.z));
 if(cov*g<=0) return 0;
 // Shear the tops downwind with height - the cheap stand-in for a curl
 // distortion that ps_3_0 cannot afford - then sample the base shape.
 float4 s=tex3Dlod(shapeMap,float4((p.xy+motion2.zw*h)*scales.x+motion.xy,p.z*scales.y+evolution.x,0));
 float fbm=s.g*.625+s.b*.25+s.a*.125;
 // Perlin-Worley base eroded by its own Worley octaves, then the coverage
 // remap: raising coverage pulls the whole field up through the threshold
 // instead of simply scaling what is already there.
 float d=saturate((s.r-fbm+1)/max(2-fbm,1e-4))*g;
 d=saturate((d-1+cov)/max(cov,1e-4))*(.45+.55*cov);
 if(d<=0) return 0;
 if(erosion>0) {
   float3 n=tex3Dlod(detailMap,float4((p.xy+motion2.zw*h)*scales.z+detailMotion.xy,p.z*scales.z+evolution.y,0)).rgb;
   // Wispy shreds at the base, cauliflower billows at the top.
   float dfbm=n.r*.625+n.g*.25+n.b*.125;
   // Erosion that fades out inside the core. Eroding uniformly either
   // leaves the silhouette smooth or hollows the cloud out; real cumulus
   // shred at the edges and stay solid in the middle.
   float strength=erosion*(1-d*.7);
   float eroded=saturate((d-lerp(1-dfbm,dfbm,saturate(h*4))*strength)/max(1-erosion,1e-4));
   // Preserve enough of the thin lower cloud boundary for grazing rays;
   // full detail erosion fades in through the base instead of erasing it.
   d=lerp(d,eroded,saturate(h*10));
 }
 // More water higher up, modulated by the weather map's variation channel.
 return d*lerp(.45,1.25,saturate(h*1.4))*lerp(.75,1.2,w.b)*lighting.x;
}

// Deliberately NOT divided by 4*pi: an isotropic lobe returns 1, so the
// direct term is "sunlight times how much of it reaches here" and the gain
// slider means what it says.
float HG(float c,float g) { return (1-g*g)*pow(max(1+g*g-2*g*c,1e-4),-1.5); }

// WoW draws the moon as a small camera-relative billboard. Its depth is much
// nearer than the world-space cloud deck, so treating that depth as terrain
// clipped the clouds away in a dark circular hole. Match only the tracked
// disc's view-space cone AND depth; all other geometry remains an occluder.
float MoonDiscPixel(float2 uv,float rawDepth,float viewDepth,float sprite) {
 float2 delta=(uv-moonDisc.xy)*float2(moonControl.w,1);
 float disc=1-step(moonControl.x*moonControl.x,dot(delta,delta));
 float depthMatch=1-step(moonControl.y,abs(viewDepth-moonDisc.z));
 float geometry=1-step((depthRange.z+1)*.5,rawDepth);
 return disc*depthMatch*geometry*moonControl.z*sprite;
}

float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
 float rawDepth=tex2D(depthMap,uv).r;
 // INTZ stores viewport depth; normalise it exactly like the atmosphere's
 // boundary pass so one convention covers every injected pass.
 // Clear depth stays at one; normalize only the depth used for
 // projection reconstruction. Geometry/sky classification below uses the
 // original viewport-mapped value and MaxZ.
 float raw=rawDepth>=1?1:saturate((rawDepth-depthRange.x)/max(depthRange.y,.001));

 float3 view=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1);
 // The inverse view rows are orthonormal, so dividing by the view-space
 // length is the normalize - and it hands back the scale factor that turns
 // a view-space depth into a distance along this ray for free.
 float viewLength=length(view);
 float3 ray=(view.x*invView0.xyz+view.y*invView1.xyz+view.z*invView2.xyz)/viewLength;
 // Geometry in front of the deck clips the march instead of cancelling it.
 // A sky-only march deletes every cloud you are flying over - from above
 // the layer each downward ray ends on terrain - and stops a mountain top
 // inside the deck from being fogged by the cloud it is standing in.
 // The reciprocal is clamped away from the projection's pole: a far plane
 // at infinity puts raw depth exactly on it, and the infinity that comes
 // back turns into a NaN the moment it meets another one.
 float hasGeometry=1-step((depthRange.z+1)*.5,rawDepth);
 float sceneViewDepth=projection.y/min(raw-projection.x,-1e-6);
 // Only where the tracked sprite really is: at the disc's own centre and rim
 // the buffer must hold one flat camera-facing depth. A floor or ceiling that
 // happens to sit at the billboard's 4-16 yards inside the same circle is an
 // angled surface and fails, so it cannot punch the moon's disc through the
 // ground and let the deck march through it.
 float2 discDelta=float2((uv.x-moonDisc.x)*moonControl.w,uv.y-moonDisc.y);
 float withinDisc=1-step(length(discDelta),moonControl.x*1.75);
 float moonPixel=MoonDiscPixel(uv,rawDepth,sceneViewDepth,
                               withinDisc>.5?SpriteVerified(moonDisc.xy):0);
 float sunPixel=SunDiscPixel(uv,rawDepth);
 // Both tracked billboards are sky radiance rather than terrain depth. The
 // cloud composite will still shade the sun; this only lets the march pass
 // through the sprite's finite depth instead of treating it as an occluder.
 float sceneLimit=hasGeometry>.5&&moonPixel<.5&&sunPixel<.5
     ?sceneViewDepth*viewLength:layer.w;

 // Intersect fixed world-height planes, never a camera-centred dome.
 // Horizontal rays inside the layer march to the scene/distance limit;
 // outside it they cannot hit a cloud. Avoid division by zero at the horizon.
 float entry=0,exitDistance=layer.w;
 if(abs(ray.z)<1e-6) {
   if(cameraPos.z<layer.x||cameraPos.z>layer.y) return float4(0,0,0,1);
 } else {
   float bottom=(layer.x-cameraPos.z)/ray.z;
   float top=(layer.y-cameraPos.z)/ray.z;
   entry=max(0,min(bottom,top));
   exitDistance=max(bottom,top);
 }
 exitDistance=min(exitDistance,min(layer.w,sceneLimit));
 if(exitDistance<=entry) return float4(0,0,0,1);

 // Do not attenuate by view pitch: that made the deck vanish over a band of
 // upward angles. The fixed-height intersection and scene depth already decide
 // which rays can hit clouds; only soften the configured finite-distance edge.
 float fadeWidth=max(groundColor.w,.1);
 float fadeStart=layer.w*(1-fadeWidth);
 float fade=1-smoothstep(fadeStart,layer.w,entry);

 // Phase depends only on the angle between the ray and the light, so all
 // three scattering octaves are evaluated once per pixel.
 float3 lightDir=normalize(celestial.xyz);
 float cosTheta=dot(ray,lightDir);
 // Sharp forward peak for the silver lining plus a broad backward lobe so
 // the anti-solar side is not flat. Evaluated once: each deeper scattering
 // octave only relaxes it towards isotropic, which is both what the
 // approximation wants and six Henyey-Greenstein evaluations cheaper.
 float single=min(lerp(HG(cosTheta,lobe.x),HG(cosTheta,-lobe.y),.35)
                 +skyColor.w*pow(saturate(cosTheta),24),9);
 float3 phase=float3(single,lerp(1,single,lobe.z),lerp(1,single,lobe.z*lobe.z));

 // A step tied to world length, never to the span divided by the step
 // count: the layer is 1400 units thick but can stretch 200000 units along
 // shallow rays. Distance growth preserves detail while keeping that far field affordable.
 float baseStep=max(stepping.x,(exitDistance-entry)/max(depthRange.w*3,1));
 float jitter=.5+(frac(52.9829189*frac(dot(vpos,float2(.06711056,.00583715))))-.5)*stepping.w;
 float t=entry+jitter*baseStep;
 float transmittance=1,direct=0,mode=-1;   // mode<0 marches coarse, >=0 counts empty steps
 float depthSum=0;                        // transmittance-weighted distance
 float3 scattered=0;

 [loop] for(int i=0;i<160;i++) {
   if(i>=(int)depthRange.w||t>=exitDistance||transmittance<.01) break;
   // Clamped to what is left: an overshooting last step integrates cloud
   // from behind the surface that stopped the march, which shows up as a
   // bright fringe exactly where terrain cuts into the deck.
   float stepLength=min(max(baseStep,t*stepping.y),exitDistance-t);
   float3 p=cameraPos.xyz+ray*t;
   float h=HeightFraction(p);
   // One inlined copy of the field per iteration, not one per branch: the
   // coarse probe simply asks for it without the erosion fetch. Two call
   // sites here cost about seventy instruction slots of the 512 budget.
   float density=Density(p,h,mode<0?0:scales.w);
   if(mode<0) {
     // Coarse probes remain below a feature-sized stride even when t is far
     // away. A pitch change can move the deck hundreds of kilometres out;
     // letting t*growth skip whole cells made the entire sky blink off.
     // The remaining-span term still guarantees progress within the budget.
     float coarseStride=max(min(stepLength*stepping.z,.75/scales.x),
                            (exitDistance-t)/(depthRange.w-i));
     if(density>0) { mode=0; t=max(entry,t-coarseStride); }
     else t+=coarseStride;
   } else {
     if(density<=0) {
       mode+=1;
       if(mode>6) mode=-1;
       t+=stepLength;
     } else {
       mode=0;
       // Beer march toward the disc. A real loop, not [unroll]: inlined
       // copies of the field keep far too many intermediates alive at once
       // and blow past the 32 temp registers ps_3_0 has (fxc error X4505).
       // The final iteration is the long reach sample - a tower far up the
       // light path still has to cast a shadow - folded into the loop
       // instead of being a second call site, because one more inlined copy
       // of the density field costs about forty of the 512 slots.
       float optical=0;
       [loop] for(int j=0;j<8;j++) {
         if(j>=(int)detailMotion.w) break;
         float last=saturate(j-detailMotion.w+2);
         float3 q=p+lightDir*(detailMotion.z*lerp(j+.5,motion.w,last));
         optical+=Density(q,HeightFraction(q),0)*lerp(1,3,last);
       }
       optical*=detailMotion.z*lighting.y;
       // Three octaves with geometrically scaled extinction, contribution
       // and phase: single scattering alone renders thick cloud as slate.
       float energy=phase.x*exp(-optical)+octaves.z*phase.y*exp(-optical*octaves.x)
                   +octaves.w*phase.z*exp(-optical*octaves.y);
       float powder=lerp(1,1-exp(-density*18),sunColor.w*saturate(cosTheta*.5+.5));
       // Sky above, dark ground below: the vertical ambient gradient is
       // most of what makes a cumulus read as three-dimensional.
       float3 lit=sunColor.rgb*(lighting.z*energy*powder)
                 +lerp(groundColor.rgb,skyColor.rgb,pow(h,.7))*lighting.w;
       // Analytic in-scattering over the step: energy conserving, and
       // stable when the step length changes along the ray.
       float stepTransmittance=exp(-density*stepLength);
       float deposited=transmittance*(1-stepTransmittance);
       scattered+=lit*deposited;
       // Where the light the eye receives actually came from. The weights
       // sum to exactly 1-transmittance, so this costs one mad per sample.
       depthSum+=t*deposited;
       direct+=energy*(1-stepTransmittance);
       transmittance*=stepTransmittance;
       t+=stepLength;
     }
   }
 }

 // Aerial perspective, applied once to the integrated result rather than
 // per sample: distance has to desaturate the radiance AND eat the
 // silhouette, or every cloud reads as equally close no matter how far
 // away it is.
 float opacity=saturate(1-transmittance);
 float aerial=1-exp(-(depthSum/max(opacity,1e-4))*haze.w);
 scattered=lerp(scattered,haze.rgb*opacity,aerial);
 transmittance=lerp(transmittance,1,aerial*window.w);

 scattered*=fade;
 transmittance=lerp(1,saturate(transmittance),fade);
 if(lobe.w>1.5&&lobe.w<2.5) return float4((1-transmittance).xxx,transmittance);
 if(lobe.w>2.5&&lobe.w<3.5) return float4(saturate(direct).xxx,transmittance);
 return float4(max(scattered,0),transmittance);
})HLSL";

// The cloud amount of the column between every world point and the top of
// the deck, measured along the sunlight, rendered into a small map centred
// on the camera. The world below reads it as its cloud shadow and the
// atmosphere reads it to decide which sun rays reach which part of the sky.
// Low-frequency planar approximation of the march field: same
// weather/shape/profile/wind, without detail erosion or viewer curvature.
//
// What the map stores is the SHAPE of the deck, not an optical depth. The
// first version marched the slant path with the cloud's real extinction,
// which is 20-50 optical depths through a full column: `exp(-tau)` is then
// 0 or 1, and the map degenerates into a saturated silhouette of a light
// path that is two to five kilometres long at any sun elevation below 45
// degrees. Everything within that distance of a point painted it black,
// neighbouring shadows merged into one dark mass, and stepping under a
// cloud dimmed the whole world instead of drawing that cloud's own shadow
// beneath it (measured with tools/CloudShadowProbe.cpp at the shipped
// defaults: 63 % of the ground shaded against 34 % of the plane having a
// casting cloud up-sun, most of the shading under an open sky). An amount
// that grows with coverage and saturates
// only when the column is genuinely solid keeps the deck's silhouette and
// its gradients, at every sun angle.
const char* kCloudShadowSource = R"HLSL(
sampler3D shapeMap:register(s1); sampler2D weatherMap:register(s3);
float4 celestial:register(c1);
float4 shell:register(c7);
float4 centre:register(c8);
float4 window:register(c9);
float4 scales:register(c10);
float4 motion:register(c11);
float4 motion2:register(c12);
float4 evolution:register(c23);
float4 area:register(c21);          // xy=world origin, z=world extent, w=receiver height

float Gradient(float h,float type) {
 float stratus=saturate(h*14.2857)*saturate((.32-h)*8.3333);
 float cumulus=saturate((h-.08)*4.1667)*saturate((.96-h)*2.6316);
 float tower=saturate((h-.04)*7.1429)*saturate((1-h)*5.5556);
 return stratus*saturate(1-type*2)+cumulus*saturate(1-abs(type-.5)*2)+tower*saturate(type*2-1);
}

// Unitless cloud fraction at a point of the layer: the march's density with
// its extinction (and therefore DensityPercent) folded out, and with the
// caller's own height fraction driving the profile, so a texel integrates
// the same field the march would sample at that height.
float Cloud(float3 p,float h) {
 float3 w=tex2Dlod(weatherMap,float4(p.xy*motion.z+motion2.xy,0,0)).rgb;
 float cov=saturate((w.r+(w.b-.5)*evolution.z-window.x)*window.y);
 float g=Gradient(h,saturate(w.g*evolution.w+window.z));
 if(cov*g<=0) return 0;
 float4 s=tex3Dlod(shapeMap,float4((p.xy+motion2.zw*h)*scales.x+motion.xy,p.z*scales.y+evolution.x,0));
 float fbm=s.g*.625+s.b*.25+s.a*.125;
 float d=saturate((s.r-fbm+1)/max(2-fbm,1e-4))*g;
 return saturate((d-1+cov)/max(cov,1e-4))*(.45+.55*cov)
       *lerp(.45,1.25,saturate(h*1.4))*lerp(.75,1.2,w.b);
}

float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float3 light=normalize(celestial.xyz);
 // A disc on the horizon casts shadows a hundred kilometres long; there is
 // nothing useful left to project, so hand back "lit".
 if(light.z<=.03) return float4(1,1,1,1);
 float3 p=float3(area.xy+uv*area.z,area.w);
 float base=shell.x+centre.z;
 // Sample the layer on a uniform grid in HEIGHT and displace each sample
 // along the sunlight by the height it sits at: the field the light crosses
 // on its way to this point, each sample weighted by the slice of column it
 // stands for. Sixteen steps across the full height is more than the old
 // eight across a slant path twice as long, and unlike the slant integral
 // the sample spacing is the same for every sun elevation.
 float low=saturate((p.z-base)*shell.z);
 float span=1-low;
 float stepHeight=span*.0625;
 // 1/shell.z is the layer thickness, so (h-low) of it is how far up the
 // light has to travel to reach that height. The receiver's own height is
 // subtracted, which is what makes an object inside or above the deck count
 // only the cloud that is actually between it and the light.
 float reach=1/(light.z*shell.z);
 // Start at the actual world-height base, not a second copy of the field
 // at sea level. Zone thickness/type changes must cast the visible bodies.
 float baseDistance=max(base-p.z,0)/light.z;
 float amount=0;
 [loop] for(int i=0;i<16;i++) {
   float h=low+stepHeight*(i+.5);
   amount+=Cloud(p+light*(baseDistance+(h-low)*reach),h);
 }
 amount*=.0625;
 // Transmittance, with an opacity per unit of cloud amount rather than the
 // cloud's physical extinction. The extinction is what saturated the old
 // map: a column that is one fifth covered is already 9 optical depths
 // deep, so it read as solid. Five is a contrast curve instead - a solidly
 // covered column blocks ~95 % of the light, a tenth-covered one ~35 %, and
 // open sky with a wisp up-sun a few percent - which is what makes the map
 // read as the deck's silhouette instead of a grey wash.
 return exp(-amount*5).xxxx;
})HLSL";

// Full-resolution composite. The low-resolution deck is a smooth signal
// with no depth edges of its own, so a plain bilinear fetch is enough -
// geometry pixels carry transmittance 1 and therefore fade the clouds out
// across a silhouette instead of haloing it. The same pass drops the cloud
// shadow onto everything the client drew.
const char* kCloudCompositeSource = R"HLSL(
sampler2D sceneMap:register(s0); sampler2D cloudMap:register(s1);
sampler2D depthMap:register(s2); sampler2D shadowMap:register(s3);
sampler2D waterMask:register(s4);
float4 texels:register(c0);      // x=1/full width, y=1/full height, z=1/low width, w=1/low height
float4 depthRange:register(c1);  // x=viewport MinZ, y=depth range, z=viewport MaxZ, w=debug
float4 cameraPos:register(c2);
float4 invView0:register(c3); float4 invView1:register(c4); float4 invView2:register(c5);
float4 projection:register(c6);  // x=A, y=B, z=P00, w=P11
float4 area:register(c7);        // xy=shadow world origin, z=1/extent, w=shadow strength
float4 lightProjection:register(c8); // xy=light XY/Z, z=cloud base, w=thickness
float4 moonDisc:register(c9); // xy=tracked screen centre, z=disc view depth
float4 moonControl:register(c10);  // x=vertical UV radius, y=depth tolerance, z=valid, w=horizontal scale
// Same contract as the march's c26 (see CelestialDiscGate.h).
float4 moonSprite:register(c12);
float4 mediumControl:register(c11); // x=how far the camera is under water (0 air, 1 submerged)
float4 sunDisc:register(c13); // xy=tracked sun centre, z=radius squared (negative when invalid)
float4 sunDepth:register(c14); // x=horizontal scale, y=minimum qualifying raw depth
float SunDiscPixel(float2 pixelUv,float rawDepth) {
 float2 delta=float2((pixelUv.x-sunDisc.x)*sunDepth.x,pixelUv.y-sunDisc.y);
 float disc=step(dot(delta,delta),sunDisc.z);
 float depthOpen=step(sunDepth.y,rawDepth);
 return disc*depthOpen;
}
float MoonDiscPixel(float2 pixelUv,float rawDepth,float viewDepth,float sprite) {
 float2 delta=(pixelUv-moonDisc.xy)*float2(moonControl.w,1);
 float disc=1-step(moonControl.x*moonControl.x,dot(delta,delta));
 float depthMatch=1-step(moonControl.y,abs(viewDepth-moonDisc.z));
 float geometry=1-step((depthRange.z+1)*.5,rawDepth);
 return disc*depthMatch*geometry*moonControl.z*sprite;
}
// A resolved sprite is one flat, camera-facing depth across its face. Verified
// once per pixel (and only inside a circle slightly larger than the disc), not
// once per upsample tap.
float SpriteVerified(float2 centre) {
 if(moonSprite.w<.5) return 0;
 float2 h=float2(moonSprite.z/max(moonControl.w,1e-3),0),v=float2(0,moonSprite.z);
 // Raw viewport depth, same units as the host's CelestialDiscGate values.
 float c=tex2D(depthMap,centre).r;
 float2 rim=float2(tex2D(depthMap,centre+h).r,tex2D(depthMap,centre+v).r);
 float tol=max(moonSprite.y,1e-5);
 return step(abs(c-moonSprite.x),tol)*step(max(abs(rim.x-c),abs(rim.y-c)),tol);
}
float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
 float4 scene=tex2D(sceneMap,uv);
 if(depthRange.w>3.5) return tex2D(shadowMap,uv).rrra;
 float linearRaw=tex2D(depthMap,uv).r;
 // The sampled scene is the client's original frame, before this cloud pass.
 // Tracked sun pixels are composited like sky, not returned unchanged: a cloud
 // over the sun must attenuate the original disc rather than leave a bright
 // unshaded cut-out. The depth helper below keeps the tracked disc from being
 // mistaken for foreground geometry.

 // Depth-aware upsample of the quarter-resolution march. The deck is no
 // longer sky-only - it is clipped by the scene, so a cloud in front of a
 // mountain is a real pixel - and a plain bilinear fetch would drag the
 // far cloud across the near silhouette as a halo. The four taps are
 // low-resolution texel centres, so point-sampling the depth map at the
 // same places returns exactly the depth each of those marches used.
 float2 lowTexel=texels.zw;
 float2 grid=uv/lowTexel-.5;
 float2 base=(floor(grid)+.5)*lowTexel, fr=frac(grid);
 float2 tapUv0=base,tapUv1=base+float2(lowTexel.x,0);
 float2 tapUv2=base+float2(0,lowTexel.y),tapUv3=base+lowTexel;
 float4 taps=float4(tex2D(depthMap,tapUv0).r,tex2D(depthMap,tapUv1).r,
                    tex2D(depthMap,tapUv2).r,tex2D(depthMap,tapUv3).r);
 // Compare in linear view depth: raw depth-buffer values crowd into the
 // last fraction of their range at distance, where most of the sky is.
 // Both reciprocals are clamped away from the projection's pole, which a
 // far plane at infinity sits exactly on: two infinities one subtraction
 // apart are a NaN, and a NaN weight poisons the whole pixel.
 float normalizedRaw=saturate((linearRaw-depthRange.x)/max(depthRange.y,.001));
 float4 normalizedTaps=saturate((taps-depthRange.x)/max(depthRange.y,.001));
 float centreZ=projection.y/min(normalizedRaw-projection.x,-1e-6);
 float4 tapZ=projection.y/min(normalizedTaps-projection.x,-1e-6);
 float clearZ=projection.y/min(1-projection.x,-1e-6);
 // Verify the sprite only where it can matter: cheap circle test first, so
 // the extra depth taps are paid on a handful of pixels.
 float2 discDelta=float2((uv.x-moonDisc.x)*moonControl.w,uv.y-moonDisc.y);
 float withinDisc=1-step(length(discDelta),moonControl.x*1.75);
 float sprite=withinDisc>.5?SpriteVerified(moonDisc.xy):0;
 float moonCentre=MoonDiscPixel(uv,linearRaw,centreZ,sprite);
 // The verified near moon sprite is not a depth occluder for the cloud march,
 // but it is still part of the scene radiance: composite clouds over it just
 // as over the tracked sun, rather than leaving a clear circular cut-out.
 float sunCentre=SunDiscPixel(uv,linearRaw);
 float4 moonTaps=float4(MoonDiscPixel(tapUv0,taps.x,tapZ.x,sprite),MoonDiscPixel(tapUv1,taps.y,tapZ.y,sprite),
                        MoonDiscPixel(tapUv2,taps.z,tapZ.z,sprite),MoonDiscPixel(tapUv3,taps.w,tapZ.w,sprite));
 float4 sunTaps=float4(SunDiscPixel(tapUv0,taps.x),SunDiscPixel(tapUv1,taps.y),
                       SunDiscPixel(tapUv2,taps.z),SunDiscPixel(tapUv3,taps.w));
 // Tracked sprite depths are not cloud/terrain edges. Treat matched sun pixels
 // as clear sky for upsample weighting, while still applying the cloud colour
 // and transmittance to the original sun pixel below.
 float filterCentreZ=lerp(centreZ,clearZ,max(moonCentre,sunCentre));
 float4 filterTapZ=lerp(tapZ,float4(clearZ,clearZ,clearZ,clearZ),max(moonTaps,sunTaps));
 float4 weight=float4((1-fr.x)*(1-fr.y),fr.x*(1-fr.y),(1-fr.x)*fr.y,fr.x*fr.y)
              /(.002+abs(filterTapZ-filterCentreZ)/max(filterCentreZ,1));
 float4 cloud=(tex2D(cloudMap,base)*weight.x+tex2D(cloudMap,base+float2(lowTexel.x,0))*weight.y
              +tex2D(cloudMap,base+float2(0,lowTexel.y))*weight.z+tex2D(cloudMap,base+lowTexel)*weight.w)
              /max(dot(weight,1),1e-5);

 if(depthRange.w>.5&&depthRange.w<1.5) return float4(cloud.rgb,1);
 float sky=max(step((depthRange.z+1)*.5,linearRaw),max(moonCentre,sunCentre));
 // The deck is air, and this pass is the last thing that touches a pixel before the frame is
 // graded. Water writes no depth, so a liquid pixel reads as sky to the march above and the
 // deck lands on it at full air brightness - which is right when the camera is standing in
 // that air, and wrong when the camera is under the surface looking up at the underside of
 // it: the water stage has already shaded, absorbed and in-scattered that pixel through its
 // own camera column, and a cloud painted over it afterwards is a full-brightness cloud ten
 // yards under water. The ramp gates it, so above water this is an exact identity and the
 // sea keeps the deck it has always had.
 float4 waterData=tex2D(waterMask,uv);
 float submergedWater=step(.35,max(waterData.r,waterData.g))*saturate(mediumControl.x);
 cloud.rgb*=1-submergedWater;
 cloud.a=lerp(cloud.a,1,submergedWater);
 // Reconstruct where this pixel is in the world and read the deck's
 // transmittance toward the sun. Unconditionally: a texture fetch
 // inside flow control would force fxc to hoist it anyway, and sky pixels
 // are masked out by the (1 - sky) term instead.
 float3 v=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*centreZ;
 float3 world=cameraPos.xyz+v.x*invView0.xyz+v.y*invView1.xyz+v.z*invView2.xyz;
 // Project along the sunlight onto the fixed sea-level reference plane.
 // This accounts for each receiver's height, not the camera's height.
 float2 shadowUv=(world.xy-world.z*lightProjection.xy-area.xy)*area.z;
 float2 edge=abs(shadowUv-.5);
 // Fade the map out before its border instead of clamping, which would
 // smear the last texel across the whole distance. The band is wide in
 // world units (edge is a fraction of the shadow extent, so it scales with
 // the coverage radius): a narrow band made the moving camera's coverage
 // disk drag a hard edge across terrain, reading as the world shadow
 // blinking in and out and the sun appearing to slide as you move.
 float inside=1-smoothstep(.34,.49,max(edge.x,edge.y));
 // The shadow belongs to the surface, so it is applied to the surface -
 // before the cloud in front of it is composited, not to the sum. Cloud
 // between the eye and the ground is not standing in its own shadow.
 float below=1-smoothstep(lightProjection.z,lightProjection.z+lightProjection.w,world.z);
 float3 lit=scene.rgb*lerp(1,tex2D(shadowMap,shadowUv).r,area.w*inside*(1-sky)*below);
 // Applied to every pixel, not just the sky: the march already stopped at
 // the depth buffer, so what is left in front of geometry belongs there.
 // Half an output level of static dither, as in the atmosphere composite:
 // the deck's smooth sunlit gradients otherwise quantize into concentric
 // rings around the sun, and the sky brightens/darkens a level at a time
 // as the deck drifts. The pattern is fixed per pixel, so it cannot
 // animate; downstream accumulation averages it to the true value.
 float3 composited=lit*saturate(cloud.a)+max(cloud.rgb,0);
 composited+=(frac(52.9829189*frac(dot(vpos,float2(.06711056,.00583715))))-.5)/255.0;
 return float4(composited,scene.a);
})HLSL";

// Scalar reference distance used only to size the sky-only noise field. It is
// deliberately independent of planet radius and camera position. The former
// 1500-yard anchor sampled less than one default shape tile across a typical
// view and, once the world weather map was removed from this direction-only
// pass, often read as one empty/soft mass. 6000 yards gives roughly two
// default feature widths across the reported view without planet-scale tiling.
constexpr float kSkyFallbackReferenceDistance = 6000.f;
// A subdued, cool floor for the cloud deck alone. Some zones report zero
// native ambient after sunset; clouds should become dark blue, not disappear
// into mathematically black radiance. Manual tint is applied after this floor.
constexpr float kCloudSkyAmbientFloor[3] = {.08f,.10f,.14f};
constexpr float kCloudGroundAmbientFloor[3] = {.05f,.065f,.09f};

// Emergency sky-only field for the narrow case where the current VIEW
// orientation is independently corroborated but its eye has aged out. This
// samples cloud shape on a direction sphere: no camera/world position, layer
// intersection, depth reconstruction, or world shadow can enter this pass.
// Its full-resolution composite below is separately restricted to clear sky
// and explicitly leaves native-water pixels untouched.
const char* kCloudSkyFallbackSource = R"HLSL(
sampler3D shapeMap:register(s0); sampler3D detailMap:register(s1);
float4 invView0:register(c0); float4 invView1:register(c1); float4 invView2:register(c2);
float4 projection:register(c3); // x=P00, y=P11
float4 scales:register(c4);     // x=direction-space feature scale, y=shape aspect, z=direction-space detail scale, w=detail strength
float4 motion:register(c5);     // xy=shape offset
float4 phase:register(c6);      // xy=detail offset, z=shape slice, w=detail slice
float4 coverage:register(c7);   // x=climate-resolved mean coverage, y=climate-resolved mean cloud type
float4 material:register(c8);   // rgb=zone/manual tint, w=density * opacity
float4 lighting:register(c9);   // xyz=world light direction, w=confirmed silver-lining strength
float4 volume:register(c10);    // xy=vertical shape/detail span; normalized by each feature size
float4 shear:register(c11);     // xy=shape shear, zw=detail shear across the virtual depth
float Gradient(float h,float type) {
 float stratus=saturate(h*14.2857)*saturate((.32-h)*8.3333);
 float cumulus=saturate((h-.08)*4.1667)*saturate((.96-h)*2.6316);
 float tower=saturate((h-.04)*7.1429)*saturate((1-h)*5.5556);
 return stratus*saturate(1-type*2)+cumulus*saturate(1-abs(type-.5)*2)+tower*saturate(type*2-1);
}
float Density(float3 ray,float h,float cov,float cloudType,out float shapeSignal,out float detailSignal) {
 shapeSignal=0;detailSignal=1;
 float gradient=Gradient(h,cloudType);
 if(cov*gradient<=0)return 0;
 // Treat the fallback as a short direction-space volume, not a single flat
 // noise sample. Its normalized vertical coordinate spans the configured
 // deck thickness; no camera eye, world point, or layer intersection enters.
 float3 shapePoint=float3(ray.x*scales.x+shear.x*h,ray.y*scales.x+shear.y*h,
     ray.z*scales.x*max(scales.y,.05)+(h-.5)*volume.x*max(scales.y,.05))
     +float3(motion.xy,phase.z);
 float4 shape=tex3Dlod(shapeMap,float4(shapePoint,0));
 float fbm=shape.g*.625+shape.b*.25+shape.a*.125;
 shapeSignal=saturate((shape.r-fbm+1)/max(2-fbm,1e-4));
 float density=shapeSignal*gradient;
 density=saturate((density-1+cov)/max(cov,1e-4))*(.45+.55*cov);
 if(density<=0)return 0;
 if(scales.w>0) {
  float3 detailPoint=float3(ray.x*scales.z+shear.z*h,ray.y*scales.z+shear.w*h,
      ray.z*scales.z+(h-.5)*volume.y)+float3(phase.xy,phase.w);
  float3 detail=tex3Dlod(detailMap,float4(detailPoint,0)).rgb;
  float dfbm=detail.r*.625+detail.g*.25+detail.b*.125;
  detailSignal=dfbm;
  float erosion=scales.w*(1-density*.7);
  density=saturate((density-lerp(1-dfbm,dfbm,h)*erosion)/max(1-scales.w,1e-4));
 }
 return density;
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float2 focal=max(projection.xy,.01);
 float3 viewRay=normalize(float3((uv.x*2-1)/focal.x,(1-uv.y*2)/focal.y,1));
 float3 ray=normalize(viewRay.x*invView0.xyz+viewRay.y*invView1.xyz+viewRay.z*invView2.xyz);
 // This recovery has no trusted world XY for the spatial weather map. A
 // direction-space weather projection invented one front across the whole
 // dome, which produced broad, disconnected blobs as the view turned. The
 // host supplies the climate/native-weather-resolved mean for this sky-only
 // approximation; the normal world march keeps the full spatial weather map.
 float cov=saturate(coverage.x);
 float cloudType=saturate(coverage.y);
 float sun=saturate(dot(ray,normalize(lighting.xyz)));
 float transmittance=1;
 float3 scattered=0;
 // Sixteen samples resolve the narrow stratus profile and the detail erosion
 // across the configured depth; eight left only two or three samples in low clouds.
 [loop] for(int i=0;i<16;++i) {
  float h=(i+.5)/16;
  float shapeSignal,detailSignal;
  float density=Density(ray,h,cov,cloudType,shapeSignal,detailSignal);
  if(density>0) {
   float sampleTransmittance=exp(-density*max(material.w,0)*2.25/16);
   float deposited=transmittance*(1-sampleTransmittance);
   float bodyLight=.75+.25*saturate(shapeSignal);
   float detailLight=.95+.05*saturate(detailSignal);
   float verticalLight=.75+.25*h;
   float3 cloudColour=lerp(material.rgb,float3(1,1,1),.12+.20*h)*verticalLight*bodyLight*detailLight;
   cloudColour+=pow(sun,16)*lighting.w*.16*(.5+.5*h)*saturate(1-density);
   scattered+=max(cloudColour,0)*deposited;
   transmittance*=sampleTransmittance;
  }
 }
 // The full-resolution composite is already restricted to clear sky and
 // excludes native water. A second ray-elevation fade would turn clouds off
 // merely because the player pitches through the horizon.
 return float4(max(scattered,0),saturate(transmittance));
})HLSL";

// Screen-only composite: no inverse view, held eye, world reconstruction or
// shadow sampler. The clear-depth test preserves all geometry; the coverage
// mask is an unconditional exclusion so this recovery cannot alter the water
// shader or turn a native-water pixel into a cloud pixel.
const char* kCloudSkyCompositeSource = R"HLSL(
sampler2D sceneMap:register(s0); sampler2D cloudMap:register(s1);
sampler2D depthMap:register(s2); sampler2D waterMask:register(s3);
float4 depthRange:register(c0); // x=viewport MinZ, y=depth range, z=MaxZ
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float4 scene=tex2D(sceneMap,uv);
 float rawDepth=tex2D(depthMap,uv).r;
 float sky=step((depthRange.z+1)*.5,rawDepth);
 float4 water=tex2D(waterMask,uv);
 float waterPixel=step(.35,max(water.r,water.g));
 float active=sky*(1-waterPixel);
 float4 cloud=tex2D(cloudMap,uv);
 float3 composited=scene.rgb*saturate(cloud.a)+max(cloud.rgb,0);
 return float4(lerp(scene.rgb,composited,active),scene.a);
})HLSL";

bool Compile(IDirect3DDevice9* d,const char* source,const std::wstring& cacheDir,IDirect3DPixelShader9** shader)
{
 // On-disk bytecode cache first (renderer::shaderbytecode): a repeat launch
 // skips fxc, and a cached blob the driver refuses falls back to one
 // recompile, exactly as a fresh compile would.
 ComPtr<ID3DBlob> blob;
 return renderer::shaderbytecode::GetShader(cacheDir,source,"main","ps_3_0",blob,
  [d,shader](ID3DBlob* b){return SUCCEEDED(d->CreatePixelShader(static_cast<DWORD*>(b->GetBufferPointer()),shader));});
}

// Extinction of fully dense cloud material per world unit. DensityPercent
// scales this, so the ini value stays a readable percentage instead of a
// physical coefficient: 100% over the default 1400-unit deck gives an
// optical depth around 30 through a tower, which is why the multiple
// scattering octaves matter so much.
constexpr float kBaseExtinction = .022f;

constexpr uint32_t kShapeSize = 64, kDetailSize = 32, kWeatherSize = 512;
constexpr uint32_t kCacheMagic = 0x46435743;  // 'CWCF'
constexpr uint32_t kCacheVersion = 3;

struct CacheHeader
{
 uint32_t magic, version, shape, detail, weather;
};

bool LoadFields(const std::wstring& path,cloudnoise::Volume3D& shape,cloudnoise::Volume3D& detail,cloudnoise::Image2D& weather)
{
 if(path.empty())return false;
 std::ifstream in(std::filesystem::path(path),std::ios::binary);
 if(!in)return false;
 CacheHeader header{};
 in.read(reinterpret_cast<char*>(&header),sizeof(header));
 if(!in||header.magic!=kCacheMagic||header.version!=kCacheVersion||
    header.shape!=kShapeSize||header.detail!=kDetailSize||header.weather!=kWeatherSize)return false;
 shape.size=int(kShapeSize);shape.rgba.resize(size_t(kShapeSize)*kShapeSize*kShapeSize*4);
 detail.size=int(kDetailSize);detail.rgba.resize(size_t(kDetailSize)*kDetailSize*kDetailSize*4);
 weather.width=weather.height=int(kWeatherSize);weather.rgba.resize(size_t(kWeatherSize)*kWeatherSize*4);
 in.read(reinterpret_cast<char*>(shape.rgba.data()),std::streamsize(shape.rgba.size()));
 in.read(reinterpret_cast<char*>(detail.rgba.data()),std::streamsize(detail.rgba.size()));
 in.read(reinterpret_cast<char*>(weather.rgba.data()),std::streamsize(weather.rgba.size()));
 return bool(in);
}

void SaveFields(const std::wstring& path,const cloudnoise::Volume3D& shape,const cloudnoise::Volume3D& detail,const cloudnoise::Image2D& weather)
{
 if(path.empty())return;
 // Write to a temp file and rename over the cache: on NTFS the rename is
 // atomic, so a reader can only ever see one complete generation. This is
 // what makes the startup generator (Configure) and the first-frame
 // fallback (EnsureFields) safe to run against the same file. The unique
 // temp name keeps even a pathological concurrent save from sharing a file.
 const std::wstring tmp=path+L".tmp"+std::to_wstring(GetTickCount64());
 std::ofstream out(std::filesystem::path(tmp),std::ios::binary|std::ios::trunc);
 if(!out)return;
 const CacheHeader header{kCacheMagic,kCacheVersion,kShapeSize,kDetailSize,kWeatherSize};
 out.write(reinterpret_cast<const char*>(&header),sizeof(header));
 out.write(reinterpret_cast<const char*>(shape.rgba.data()),std::streamsize(shape.rgba.size()));
 out.write(reinterpret_cast<const char*>(detail.rgba.data()),std::streamsize(detail.rgba.size()));
 out.write(reinterpret_cast<const char*>(weather.rgba.data()),std::streamsize(weather.rgba.size()));
 out.close();
 if(!out)return;
 std::error_code ec;
 std::filesystem::rename(tmp,path,ec);
 if(ec)std::filesystem::remove(tmp,ec);
}

// The generators produce RGBA bytes; D3DFMT_A8R8G8B8 is BGRA in memory.
bool UploadVolume(IDirect3DDevice9* d,const cloudnoise::Volume3D& v,IDirect3DVolumeTexture9** out)
{
 ComPtr<IDirect3DVolumeTexture9> texture;
 const UINT size=UINT(v.size);
 if(FAILED(d->CreateVolumeTexture(size,size,size,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture.GetAddressOf(),nullptr)))return false;
 D3DLOCKED_BOX box{};
 if(FAILED(texture->LockBox(0,&box,nullptr,0)))return false;
 for(UINT z=0;z<size;++z)
  for(UINT y=0;y<size;++y)
  {
   const uint8_t* src=&v.rgba[(size_t(z)*size*size+size_t(y)*size)*4];
   uint8_t* row=static_cast<uint8_t*>(box.pBits)+size_t(z)*box.SlicePitch+size_t(y)*box.RowPitch;
   for(UINT x=0;x<size;++x)
   {
    row[x*4+0]=src[x*4+2];row[x*4+1]=src[x*4+1];row[x*4+2]=src[x*4+0];row[x*4+3]=src[x*4+3];
   }
  }
 texture->UnlockBox(0);
 *out=texture.Detach();
 return true;
}

bool UploadImage(IDirect3DDevice9* d,const cloudnoise::Image2D& image,IDirect3DTexture9** out)
{
 ComPtr<IDirect3DTexture9> texture;
 const UINT size=UINT(image.width);
 if(FAILED(d->CreateTexture(size,size,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture.GetAddressOf(),nullptr)))return false;
 D3DLOCKED_RECT rect{};
 if(FAILED(texture->LockRect(0,&rect,nullptr,0)))return false;
 for(UINT y=0;y<size;++y)
 {
  const uint8_t* src=&image.rgba[size_t(y)*size*4];
  uint8_t* row=static_cast<uint8_t*>(rect.pBits)+size_t(y)*rect.Pitch;
  for(UINT x=0;x<size;++x)
  {
   row[x*4+0]=src[x*4+2];row[x*4+1]=src[x*4+1];row[x*4+2]=src[x*4+0];row[x*4+3]=src[x*4+3];
  }
 }
 texture->UnlockRect(0);
 *out=texture.Detach();
 return true;
}

void SetSampler(IDirect3DDevice9* d,DWORD stage,DWORD filter,DWORD address)
{
 d->SetSamplerState(stage,D3DSAMP_MINFILTER,filter);
 d->SetSamplerState(stage,D3DSAMP_MAGFILTER,filter);
 d->SetSamplerState(stage,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
 d->SetSamplerState(stage,D3DSAMP_SRGBTEXTURE,FALSE);
 d->SetSamplerState(stage,D3DSAMP_ADDRESSU,address);
 d->SetSamplerState(stage,D3DSAMP_ADDRESSV,address);
 d->SetSamplerState(stage,D3DSAMP_ADDRESSW,address);
}
}

VolumetricClouds& VolumetricClouds::Instance(){static VolumetricClouds clouds;return clouds;}

void VolumetricClouds::ApplyQualityPreset(uint32_t quality)
{
 // Keep march quality a single control: sample count, light samples and
 // base step must agree. Buffer resolution has its own F7 slider and must
 // not be silently changed by this preset (or vice versa).
 static constexpr uint32_t kSteps[]={48,64,96,128};
 static constexpr uint32_t kLightSteps[]={4,5,6,6};
 static constexpr float kBaseSteps[]={130.f,100.f,70.f,55.f};
 const uint32_t level=std::min(quality,3u);
 m_settings.quality=level;
 m_settings.sampleCount=kSteps[level];
 m_settings.lightSampleCount=kLightSteps[level];
 m_settings.baseStep=kBaseSteps[level];
}

void VolumetricClouds::Configure(const std::wstring& basePath)
{
 const std::wstring ini=basePath+L"GraphicsEffects.ini";
 m_logPath=basePath+L"Clouds.log";
 m_shaderCacheDir=renderer::shaderbytecode::Dir(basePath);
 m_cachePath=basePath+L"CloudFields.cache";
 // Fingerprint the raw inputs: a crossing whose place resolves this section
 // to the values already applied re-derives identical settings, and the
 // re-apply below (quality preset, log, field-cache probe, generation thread)
 // is the hitch on the frame the player crosses.
 renderer::locationtuning::SettingsFingerprint fp;
 auto read=[&](const wchar_t* key,int fallback)->int{const int v=static_cast<int>(renderer::locationtuning::ReadInt(L"VolumetricClouds",key,fallback,ini.c_str()));renderer::locationtuning::MixInt(fp,v);return v;};
 m_settings.enabled=read(L"Enabled",1)!=0;
 ApplyQualityPreset(static_cast<uint32_t>(std::clamp(read(L"Quality",3),0,3)));
 // Older INIs shipped explicit SampleCount/LightSampleCount/BaseStep values
 // identical to quality 2. Reading them made the F7 quality slider a no-op
 // on existing installs. Quality is now the sole owner of these three
 // dependent march settings; resolution remains independently adjustable.
 m_settings.resolutionScale=std::clamp(read(L"ResolutionPercent",75),20,100)*.01f;

 m_settings.coverage=std::clamp(read(L"CoveragePercent",50),0,100)*.01f;
 m_settings.density=std::clamp(read(L"DensityPercent",100),10,400)*.01f;
 m_settings.cloudType=std::clamp(read(L"CloudTypePercent",0),-100,100)*.01f;
 m_settings.planetRadius=float(std::clamp(read(L"PlanetRadius",42000),6000,400000));
 m_settings.baseAltitude=float(std::clamp(read(L"BaseAltitude",850),100,20000));
 m_settings.thickness=float(std::clamp(read(L"Thickness",2150),100,12000));
 m_settings.featureSize=float(std::clamp(read(L"FeatureSize",3000),600,40000));
 m_settings.shapeAspect=std::clamp(read(L"ShapeAspectPercent",130),20,400)*.01f;
 m_settings.detailSize=float(std::clamp(read(L"DetailSize",240),40,4000));
 m_settings.weatherSize=float(std::clamp(read(L"WeatherSize",34000),4000,400000));
 m_settings.detailStrength=std::clamp(read(L"DetailPercent",88),0,90)*.01f;
 m_settings.windSpeed=float(std::clamp(read(L"WindSpeed",26),0,400));
 m_settings.windAngle=float(std::clamp(read(L"WindAngle",25),-180,180));
 m_settings.windShear=std::clamp(read(L"WindShearPercent",22),0,200)*.01f;
 m_settings.lightAbsorption=std::clamp(read(L"AbsorptionPercent",38),5,400)*.01f;
 m_settings.sunIntensity=std::clamp(read(L"SunIntensityPercent",130),0,400)*.01f;
 m_settings.ambientIntensity=std::clamp(read(L"AmbientPercent",105),0,250)*.01f;
 m_settings.moonStrength=std::clamp(read(L"MoonStrengthPercent",30),0,150)*.01f;
 m_settings.forwardScatter=std::clamp(read(L"ForwardScatterPercent",78),0,92)*.01f;
 m_settings.backwardScatter=std::clamp(read(L"BackwardScatterPercent",32),0,92)*.01f;
 m_settings.silverLining=std::clamp(read(L"SilverLiningPercent",55),0,400)*.01f;
 m_settings.powder=std::clamp(read(L"PowderPercent",70),0,100)*.01f;
 m_settings.multiScatterAttenuation=std::clamp(read(L"MultiScatterAttenuationPercent",60),5,95)*.01f;
 m_settings.multiScatterContribution=std::clamp(read(L"MultiScatterContributionPercent",92),5,120)*.01f;
 m_settings.multiScatterEccentricity=std::clamp(read(L"MultiScatterEccentricityPercent",55),5,100)*.01f;
 m_settings.stepGrowth=std::clamp(read(L"StepGrowthPermille",10),0,200)*.001f;
 m_settings.coarseScale=float(std::clamp(read(L"CoarseStepScale",3),1,12));
 m_settings.jitter=std::clamp(read(L"JitterPercent",25),0,100)*.01f;
 m_settings.horizonFade=std::clamp(read(L"HorizonFadePermille",18),1,400)*.001f;
 int maxDistance=read(L"MaxDistance",200000);
 // 26,000 was the shipped pre-sky-view default. Upgrade that legacy value
 // so existing installs gain the near-horizon reach as well as fresh installs.
 if(maxDistance==26000)maxDistance=200000;
 m_settings.maxDistance=float(std::clamp(maxDistance,2000,200000));
 m_settings.aerialPerspective=std::clamp(read(L"AerialPerspectivePercent",60),0,100)*.01f;
 m_settings.manualCloudColor=manualcolor::Override::FromPercent(
     read(L"ManualColorRed",255),read(L"ManualColorGreen",255),read(L"ManualColorBlue",255),
     read(L"ManualColorAlphaPercent",0),read(L"ManualColorBrightnessPercent",100));
 m_settings.groundShadows=read(L"GroundShadows",1)!=0;
 m_settings.groundShadowStrength=std::clamp(read(L"GroundShadowPercent",55),0,100)*.01f;
 m_settings.groundShadowExtent=float(std::clamp(read(L"GroundShadowExtent",24000),2000,80000));
 m_settings.sunRays=read(L"SunRaysEnabled",0)!=0;
 m_settings.sunRayStrength=std::clamp(read(L"SunRayPercent",80),0,200)*.01f;
 m_settings.opacity=std::clamp(read(L"OpacityPercent",100),0,300)*.01f;
 m_settings.debugMode=static_cast<CloudDebugMode>(std::clamp(read(L"DebugMode",0),0,4));
 m_settings.zoneClouds=read(L"ZoneClouds",1)!=0;
 m_settings.weatherClouds=read(L"WeatherClouds",1)!=0;
 m_settings.evolutionSpeed=std::clamp(read(L"EvolutionPercent",100),0,400)*.01f;
 m_settings.transitionSeconds=float(std::clamp(read(L"TransitionSeconds",12),0,60));
 if(renderer::locationtuning::SameSettings(m_settingsFingerprint,fp))return;
 // Cold-start field generation is roughly a second of CPU. It used to run on
 // the first frame that would draw clouds - hidden under a loading screen on
 // slow machines, but in-game on a fast one. When the cache is missing, start
 // it on a worker thread at load time instead: same work, started under the
 // game's own loading, and EnsureFields waits for it (or falls back to the
 // old inline generation if it is still not done). The generators are
 // deterministic (fixed seeds), so the two paths can never disagree.
 if(m_settings.enabled&&!m_cachePath.empty()) {
  std::error_code ec;
  // Not exists alone: a settings change can re-enter this block while the
  // first worker is still generating, and two workers writing the same
  // cache is wasted CPU on the crossing's frame.
  auto worker=m_fieldWorker;
  bool expected=false;
  if(!std::filesystem::exists(m_cachePath,ec)&&
     worker->generating.compare_exchange_strong(expected,true,std::memory_order_acq_rel)) {
   const std::wstring cachePath=m_cachePath;
   try {
    std::thread([worker,cachePath]{
     try {
      auto shape=cloudnoise::GenerateShape(int(kShapeSize));
      auto detail=cloudnoise::GenerateDetail(int(kDetailSize));
      auto weather=cloudnoise::GenerateWeather(int(kWeatherSize));
      if(shape.size>0&&detail.size>0&&weather.width>0)
       SaveFields(cachePath,shape,detail,weather);
      else
       worker->failed.store(true,std::memory_order_release);
     } catch (...) {
      // A failed generation costs this launch's clouds only; it must never
      // escape a worker thread and terminate the game process.
      worker->failed.store(true,std::memory_order_release);
     }
     worker->generating.store(false,std::memory_order_release);
    }).detach();
   } catch (...) {
    worker->failed.store(true,std::memory_order_release);
    worker->generating.store(false,std::memory_order_release);
   }
  }
 }
}

void VolumetricClouds::Reset(IDirect3DDevice9* device)
{
 // A device recreation must release resources owned by the discarded device;
 // comparing the owner to the new device and returning would retain dead COM
 // references and make every subsequent cloud allocation fail.
 m_cloudSurface.Reset();m_cloudTexture.Reset();
 m_shadowSurface.Reset();m_shadowTexture.Reset();m_shadowValid=false;
 m_marchShader.Reset();m_compositeShader.Reset();m_shadowShader.Reset();
 m_skyFallbackShader.Reset();m_skyCompositeShader.Reset();m_skyFallbackUsed=false;
 // A new device (or a resize, which is rare) is worth one more attempt.
 m_shaderRetry.Succeed();m_skyShaderRetry.Succeed();
 m_clock.Reset();
 m_owner=nullptr;m_fullWidth=m_fullHeight=m_lowWidth=m_lowHeight=0;m_shadowSize=0;
 // The noise fields live in D3DPOOL_MANAGED and survive a device reset, so
 // an alt-tab does not pay for regenerating them. Only a different device
 // invalidates them.
 if(device==nullptr || (m_fieldsDevice && m_fieldsDevice!=device))
 {
  m_shapeTexture.Reset();m_detailTexture.Reset();m_weatherTexture.Reset();
  m_fieldsDevice=nullptr;m_fieldsReady=false;m_fieldsWaitStart=0;
  m_fieldWorker->failed.store(false,std::memory_order_release);
 }
}

bool VolumetricClouds::EnsureShaders(IDirect3DDevice9* d)
{
 if(m_marchShader&&m_compositeShader&&m_shadowShader)return true;
 // Compiling a 460-slot ps_3_0 shader costs tens of milliseconds. A failed
 // attempt is throttled, not permanently cached: transient device/driver
 // failures get another chance after the retry cooldown without taxing every
 // frame or leaving clouds disabled until the next device reset.
 const uint64_t now=GetTickCount64();
 if(!m_shaderRetry.Open(now))return false;
 if(Compile(d,kCloudMarchSource,m_shaderCacheDir,m_marchShader.GetAddressOf())&&
    Compile(d,kCloudCompositeSource,m_shaderCacheDir,m_compositeShader.GetAddressOf())&&
    Compile(d,kCloudShadowSource,m_shaderCacheDir,m_shadowShader.GetAddressOf()))
 {
  m_shaderRetry.Succeed();
  return true;
 }
 m_marchShader.Reset();m_compositeShader.Reset();m_shadowShader.Reset();
 const bool firstFailure=m_shaderRetry.Fail(now,kOptionalPassRetryMs);
 if(firstFailure&&!m_logPath.empty())
 {
  std::ofstream out(std::filesystem::path(m_logPath),std::ios::app);
  out<<"clouds: pixel shader creation failed; retrying after cooldown\n";
 }
 return false;
}

bool VolumetricClouds::EnsureSkyFallbackShaders(IDirect3DDevice9* d)
{
 if(m_skyFallbackShader&&m_skyCompositeShader)return true;
 const uint64_t now=GetTickCount64();
 if(!m_skyShaderRetry.Open(now))return false;
 bool ok=true;
 if(!m_skyFallbackShader)
  ok=Compile(d,kCloudSkyFallbackSource,m_shaderCacheDir,m_skyFallbackShader.GetAddressOf());
 if(ok&&!m_skyCompositeShader)
  ok=Compile(d,kCloudSkyCompositeSource,m_shaderCacheDir,m_skyCompositeShader.GetAddressOf());
 if(ok){m_skyShaderRetry.Succeed();return true;}
 const bool firstFailure=m_skyShaderRetry.Fail(now,kOptionalPassRetryMs);
 if(firstFailure&&!m_logPath.empty())
 {
  std::ofstream out(std::filesystem::path(m_logPath),std::ios::app);
  out<<"clouds: optional sky-only shader creation failed; normal world cloud shaders remain available; retrying after cooldown\n";
 }
 return false;
}

bool VolumetricClouds::EnsureFields(IDirect3DDevice9* d)
{
 if(m_fieldsReady)return true;
 if(m_fieldWorker->failed.load(std::memory_order_acquire))return false;
 D3DCAPS9 caps{};
 if(FAILED(d->GetDeviceCaps(&caps))||!(caps.TextureCaps&D3DPTEXTURECAPS_VOLUMEMAP)||
    caps.MaxVolumeExtent<kShapeSize)
 {
  m_fieldWorker->failed.store(true,std::memory_order_release);
  return false;
 }
 cloudnoise::Volume3D shape,detail;
 cloudnoise::Image2D weather;
 // Roughly a second of work on a cold start, so it is written next to the
 // DLL and read back on every later launch. The first launch generates it
 // on a worker thread at load time (see Configure). If the cache is still
 // missing when the first cloud frame arrives, skip this frame and retry
 // next - the worker started seconds ago and the render thread must not
 // block on it. Past a five-second bound the worker is pathologically slow
 // or dead, and the legacy inline generation runs (its atomic save and
 // deterministic content keep a late worker write harmless). Do not reopen
 // and scan the multi-megabyte cache on every frame while that worker runs.
 const bool generating=m_fieldWorker->generating.load(std::memory_order_acquire);
 const uint64_t now=GetTickCount64();
 if(generating) {
  if(!m_fieldsWaitStart)m_fieldsWaitStart=now;
  if(now-m_fieldsWaitStart<5000)return false;
 } else {
  m_fieldsWaitStart=0;
 }
 if(!LoadFields(m_cachePath,shape,detail,weather))
 {
  shape=cloudnoise::GenerateShape(int(kShapeSize));
  detail=cloudnoise::GenerateDetail(int(kDetailSize));
  weather=cloudnoise::GenerateWeather(int(kWeatherSize));
  if(shape.size<=0||detail.size<=0||weather.width<=0){m_fieldWorker->failed.store(true,std::memory_order_release);return false;}
  SaveFields(m_cachePath,shape,detail,weather);
 }
 if(!UploadVolume(d,shape,m_shapeTexture.ReleaseAndGetAddressOf())||
    !UploadVolume(d,detail,m_detailTexture.ReleaseAndGetAddressOf())||
    !UploadImage(d,weather,m_weatherTexture.ReleaseAndGetAddressOf()))
 {
  m_shapeTexture.Reset();m_detailTexture.Reset();m_weatherTexture.Reset();
  m_fieldWorker->failed.store(true,std::memory_order_release);
  return false;
 }
 m_fieldsDevice=d;
 m_fieldsReady=true;
 return true;
}

bool VolumetricClouds::EnsureResources(IDirect3DDevice9* d,uint32_t w,uint32_t h)
{
 const uint32_t lw=std::max(1u,uint32_t(float(w)*m_settings.resolutionScale));
 const uint32_t lh=std::max(1u,uint32_t(float(h)*m_settings.resolutionScale));
 if(m_owner==d&&m_fullWidth==w&&m_fullHeight==h&&m_lowWidth==lw&&m_lowHeight==lh&&m_cloudTexture)return true;
 // A backbuffer resize only invalidates size-dependent targets. Keep the
 // expensive raymarch shaders and managed noise fields: resetting/recompiling
 // all three cloud shaders on every resolution switch caused a long first-frame
 // hitch and could make clouds appear to lose their shader transiently.
 m_cloudSurface.Reset();m_cloudTexture.Reset();
 m_shadowSurface.Reset();m_shadowTexture.Reset();m_shadowValid=false;m_shadowSize=0;
 m_owner=d;m_fullWidth=w;m_fullHeight=h;m_lowWidth=lw;m_lowHeight=lh;
 auto tex=[&](D3DFORMAT format){
  m_cloudTexture.Reset();m_cloudSurface.Reset();
  return SUCCEEDED(d->CreateTexture(lw,lh,1,D3DUSAGE_RENDERTARGET,format,D3DPOOL_DEFAULT,m_cloudTexture.GetAddressOf(),nullptr))&&
         SUCCEEDED(m_cloudTexture->GetSurfaceLevel(0,m_cloudSurface.GetAddressOf()));
 };
 // Half-float keeps bright sunlit tops and the transmittance channel
 // separable; the 8-bit fallback only costs banding on very bright decks.
 if(!tex(D3DFMT_A16B16G16R16F)&&!tex(D3DFMT_A8R8G8B8))
 {
  m_owner=nullptr;m_fullWidth=m_fullHeight=m_lowWidth=m_lowHeight=0;
  return false;
 }
 m_shadowSize=512;
 if(FAILED(d->CreateTexture(m_shadowSize,m_shadowSize,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,m_shadowTexture.GetAddressOf(),nullptr))||
    FAILED(m_shadowTexture->GetSurfaceLevel(0,m_shadowSurface.GetAddressOf())))
 {
  // Shadows are optional; the deck itself still renders without them.
  m_shadowTexture.Reset();m_shadowSurface.Reset();m_shadowSize=0;
 }
 return true;
}

void VolumetricClouds::DrawScreenQuad(IDirect3DDevice9* d,uint32_t w,uint32_t h)
{
 struct V{float x,y,z,rhw,u,v;};float fw=float(w)-.5f,fh=float(h)-.5f;
 V q[]={{-.5f,-.5f,0,1,0,0},{fw,-.5f,0,1,1,0},{-.5f,fh,0,1,0,1},{fw,fh,0,1,1,1}};
 d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(V));
}

bool VolumetricClouds::RenderSkyFallback(IDirect3DDevice9* d,const FrameContext& f,
                                              IDirect3DSurface9* target,
                                              const VolumetricCloudSettings& settings,
                                              const CloudMotion& motion)
{
 if(!d||!target||!EnsureSkyFallbackShaders(d))return false;
 const float p00=f.projUnpack[2],p11=f.projUnpack[3];
 if(!std::isfinite(p00)||!std::isfinite(p11)||p00<=.01f||p11<=.01f)return false;

 // The branch never reads the retained eye, reconstructs a world point, or
 // publishes a shadow. Its only camera input is the fresh, corroborated VIEW
 // rotation and a previously validated focal scale used to form a screen ray.
 m_shadowValid=false;m_shadowArea={};m_lightProjection={};
 ScopedPassState state(d);
 DepthCapture::Instance().RawSetDepth(d,nullptr);
 for(auto s:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,
             D3DRS_SCISSORTESTENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_SRGBWRITEENABLE,
             D3DRS_ALPHABLENDENABLE})d->SetRenderState(s,FALSE);
 d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
 d->SetRenderState(D3DRS_COLORWRITEENABLE,0xF);
 d->SetVertexShader(nullptr);

 D3DVIEWPORT9 low{0,0,m_lowWidth,m_lowHeight,0,1};
 {
  ScopedGpuTimer timer(d,GpuPerfStage::VolumetricClouds);
  d->SetViewport(&low);d->SetRenderTarget(0,m_cloudSurface.Get());
  d->SetPixelShader(m_skyFallbackShader.Get());
  d->SetTexture(0,m_shapeTexture.Get());SetSampler(d,0,D3DTEXF_LINEAR,D3DTADDRESS_WRAP);
  d->SetTexture(1,m_detailTexture.Get());SetSampler(d,1,D3DTEXF_LINEAR,D3DTADDRESS_WRAP);
  const float inv[3][4]={{f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},
                         {f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},
                         {f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0}};
  const float projection[4]={p00,p11,0,0};
  const float featureSize=std::max(settings.featureSize,1.f);
  const float detailSize=std::max(settings.detailSize,1.f);
  // Screen-direction recovery has no eye with which to intersect the world
  // layer or weather map. Use a fixed, medium angular footprint for shape and
  // detail: 1500 yards made one broad/soft patch in the reported view, while
  // planet-radius scaling made many tiny camera-following tiles. The user
  // feature/detail sizes still control their relative scales.
  const float angularScale=kSkyFallbackReferenceDistance/featureSize;
  const float detailAngularScale=kSkyFallbackReferenceDistance/detailSize;
  const float scales[4]={angularScale,std::max(settings.shapeAspect,.05f),
                         detailAngularScale,std::clamp(settings.detailStrength,0.f,.9f)};
  const float motionConstants[4]={float(motion.shape[0]),float(motion.shape[1]),0.f,0.f};
  const float phase[4]={float(motion.detail[0]),float(motion.detail[1]),
                        float(motion.shapeZ),float(motion.detailZ)};
  const float depthSpan=std::clamp(settings.thickness,100.f,12000.f);
  const float volume[4]={depthSpan/featureSize,depthSpan/detailSize,0.f,0.f};
  const float windRadians=settings.windAngle*.01745329252f;
  const float shear[4]={std::cos(windRadians)*settings.windShear,
                        std::sin(windRadians)*settings.windShear,
                        std::cos(windRadians)*settings.windShear*featureSize/detailSize,
                        std::sin(windRadians)*settings.windShear*featureSize/detailSize};
  const float fallbackCoverage=SkyFallbackCoverage(settings);
  const float fallbackCloudType=SkyFallbackCloudType(settings);
  const float coverageConstants[4]={fallbackCoverage,fallbackCloudType,0.f,0.f};
  const auto& envFog=EnvironmentFogCapture::Instance().State();
  const float ambientLevel=std::clamp(std::max(std::clamp(f.daylightFactor,0.f,1.f),
                                                   std::clamp(f.moonlightFactor,0.f,1.f)*.5f),.2f,1.f);
  const float skyR=envFog.colorValid?envFog.ambientFogColor[0]:.42f;
  const float skyG=envFog.colorValid?envFog.ambientFogColor[1]:.56f;
  const float skyB=envFog.colorValid?envFog.ambientFogColor[2]:.80f;
  const auto color=manualcolor::Multiplier(settings.manualCloudColor);
  const float material[4]={std::clamp(std::max(skyR*ambientLevel,kCloudSkyAmbientFloor[0])*color[0],0.f,2.f),
                           std::clamp(std::max(skyG*ambientLevel,kCloudSkyAmbientFloor[1])*color[1],0.f,2.f),
                           std::clamp(std::max(skyB*ambientLevel,kCloudSkyAmbientFloor[2])*color[2],0.f,2.f),
                           std::clamp(settings.density,0.f,3.f)*std::clamp(settings.opacity,0.f,3.f)};
  Vec3 light{0,0,1};
  if(f.directLightValid&&IsUsableCelestialDirection(f.directLightDirectionWorld))
   light=Normalize(f.directLightDirectionWorld);
  const float silver=f.directLightValid&&f.directLightDirectionConfirmed
      ?std::clamp(settings.silverLining,0.f,3.f):0.f;
  const float lighting[4]={light.x,light.y,light.z,silver};
  d->SetPixelShaderConstantF(0,inv[0],3);d->SetPixelShaderConstantF(3,projection,1);
  d->SetPixelShaderConstantF(4,scales,1);d->SetPixelShaderConstantF(5,motionConstants,1);
  d->SetPixelShaderConstantF(6,phase,1);d->SetPixelShaderConstantF(7,coverageConstants,1);
  d->SetPixelShaderConstantF(8,material,1);d->SetPixelShaderConstantF(9,lighting,1);
  d->SetPixelShaderConstantF(10,volume,1);d->SetPixelShaderConstantF(11,shear,1);
  DrawScreenQuad(d,m_lowWidth,m_lowHeight);
  for(DWORD stage=0;stage<3;++stage)d->SetTexture(stage,nullptr);
 }

 D3DVIEWPORT9 full{0,0,m_fullWidth,m_fullHeight,0,1};
 {
  ScopedGpuTimer timer(d,GpuPerfStage::VolumetricClouds);
  d->SetViewport(&full);d->SetRenderTarget(0,target);
  d->SetPixelShader(m_skyCompositeShader.Get());
  d->SetTexture(0,f.sceneColor);SetSampler(d,0,D3DTEXF_POINT,D3DTADDRESS_CLAMP);
  d->SetTexture(1,m_cloudTexture.Get());SetSampler(d,1,D3DTEXF_LINEAR,D3DTADDRESS_CLAMP);
  d->SetTexture(2,f.depthTexture);SetSampler(d,2,D3DTEXF_POINT,D3DTADDRESS_CLAMP);
  // A missing mask reads as zero, just like the normal composite; a captured
  // native-water mask makes every covered pixel an exact scene-color identity.
  d->SetTexture(3,f.waterMaskTexture);SetSampler(d,3,D3DTEXF_POINT,D3DTADDRESS_CLAMP);
  const float depthRange[4]={f.viewport.MinZ,f.depthMaxZ-f.viewport.MinZ,f.depthMaxZ,0.f};
  d->SetPixelShaderConstantF(0,depthRange,1);
  DrawScreenQuad(d,m_fullWidth,m_fullHeight);
  for(DWORD stage=0;stage<4;++stage)d->SetTexture(stage,nullptr);
 }
 m_skyFallbackUsed=true;
 return true;
}

bool VolumetricClouds::Render(IDirect3DDevice9* d,const FrameContext& f,IDirect3DSurface9* target)
{
 m_shadowValid=false;
 m_skyFallbackUsed=false;
 VolumetricCloudSettings settings=m_settings;
 if(!settings.enabled)return false;
 const bool diagnosticFrame=(++m_renderFrames==1||m_renderFrames%600==0);
 const bool freshCameraPose=f.cameraValid&&!f.cameraPoseStale;
 const bool freshOrientation=f.cameraOrientationFresh;
 const bool knownWorldDraw=f.worldTerrainSeenThisFrame||f.worldWaterSeenThisFrame;
 const bool normalWorldPath=CanRenderWorldClouds(f.worldScene.state,knownWorldDraw,freshCameraPose);
 const bool skyFallback=!normalWorldPath&&
     CanRenderSkyCloudFallback(f.worldScene.state,freshOrientation,freshCameraPose);
 float thickness=std::max(settings.thickness,20.f);
 float layerTop=settings.baseAltitude+thickness;
 const char* cameraLayer=skyFallback?"screen-only":"unavailable";
 auto refreshLayerDiagnostic=[&]{
  thickness=std::max(settings.thickness,20.f);
  layerTop=settings.baseAltitude+thickness;
  if(!skyFallback&&freshCameraPose)
   cameraLayer=f.cameraPosition.z<settings.baseAltitude?"below":
       (f.cameraPosition.z>layerTop?"above":"inside");
 };
 if(!skyFallback&&freshCameraPose)refreshLayerDiagnostic();
 auto logState=[&](const char* event){
  const bool stateChanged=m_lastDiagnosticState!=event;
  if((!diagnosticFrame&&!stateChanged)||m_logPath.empty())return;
  m_lastDiagnosticState=event;
  std::ofstream out(std::filesystem::path(m_logPath),std::ios::app);
  if(!out)return;
  const auto& celestialTrack = CelestialTracker::Instance().Diagnostics();
  out<<"clouds "<<event<<" scene="<<int(f.worldScene.state)
     <<" worldDraw="<<(knownWorldDraw?1:0)<<" freshCamera="<<(freshCameraPose?1:0)
     <<" orientationFresh="<<(freshOrientation?1:0)
     <<" skyFallbackEligible="<<(skyFallback?1:0)
     <<" cameraStale="<<(f.cameraPoseStale?1:0)<<" eyeCurrent="<<(f.cameraEyeObservedThisFrame?1:0)
     <<" clientEye=("<<(f.clientCameraEyeReadThisFrame?1:0)<<','
     <<(f.clientCameraEyeAcceptedThisFrame?1:0)<<','<<f.clientCameraEyeDistanceYards<<')'
     <<" poseSource="<<int(f.cameraPoseSource)<<" eyeAgeMs="<<f.cameraEyeAgeMs
     <<" skyCandidates="<<int(f.skyPoseCandidateCount)
     <<" skyDisc="<<(f.skyPoseDiscEvidence?1:0)
     <<" skyEyeCandidate="<<(f.skyEyeCandidateObserved?1:0)
     <<'('<<f.skyEyeCandidateDistanceYards<<"yd)"
     <<" skyObserve="<<f.SkyObservationName()
     <<" camCap=("<<f.cameraCaptureAttempts<<','<<f.cameraCaptureAccepted<<','
     <<f.cameraCaptureStateRejected<<','<<f.cameraCaptureNonFinite<<','
     <<f.cameraCaptureNotFullFrame<<','<<f.cameraCaptureProjectionRejected<<','
     <<f.cameraCaptureBasisRejected<<','<<f.cameraCaptureContinuityRejected<<','
     <<f.cameraCaptureProjectionScaleMatches<<','<<f.cameraCaptureProjectionEyeMatches<<','
     <<f.cameraCaptureProjectionFallbackCandidates<<')';
  if(f.skyProjectionAvailable)
   out<<" skyProj="<<f.skyProjection[0]<<','<<f.skyProjection[1]<<','
      <<f.skyProjection[2]<<','<<f.skyProjection[3]<<','
      <<f.skyProjection[4]<<','<<f.skyProjection[5];
  else
   out<<" skyProj=none";
  out<<" skyProjCacheOk="<<(f.skyProjectionCacheUsable?1:0)
     <<" skyProjCache=("<<f.skyProjectionCacheAgeMs<<','
     <<f.skyProjectionCacheScaleX<<','<<f.skyProjectionCacheScaleY<<')'
     <<" skyProjVp=("<<f.skyProjectionCacheMinZ<<','<<f.skyProjectionCacheMaxZ<<';'
     <<f.skyProjectionObservedMinZ<<','<<f.skyProjectionObservedMaxZ<<')'
     <<" skyProjCached="<<(f.skyProjectionFallbackUsed?1:0)
     <<" cameraLayer="<<cameraLayer<<" forwardZ="<<f.inverseView.m[2][2]
     <<" base="<<settings.baseAltitude<<" top="<<layerTop;
  if(skyFallback)out<<" cameraZ=unused";
  else out<<" cameraZ="<<f.cameraPosition.z;
  out<<" directLight=(moon="<<(f.directLightIsMoon?1:0)<<",strength="<<f.directLightIntensity
     <<",day="<<f.daylightFactor<<",night="<<f.moonlightFactor
     <<",confirmed="<<(f.directLightDirectionConfirmed?1:0)
     <<",dir="<<f.directLightDirectionWorld.x<<','<<f.directLightDirectionWorld.y<<','
     <<f.directLightDirectionWorld.z<<",color="<<f.directionalLightColor.x<<','
     <<f.directionalLightColor.y<<','<<f.directionalLightColor.z<<')';
  if(skyFallback){
   const auto& fieldMotion=m_dynamics.Motion();
   const auto& fog=EnvironmentFogCapture::Instance().State();
   const auto tint=manualcolor::Multiplier(settings.manualCloudColor);
   const float fallbackAmbientLevel=std::clamp(std::max(std::clamp(f.daylightFactor,0.f,1.f),
                                                          std::clamp(f.moonlightFactor,0.f,1.f)*.5f),.2f,1.f);
   const float fallbackSkyR=fog.colorValid?fog.ambientFogColor[0]:.42f;
   const float fallbackSkyG=fog.colorValid?fog.ambientFogColor[1]:.56f;
   const float fallbackSkyB=fog.colorValid?fog.ambientFogColor[2]:.80f;
   const float fallbackCloudR=std::max(fallbackSkyR*fallbackAmbientLevel,kCloudSkyAmbientFloor[0]);
   const float fallbackCloudG=std::max(fallbackSkyG*fallbackAmbientLevel,kCloudSkyAmbientFloor[1]);
   const float fallbackCloudB=std::max(fallbackSkyB*fallbackAmbientLevel,kCloudSkyAmbientFloor[2]);
   out<<" skyField=(samples=16,coverageSource=climate-mean,coverage="
      <<SkyFallbackCoverage(settings)<<",type="<<SkyFallbackCloudType(settings)
      <<",typeBias="<<settings.cloudType<<",weatherField=unused"
      <<",feature="<<settings.featureSize<<",detail="<<settings.detailSize
      <<",angularScale="<<kSkyFallbackReferenceDistance/std::max(settings.featureSize,1.f)
      <<",detailAngularScale="<<kSkyFallbackReferenceDistance/std::max(settings.detailSize,1.f)
      <<",shapeDepth="<<std::clamp(settings.thickness,100.f,12000.f)/std::max(settings.featureSize,1.f)
      <<",detailDepth="<<std::clamp(settings.thickness,100.f,12000.f)/std::max(settings.detailSize,1.f)
      <<",density="<<settings.density<<",opacity="<<settings.opacity
      <<",weather="<<int(settings.weather)<<",wind="<<settings.windSpeed
      <<",windAngle="<<settings.windAngle<<",windShear="<<settings.windShear
      <<",evolution="<<settings.evolutionSpeed<<",shapePhase="<<fieldMotion.shapeZ
      <<",detailPhase="<<fieldMotion.detailZ<<",weatherPhaseUnused="<<fieldMotion.weather[0]<<','
      <<fieldMotion.weather[1]<<",ambientValid="<<(fog.colorValid?1:0)
      <<",ambientLevel="<<fallbackAmbientLevel<<",ambient="<<fog.ambientFogColor[0]<<','
      <<fog.ambientFogColor[1]<<','<<fog.ambientFogColor[2]
      <<",cloudAmbient="<<fallbackCloudR<<','<<fallbackCloudG<<','<<fallbackCloudB
      <<",manual="<<tint[0]<<','<<tint[1]<<','<<tint[2]<<')';
  }
  out<<" trackedSun="<<(f.sunBillboardObserved?1:0)<<'('<<f.sunBillboardScreenX<<','
     <<f.sunBillboardScreenY<<','<<f.sunBillboardViewDepth<<')'
     <<" trackedMoon="<<(f.moonBillboardObserved?1:0)<<'('<<f.moonBillboardScreenX<<','
     <<f.moonBillboardScreenY<<','<<f.moonBillboardViewDepth<<')'
     <<" bodyTrackValid=(sun="<<(CelestialTracker::Instance().Sun().worldDirectionValid?1:0)
     <<",moon="<<(CelestialTracker::Instance().Moon().worldDirectionValid?1:0)<<')'
     <<" celTrack=("<<celestialTrack.drawCalls<<','<<celestialTrack.primitiveRejected<<','
     <<celestialTrack.vertexShaderDraws<<','<<celestialTrack.pixelShaderDraws<<','
     <<celestialTrack.fixedFunctionDraws<<','<<celestialTrack.alphaTestDraws<<','
     <<celestialTrack.texturedDraws<<','<<celestialTrack.unknownTextureSize<<','
     <<celestialTrack.largeTextureDraws<<','<<celestialTrack.moonTextureDraws<<','
     <<celestialTrack.sunSignatureDraws<<','<<celestialTrack.sunAcceptedDraws<<','
     <<celestialTrack.moonSignatureDraws<<','<<celestialTrack.moonAcceptedDraws<<')'
     <<" moonCandidate=("<<celestialTrack.moonTransformCandidates<<','
     <<celestialTrack.lastMoonWorldScaleSq<<','
     <<(celestialTrack.lastMoonViewTranslationZero?1:0)<<','
     <<celestialTrack.lastMoonViewTranslationZ<<','
     <<celestialTrack.lastMoonCameraSpaceZ<<')'
     <<" glareSource="<<(f.celestialIntensity>.001f?(f.celestialIsMoon?"moon":"sun"):"none")
     <<" glare=("<<f.sunScreenX<<','<<f.sunScreenY<<')'
     <<" sunDisc="<<(f.sunDiscValid?1:0)<<'('<<f.sunDiscScreenX<<','<<f.sunDiscScreenY<<','<<f.sunDiscViewDepth<<')'
     <<" sunRaw=("<<f.sunDiscExpectedRawDepth<<','<<f.sunDiscRawDepthTolerance<<')'
     <<" moonDisc="<<(f.moonDiscValid?1:0)<<'('<<f.moonDiscScreenX<<','<<f.moonDiscScreenY<<','<<f.moonDiscViewDepth<<')'
     <<"\n";
 };
 // World-space marching still needs the unchanged fresh-eye gate. The
 // separate screen-direction recovery is allowed only for a confirmed
 // in-world frame with a current, corroborated orientation; menu and unknown
 // scenes never reach it.
 if(!normalWorldPath&&!skyFallback){
  const bool lacksWorldContext=f.worldScene.state==WorldSceneState::OutOfWorld||
      (!knownWorldDraw&&f.worldScene.state!=WorldSceneState::InWorld);
  logState(lacksWorldContext?"skip=world-gate":"skip=stale-camera");return false;
 }
 if(!d||!target||!f.cameraValid||!f.depthAvailable||!f.depthTexture||!f.sceneColor){
  logState("skip=frame-input");return false;
 }
 if(skyFallback&&(!std::isfinite(f.projUnpack[2])||!std::isfinite(f.projUnpack[3])||
                  f.projUnpack[2]<=.01f||f.projUnpack[3]<=.01f)){
  logState("skip=sky-projection");return false;
 }

 // Advance before the clear-sky early-out: an empty deck must still be
 // able to develop a storm. The clock bounds loading/focus gaps.
 // Live frames use the shared boundary delta and snapshot, not a second
 // wind clock. Retrying a cloud pass in one frame cannot advance it twice.
 const float dt=f.wind.sequence ? (f.wind.sequence!=m_windSequence?f.wind.deltaSeconds:0.f) :
     m_clock.Observe(PerformanceCounter(),PerformanceFrequency());
 m_windSequence=f.wind.sequence;
 static_cast<CloudShape&>(settings)=m_dynamics.Update(m_settings,f.environment,
     settings.zoneClouds,settings.weatherClouds?settings.weather:CloudWeather::Clear,
     settings.weatherClouds?settings.weatherStrength:0.f,settings.transitionSeconds,dt,f.wind);
 if(!skyFallback)refreshLayerDiagnostic();
 if(settings.coverage<=0.f||settings.density<=0.f){logState("skip=empty-cloud-field");return false;}
 const auto& motion=m_dynamics.Motion();

 D3DSURFACE_DESC desc{};
 if(FAILED(target->GetDesc(&desc))||!EnsureResources(d,desc.Width,desc.Height)){
  logState("skip=resources");return false;
 }
 if(skyFallback){
  if(!EnsureFields(d)||!RenderSkyFallback(d,f,target,settings,motion)){
   logState("skip=sky-fallback-resources");return false;
  }
  logState("render=sky-fallback");
  return true;
 }
 if(!EnsureShaders(d)||!EnsureFields(d)){logState("skip=resources");return false;}

 const float featureSize=std::max(settings.featureSize,1.f);
 const float detailSize=std::max(settings.detailSize,1.f);
 const float weatherSize=std::max(settings.weatherSize,1.f);
 const float innerRadius=settings.planetRadius+settings.baseAltitude;
 const float outerRadius=innerRadius+thickness;
 // Keep the existing ground-shadow parameters unchanged. The visible deck
 // uses fixed world-Z bounds instead of these legacy spherical coordinates.
 const float centreZ=-settings.planetRadius;
 const float cameraRadius=f.cameraPosition.z-centreZ;
 const float region=cameraRadius<innerRadius?1.f:(cameraRadius>outerRadius?2.f:0.f);

 const float windRadians=settings.windAngle*.01745329252f;
 const float coverage=std::clamp(settings.coverage,0.f,1.f);
 // The slider slides a window along the weather map's equalised coverage
 // channel. The high edge closes QUADRATICALLY so that the top of the
 // slider is a genuine overcast lid: with a linear high edge the window
 // still ends at .6 and half the sky keeps broken cloud no matter how far
 // the player pushes it.
 const float windowLow=1.f-coverage*1.5f;
 const float windowHigh=1.f-coverage*coverage*1.05f;

 // Zone-authored fog colour doubles as the sky ambient so the deck belongs
 // to the same palette as the atmosphere below it.
 const auto& envFog=EnvironmentFogCapture::Instance().State();
 const float skyR=envFog.colorValid?envFog.ambientFogColor[0]:.42f;
 const float skyG=envFog.colorValid?envFog.ambientFogColor[1]:.56f;
 const float skyB=envFog.colorValid?envFog.ambientFogColor[2]:.80f;
 const auto cloudColourMultiplier=manualcolor::Multiplier(settings.manualCloudColor);
 const float daylight=std::clamp(f.daylightFactor,0.f,1.f);
 const float moonlight=std::clamp(f.moonlightFactor,0.f,1.f);
 const float ambientLevel=std::clamp(std::max(daylight,moonlight*.5f),.2f,1.f);
 // Apply day/night scaling once, and keep a small zone-independent floor for
 // clouds alone when native ambient is black. The floor is deliberately
 // subdued and cool, then still passes through the user's manual tint.
 const float cloudSkyR=std::max(skyR*ambientLevel,kCloudSkyAmbientFloor[0]);
 const float cloudSkyG=std::max(skyG*ambientLevel,kCloudSkyAmbientFloor[1]);
 const float cloudSkyB=std::max(skyB*ambientLevel,kCloudSkyAmbientFloor[2]);
 const float cloudGroundR=std::max(skyR*ambientLevel*.62f,kCloudGroundAmbientFloor[0]);
 const float cloudGroundG=std::max(skyG*ambientLevel*.52f,kCloudGroundAmbientFloor[1]);
 const float cloudGroundB=std::max(skyB*ambientLevel*.45f,kCloudGroundAmbientFloor[2]);
 // Direct light comes from the stabilized signal when the compositor
 // filled it: the disc-draw signal (celestialIntensity) toggles as the
 // camera pans the disc in and out of frame and made the deck blink
 // lit/unlit. Frames built outside the compositor (regression tests)
 // have no stabilized value and keep the legacy behaviour.
 const bool moon=f.directLightValid?f.directLightIsMoon:f.celestialIsMoon;
 const float directIntensity=(f.directLightValid?f.directLightIntensity:f.celestialIntensity)*(moon?settings.moonStrength:1.f);
 // The tracked light colour is the client's own directional colour; fall
 // back to a neutral daylight/moonlight tint when it has not been seen.
 const float lightLength=std::sqrt(f.directionalLightColor.x*f.directionalLightColor.x+
                                   f.directionalLightColor.y*f.directionalLightColor.y+
                                   f.directionalLightColor.z*f.directionalLightColor.z);
 const bool useTracked=lightLength>.05f;
 const float sunR=useTracked?std::clamp(f.directionalLightColor.x,0.f,2.f):(moon?.62f:1.f);
 const float sunG=useTracked?std::clamp(f.directionalLightColor.y,0.f,2.f):(moon?.72f:.97f);
 const float sunB=useTracked?std::clamp(f.directionalLightColor.z,0.f,2.f):(moon?.92f:.86f);

 const auto& lightWorld=f.directLightValid?f.directLightDirectionWorld:f.sunDirectionWorld;
 float lightDir[3]={lightWorld.x,lightWorld.y,lightWorld.z};
 // A frame without a confirmed disc still needs a usable march direction.
 if(std::abs(lightDir[0])+std::abs(lightDir[1])+std::abs(lightDir[2])<.01f){lightDir[0]=0;lightDir[1]=0;lightDir[2]=1;}

 const float directionLength=std::sqrt(lightDir[0]*lightDir[0]+lightDir[1]*lightDir[1]+lightDir[2]*lightDir[2]);
 for(float& component:lightDir) component/=directionLength;

 // The shadow map is snapped to its own texel grid: without that it
 // shimmers as the camera moves, because every frame resamples the same
 // cloud at a slightly different offset.
 const float shadowExtent=std::max(settings.groundShadowExtent,100.f);
 const float shadowTexel=shadowExtent/float(std::max(m_shadowSize,1u));
 const bool shadowsOn=m_shadowSurface&&lightDir[2]>.03f&&
  ((settings.groundShadows&&settings.groundShadowStrength>0.f)||(settings.sunRays&&settings.sunRayStrength>0.f));
 // Strength fades in with sun elevation and with the stabilized light
 // instead of snapping at the .03 gate: near the horizon (or when the
 // direction source changed by a degree) the whole world's cloud shadow
 // used to toggle between full strength and none between two frames.
 const float shadowStrength=settings.groundShadowStrength*CloudShadowElevationFade(lightDir[2])
  *(f.directLightValid?std::clamp(f.directLightIntensity,0.f,1.f):1.f);
 // Centre coverage on the camera projected onto the same fixed plane.
 const float slopeX=lightDir[0]/std::max(lightDir[2],.03f);
 const float slopeY=lightDir[1]/std::max(lightDir[2],.03f);
 m_shadowOrigin[0]=std::floor((f.cameraPosition.x-f.cameraPosition.z*slopeX-shadowExtent*.5f)/shadowTexel)*shadowTexel;
 m_shadowOrigin[1]=std::floor((f.cameraPosition.y-f.cameraPosition.z*slopeY-shadowExtent*.5f)/shadowTexel)*shadowTexel;
 m_shadowArea={m_shadowOrigin[0],m_shadowOrigin[1],1.f/shadowExtent,CloudShadowElevationFade(lightDir[2])};
 m_lightProjection={slopeX,slopeY,settings.baseAltitude,thickness};

 if(diagnosticFrame&&!m_logPath.empty()){
  std::ofstream out(std::filesystem::path(m_logPath),std::ios::app);
  // `sun` is the world direction the deck is lit from and the direction the
  // ground shadow is projected along, and `stable` says where it came from
  // (1 = the tracked disc, 0 = the captured light constant). Panning the
  // camera must not move `sun`; if it does, this line is where to see it.
  // An upward ray from above this fixed world-Z layer correctly misses it.
  // Record the camera/layer relationship and forward pitch so a sky-only
  // disappearance at a floating map can be distinguished from a render gate.
  out<<"clouds base="<<settings.baseAltitude<<" thickness="<<thickness<<" top="<<layerTop
     <<" cameraLayer="<<cameraLayer<<" forwardZ="<<f.inverseView.m[2][2]
     <<" coverage="<<settings.coverage
     <<" type="<<settings.cloudType<<" feature="<<settings.featureSize
     <<" weather="<<int(settings.weather)<<" wet="<<m_dynamics.WeatherAmount()
     <<" evolution="<<settings.evolutionSpeed<<" phase="<<motion.shapeZ
     <<" density="<<settings.density<<" steps="<<settings.sampleCount<<" region="<<region
     <<" moon="<<(moon?1:0)<<" intensity="<<directIntensity<<" shadows="<<(shadowsOn?1:0)
     <<" sun=("<<lightDir[0]<<','<<lightDir[1]<<','<<lightDir[2]<<") directionSource="
     <<(f.directLightDirectionConfirmed?"tracked":"overhead")
     <<" shade="<<shadowStrength
     <<" lightInput=(moon="<<(moon?1:0)<<",intensity="<<directIntensity
     <<",day="<<daylight<<",night="<<moonlight<<",color="<<f.directionalLightColor.x<<','
     <<f.directionalLightColor.y<<','<<f.directionalLightColor.z<<')'
     <<" ambient=("<<skyR<<','<<skyG<<','<<skyB<<") ambientLevel="<<ambientLevel
     <<" cloudAmbient=("<<cloudSkyR<<','<<cloudSkyG<<','<<cloudSkyB<<')'
     <<" clientEye=("<<(f.clientCameraEyeReadThisFrame?1:0)<<','
     <<(f.clientCameraEyeAcceptedThisFrame?1:0)<<','<<f.clientCameraEyeDistanceYards<<')'
     <<" eyeAgeMs="<<f.cameraEyeAgeMs<<" poseSource="<<int(f.cameraPoseSource)
     <<" glareSource="<<(f.celestialIntensity>.001f?(f.celestialIsMoon?"moon":"sun"):"none")
     <<" glare=("<<f.sunScreenX<<','<<f.sunScreenY<<')'
     <<" sunDisc="<<(f.sunDiscValid?1:0)<<'('<<f.sunDiscScreenX<<','<<f.sunDiscScreenY<<','<<f.sunDiscViewDepth<<')'
     <<" sunRaw=("<<f.sunDiscExpectedRawDepth<<','<<f.sunDiscRawDepthTolerance<<')'
     <<" moonDisc="<<(f.moonDiscValid?1:0)<<'('<<f.moonDiscScreenX<<','<<f.moonDiscScreenY<<','<<f.moonDiscViewDepth<<')'
     <<" camera=("<<f.cameraPosition.x<<','<<f.cameraPosition.y<<','<<f.cameraPosition.z<<")\n";
 }

 // Shared field constants: the march and the shadow pass read the same
 // registers so there is exactly one description of the cloud field.
 const float c1[4]={lightDir[0],lightDir[1],lightDir[2],directIntensity};
 const float c7[4]={innerRadius,outerRadius,1.f/thickness,settings.maxDistance};
 const float c8[4]={f.cameraPosition.x,f.cameraPosition.y,centreZ,region};
 const float c9[4]={windowLow,1.f/std::max(windowHigh-windowLow,1e-4f),std::max(settings.cloudType,0.f),
                    settings.aerialPerspective};
 const float c10[4]={1.f/featureSize,std::max(settings.shapeAspect,.05f)/featureSize,
                     1.f/detailSize,settings.detailStrength};
 // The light march spends its last iteration on a long reach sample; how
 // long is a constant, not something to recompute per sample.
 const float lightSteps=float(std::clamp(settings.lightSampleCount,1u,6u));
 const float c11[4]={float(motion.shape[0]),float(motion.shape[1]),1.f/weatherSize,lightSteps*3.f};
 const float c12[4]={float(motion.weather[0]),float(motion.weather[1]),
                     std::cos(windRadians)*settings.windShear*featureSize,
                     std::sin(windRadians)*settings.windShear*featureSize};
 // Less sky reaches the base of an overcast lid than the base of a lone
 // cumulus, so the ambient term closes down as the deck fills in. Without
 // it a full-coverage sky renders as a bright white ceiling.
 const float ambientOcclusion=1.f-coverage*.45f;
 const float c23[4]={float(motion.shapeZ),float(motion.detailZ),motion.variation,
                      1.f-std::abs(settings.cloudType)};
 const float moonDisc[4]={f.moonDiscScreenX,f.moonDiscScreenY,f.moonDiscViewDepth,0.f};
 const float moonAngle=5.f*0.01745329252f;
 const float moonControl[4]={.5f*std::tan(moonAngle)*f.projUnpack[3],
                              std::max(.75f,f.moonDiscViewDepth*.04f),
                              f.moonDiscValid?1.f:0.f,
                              f.projUnpack[3]/std::max(f.projUnpack[2],1e-4f)};
 // Where the sprite's own plane sits in the depth buffer, and how much a
 // camera-facing billboard may vary across its face. The shaders additionally
 // require the centre and the rim to agree, so a floor or wall at the
 // billboard's distance is not mistaken for the moon (CelestialDiscGate.h).
 // Raw viewport depth, deliberately un-normalised: the shaders read the depth
 // buffer's own value at the centre and the rim, exactly like IsFlatSprite.
 const float moonSprite[4]={renderer::discgate::RawDepth(f.projUnpack[0],f.projUnpack[1],f.moonDiscViewDepth),
                            renderer::discgate::RawTolerance(f.projUnpack[0],f.projUnpack[1],
                                f.moonDiscViewDepth,moonControl[1]),
                            moonControl[0]*.72f,
                            (f.moonDiscValid&&f.moonDiscViewDepth>.05f&&!f.cameraPoseStale)?1.f:0.f};
 const float c14[4]={kBaseExtinction*settings.density*settings.opacity,settings.lightAbsorption,
                     settings.sunIntensity*directIntensity,
                     settings.ambientIntensity*ambientOcclusion};
 const float sunViewDepth=f.sunDiscValid?f.sunDiscViewDepth:1.f;
 const float sunDepthScale=std::max(f.depthMaxZ-f.viewport.MinZ,0.f);
 const float expectedSunDepth=f.viewport.MinZ+
     renderer::discgate::RawDepth(f.projUnpack[0],f.projUnpack[1],sunViewDepth)*sunDepthScale;
 const float sunDepthTolerance=renderer::discgate::RawTolerance(f.projUnpack[0],f.projUnpack[1],
     sunViewDepth,std::max(.75f,sunViewDepth*.04f))*sunDepthScale;
 const float sunDisc[4]={f.sunDiscScreenX,f.sunDiscScreenY,f.sunDiscValid?.000484f:-1.f,0.f};
 const float sunDepth[4]={f.projUnpack[3]/std::max(f.projUnpack[2],1e-4f),
                          expectedSunDepth-sunDepthTolerance,0.f,0.f};

 ScopedPassState state(d);
 DepthCapture::Instance().RawSetDepth(d,nullptr);
 for(auto s:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,
             D3DRS_SCISSORTESTENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_SRGBWRITEENABLE,
             D3DRS_ALPHABLENDENABLE})d->SetRenderState(s,FALSE);
 d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
 d->SetRenderState(D3DRS_COLORWRITEENABLE,0xF);
 d->SetVertexShader(nullptr);

 D3DVIEWPORT9 low{0,0,m_lowWidth,m_lowHeight,0,1},full{0,0,m_fullWidth,m_fullHeight,0,1};

 if(shadowsOn)
 {
  ScopedGpuTimer timer(d,GpuPerfStage::VolumetricClouds);
  D3DVIEWPORT9 shadowPort{0,0,m_shadowSize,m_shadowSize,0,1};
  d->SetViewport(&shadowPort);
  d->SetRenderTarget(0,m_shadowSurface.Get());
  d->SetPixelShader(m_shadowShader.Get());
  d->SetTexture(1,m_shapeTexture.Get());SetSampler(d,1,D3DTEXF_LINEAR,D3DTADDRESS_WRAP);
  d->SetTexture(3,m_weatherTexture.Get());SetSampler(d,3,D3DTEXF_LINEAR,D3DTADDRESS_WRAP);
  // A fixed reference plane: lifting/rotating the camera cannot re-light
  // the whole world. The composite projects each receiver onto this plane.
  const float c21[4]={m_shadowOrigin[0],m_shadowOrigin[1],shadowExtent,0.f};
  d->SetPixelShaderConstantF(1,c1,1);d->SetPixelShaderConstantF(7,c7,1);
  d->SetPixelShaderConstantF(8,c8,1);d->SetPixelShaderConstantF(9,c9,1);
  d->SetPixelShaderConstantF(10,c10,1);d->SetPixelShaderConstantF(11,c11,1);
  d->SetPixelShaderConstantF(12,c12,1);d->SetPixelShaderConstantF(23,c23,1);
  // Deliberately no c14 (extinction / AbsorptionPercent) here: the shadow
  // map carries the unitless cloud amount, so DensityPercent stays an
  // optical-thickness knob for the sky and no longer decides how dark the
  // ground shadow is. GroundShadowPercent is the only strength control.
  d->SetPixelShaderConstantF(21,c21,1);
  DrawScreenQuad(d,m_shadowSize,m_shadowSize);
  m_shadowValid=true;
  d->SetTexture(1,nullptr);d->SetTexture(3,nullptr);
 }

 {
  ScopedGpuTimer timer(d,GpuPerfStage::VolumetricClouds);
  d->SetViewport(&low);
  d->SetRenderTarget(0,m_cloudSurface.Get());
  d->SetPixelShader(m_marchShader.Get());
  d->SetTexture(0,f.depthTexture);SetSampler(d,0,D3DTEXF_POINT,D3DTADDRESS_CLAMP);
  d->SetTexture(1,m_shapeTexture.Get());SetSampler(d,1,D3DTEXF_LINEAR,D3DTADDRESS_WRAP);
  d->SetTexture(2,m_detailTexture.Get());SetSampler(d,2,D3DTEXF_LINEAR,D3DTADDRESS_WRAP);
  d->SetTexture(3,m_weatherTexture.Get());SetSampler(d,3,D3DTEXF_LINEAR,D3DTADDRESS_WRAP);

  const float c0[4]={f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,0.f};
  const float inv[3][4]={{f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},
                         {f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},
                         {f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0}};
  const float c5[4]={f.projUnpack[0],f.projUnpack[1],f.projUnpack[2],f.projUnpack[3]};
  const float c6[4]={f.viewport.MinZ,f.depthMaxZ-f.viewport.MinZ,f.depthMaxZ,
                     float(std::clamp(settings.sampleCount,8u,160u))};
  const float c13[4]={std::max(settings.baseStep,1.f),settings.stepGrowth,settings.coarseScale,settings.jitter};
  const float c15[4]={sunR*cloudColourMultiplier[0],sunG*cloudColourMultiplier[1],
                      sunB*cloudColourMultiplier[2],settings.powder};
  // Day/night scaling and the low-ambient floor are already applied once
  // in cloudSky/cloudGround. c14.w is a gain only; putting ambientLevel in
  // both places squared the night dimmer and made clouds nearly black.
  const float c16[4]={cloudSkyR*cloudColourMultiplier[0],
                      cloudSkyG*cloudColourMultiplier[1],
                      cloudSkyB*cloudColourMultiplier[2],settings.silverLining};
  // Ground bounce keeps the zone palette, with its own lower night fill.
  const float c17[4]={cloudGroundR*cloudColourMultiplier[0],
                      cloudGroundG*cloudColourMultiplier[1],
                      cloudGroundB*cloudColourMultiplier[2],settings.horizonFade};
  const float c18[4]={settings.forwardScatter,settings.backwardScatter,settings.multiScatterEccentricity,
                      float(uint32_t(settings.debugMode))};
  const float a=settings.multiScatterAttenuation,bContribution=settings.multiScatterContribution;
  const float c19[4]={a,a*a,bContribution,bContribution*bContribution};
  const float c20[4]={float(motion.detail[0]),float(motion.detail[1]),
                      std::max(thickness*.18f,1.f),lightSteps+1.f};
  d->SetPixelShaderConstantF(0,c0,1);d->SetPixelShaderConstantF(1,c1,1);
  d->SetPixelShaderConstantF(2,inv[0],3);
  d->SetPixelShaderConstantF(5,c5,1);d->SetPixelShaderConstantF(6,c6,1);
  // Absolute world heights: never recenter the visible layer on the viewer.
  const float layer[4]={settings.baseAltitude,layerTop,1.f/thickness,settings.maxDistance};
  d->SetPixelShaderConstantF(7,layer,1);d->SetPixelShaderConstantF(8,c8,1);
  d->SetPixelShaderConstantF(9,c9,1);d->SetPixelShaderConstantF(10,c10,1);
  d->SetPixelShaderConstantF(11,c11,1);d->SetPixelShaderConstantF(12,c12,1);d->SetPixelShaderConstantF(23,c23,1);
  d->SetPixelShaderConstantF(24,moonDisc,1);d->SetPixelShaderConstantF(25,moonControl,1);
  d->SetPixelShaderConstantF(26,moonSprite,1);
  d->SetPixelShaderConstantF(27,sunDisc,1);d->SetPixelShaderConstantF(28,sunDepth,1);
  d->SetPixelShaderConstantF(13,c13,1);d->SetPixelShaderConstantF(14,c14,1);
  d->SetPixelShaderConstantF(15,c15,1);d->SetPixelShaderConstantF(16,c16,1);
  d->SetPixelShaderConstantF(17,c17,1);d->SetPixelShaderConstantF(18,c18,1);
  // Aerial perspective colour: the zone's own sky, lifted toward white,
  // so the haze belongs to the palette the atmosphere pass is already
  // using instead of being a second opinion about what distance looks like.
  const float c22[4]={(skyR+(1.f-skyR)*.28f)*ambientLevel*cloudColourMultiplier[0],
                      (skyG+(1.f-skyG)*.28f)*ambientLevel*cloudColourMultiplier[1],
                      (skyB+(1.f-skyB)*.28f)*ambientLevel*cloudColourMultiplier[2],
                      1.f/std::max(settings.maxDistance*.5f,1.f)};
  d->SetPixelShaderConstantF(19,c19,1);d->SetPixelShaderConstantF(20,c20,1);
  d->SetPixelShaderConstantF(22,c22,1);
  DrawScreenQuad(d,m_lowWidth,m_lowHeight);
  for(DWORD s=0;s<4;++s)d->SetTexture(s,nullptr);
 }

 {
  ScopedGpuTimer timer(d,GpuPerfStage::VolumetricClouds);
  d->SetViewport(&full);
  d->SetRenderTarget(0,target);
  d->SetPixelShader(m_compositeShader.Get());
  d->SetTexture(0,f.sceneColor);SetSampler(d,0,D3DTEXF_POINT,D3DTADDRESS_CLAMP);
  d->SetTexture(1,m_cloudTexture.Get());SetSampler(d,1,D3DTEXF_POINT,D3DTADDRESS_CLAMP);
  d->SetTexture(2,f.depthTexture);SetSampler(d,2,D3DTEXF_POINT,D3DTADDRESS_CLAMP);
  d->SetTexture(3,shadowsOn?m_shadowTexture.Get():nullptr);SetSampler(d,3,D3DTEXF_LINEAR,D3DTADDRESS_CLAMP);
  // No mask this frame samples as zero, which means no water anywhere and therefore no
  // exclusion - the identity this pass has always had.
  d->SetTexture(4,f.waterMaskTexture);SetSampler(d,4,D3DTEXF_POINT,D3DTADDRESS_CLAMP);
  const float c0[4]={1.f/float(m_fullWidth),1.f/float(m_fullHeight),1.f/float(m_lowWidth),1.f/float(m_lowHeight)};
  const float c1c[4]={f.viewport.MinZ,f.depthMaxZ-f.viewport.MinZ,f.depthMaxZ,float(uint32_t(settings.debugMode))};
  const float c2[4]={f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,0};
  const float inv[3][4]={{f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},
                         {f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},
                         {f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0}};
  const float c6c[4]={f.projUnpack[0],f.projUnpack[1],f.projUnpack[2],f.projUnpack[3]};
  const float c7c[4]={m_shadowOrigin[0],m_shadowOrigin[1],1.f/shadowExtent,
                      shadowsOn&&settings.groundShadows?shadowStrength:0.f};
  d->SetPixelShaderConstantF(0,c0,1);d->SetPixelShaderConstantF(1,c1c,1);
  d->SetPixelShaderConstantF(2,c2,1);d->SetPixelShaderConstantF(3,inv[0],3);
  d->SetPixelShaderConstantF(6,c6c,1);d->SetPixelShaderConstantF(7,c7c,1);
  const float c8c[4]={slopeX,slopeY,settings.baseAltitude,thickness};
  d->SetPixelShaderConstantF(8,c8c,1);
  d->SetPixelShaderConstantF(9,moonDisc,1);d->SetPixelShaderConstantF(10,moonControl,1);
  d->SetPixelShaderConstantF(12,moonSprite,1);
  const float c11[4]={std::clamp(f.underwaterRamp,0.f,1.f),0,0,0};
  d->SetPixelShaderConstantF(11,c11,1);
  d->SetPixelShaderConstantF(13,sunDisc,1);d->SetPixelShaderConstantF(14,sunDepth,1);
  DrawScreenQuad(d,m_fullWidth,m_fullHeight);
  for(DWORD s=0;s<5;++s)d->SetTexture(s,nullptr);
 }
 if(m_lastDiagnosticState.rfind("skip=",0)==0)logState("render=active");
 else m_lastDiagnosticState="render=active";
 return true;
}
}
