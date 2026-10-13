#include "../Environment/LocationTuning.h"
#include "VolumetricClouds.h"
#include "CloudNoise.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "../D3D9/CelestialStability.h"
#include "../D3D9/DepthCapture.h"
#include "../D3D9/ScopedRenderState.h"
#include "../Diagnostics/PerformanceProfiler.h"
#include "EnvironmentFogCapture.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
namespace
{
// Volumetric cloud deck.
//
// The deck is a spherical shell (planet radius `shell`, centre directly
// under the camera) rather than a flat slab, so it meets the horizon with a
// curve, keeps a sane optical path at grazing angles, and can be flown into
// and above. Density comes from three offline-generated, tiling fields that
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
// Only sky pixels are integrated: everything the client already drew keeps a
// transmittance of 1, so the composite below cannot touch solid geometry
// even where the quarter-resolution buffer bleeds across a silhouette.
const char* kCloudMarchSource = R"HLSL(
sampler2D depthMap:register(s0); sampler3D shapeMap:register(s1);
sampler3D detailMap:register(s2); sampler2D weatherMap:register(s3);
float4 cameraPos:register(c0);      // xyz=camera world position, w=elapsed seconds
float4 celestial:register(c1);      // xyz=world direction TOWARDS the tracked disc, w=intensity
float4 invView0:register(c2); float4 invView1:register(c3); float4 invView2:register(c4);
float4 projection:register(c5);     // x=A, y=B (raw->view z), z=P00, w=P11
float4 depthRange:register(c6);     // x=viewport MinZ, y=viewport depth range, z=sky threshold, w=march steps
float4 shell:register(c7);          // x=inner radius, y=outer radius, z=1/thickness, w=max march distance
float4 centre:register(c8);         // xyz=planet centre, w=camera region (0 inside, 1 below, 2 above)
float4 window:register(c9);         // x=coverage low edge, y=1/(high-low), z=cloud type bias, w=aerial opacity
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

// Analytic vertical profile per cloud type: this is what separates a flat
// overcast lid from towering cumulus with flat bottoms and round tops.
float Gradient(float h,float type) {
 float stratus=saturate(h*14.2857)*saturate((.32-h)*8.3333);
 float cumulus=saturate((h-.08)*4.1667)*saturate((.96-h)*2.6316);
 float tower=saturate((h-.04)*7.1429)*saturate((1-h)*5.5556);
 return stratus*saturate(1-type*2)+cumulus*saturate(1-abs(type-.5)*2)+tower*saturate(type*2-1);
}

float HeightFraction(float3 p) { return saturate((length(p-centre.xyz)-shell.x)*shell.z); }

// `erosion` is 0 for the light march and the coarse probe, which lets the
// compiler drop the detail fetch from those two inlined copies entirely.
float Density(float3 p,float h,float erosion) {
 float3 w=tex2Dlod(weatherMap,float4(p.xy*motion.z+motion2.xy,0,0)).rgb;
 // The slider slides a window along the weather map's (histogram-equalised)
 // coverage channel. Both ends matter: the low edge decides how much sky
 // gets any cloud at all, the high edge how solid a mass becomes.
 float cov=saturate((w.r-window.x)*window.y);
 float g=Gradient(h,saturate(w.g+window.z));
 if(cov*g<=0) return 0;
 // Shear the tops downwind with height - the cheap stand-in for a curl
 // distortion that ps_3_0 cannot afford - then sample the base shape.
 float4 s=tex3Dlod(shapeMap,float4((p.xy+motion2.zw*h)*scales.x+motion.xy,p.z*scales.y,0));
 float fbm=s.g*.625+s.b*.25+s.a*.125;
 // Perlin-Worley base eroded by its own Worley octaves, then the coverage
 // remap: raising coverage pulls the whole field up through the threshold
 // instead of simply scaling what is already there.
 float d=saturate((s.r-fbm+1)/max(2-fbm,1e-4))*g;
 d=saturate((d-1+cov)/max(cov,1e-4))*(.45+.55*cov);
 if(d<=0) return 0;
 if(erosion>0) {
   float3 n=tex3Dlod(detailMap,float4((p.xy+motion2.zw*h)*scales.z+detailMotion.xy,p.z*scales.z,0)).rgb;
   // Wispy shreds at the base, cauliflower billows at the top.
   float dfbm=n.r*.625+n.g*.25+n.b*.125;
   // Erosion that fades out inside the core. Eroding uniformly either
   // leaves the silhouette smooth or hollows the cloud out; real cumulus
   // shred at the edges and stay solid in the middle.
   float strength=erosion*(1-d*.7);
   d=saturate((d-lerp(1-dfbm,dfbm,saturate(h*4))*strength)/max(1-erosion,1e-4));
 }
 // More water higher up, modulated by the weather map's variation channel.
 return d*lerp(.45,1.25,saturate(h*1.4))*lerp(.75,1.2,w.b)*lighting.x;
}

// Deliberately NOT divided by 4*pi: an isotropic lobe returns 1, so the
// direct term is "sunlight times how much of it reaches here" and the gain
// slider means what it says.
float HG(float c,float g) { return (1-g*g)*pow(max(1+g*g-2*g*c,1e-4),-1.5); }

float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
 float rawDepth=tex2D(depthMap,uv).r;
 // INTZ stores viewport depth; normalise it exactly like the atmosphere's
 // boundary pass so one convention covers every injected pass.
 float raw=rawDepth>=.99999?1:saturate((rawDepth-depthRange.x)/max(depthRange.y,.001));

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
 float sceneLimit=raw<depthRange.z?(projection.y/min(rawDepth-projection.x,-1e-6))*viewLength:shell.w;

 // Shell intersection. The planet centre sits directly under the camera, so
 // the quadratic collapses to a couple of multiplies.
 float camR=cameraPos.z-centre.z;
 float b=camR*ray.z, cc=camR*camR;
 float outerDisc=b*b-cc+shell.y*shell.y;
 if(outerDisc<0) return float4(0,0,0,1);
 float outerSq=sqrt(outerDisc);
 float innerDisc=b*b-cc+shell.x*shell.x;
 float innerSq=innerDisc>0?sqrt(innerDisc):0;
 float entry,exitDistance;
 if(centre.w>1.5) {        // above the deck: enter at the top, leave at the bottom
   entry=-b-outerSq;
   if(entry<0) return float4(0,0,0,1);
   exitDistance=innerDisc>0?(-b-innerSq):(-b+outerSq);
 } else if(centre.w>.5) {  // below the deck: skip the ground half of the sky
   if(ray.z<=0) return float4(0,0,0,1);
   entry=-b+innerSq;
   exitDistance=-b+outerSq;
 } else {                  // inside the deck
   entry=0;
   exitDistance=(innerDisc>0&&-b-innerSq>0)?(-b-innerSq):(-b+outerSq);
 }
 exitDistance=min(exitDistance,min(shell.w,sceneLimit));
 if(exitDistance<=entry) return float4(0,0,0,1);

 // The horizon fade stops a grazing ray from UNDER the deck stretching the
 // shell to infinity and showing a hard band; from inside or above the
 // layer, downward rays are the whole point, so it is gated on the region.
 float fade=(centre.w>.5&&centre.w<1.5)?smoothstep(0,max(groundColor.w,.001),ray.z):1;
 // The march stops at its own limit long before the shell does: fade there
 // too or the cut shows up as a straight edge across the sky.
 fade*=1-smoothstep(shell.w*.55,shell.w,entry);
 if(fade<=.002) return float4(0,0,0,1);

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
 // count: the shell is 1400 units thick but 25000 long near the horizon,
 // and dividing the span is what turns detail into putty. It grows with
 // distance because far clouds cover fewer pixels.
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
     // Big strides until something is hit, then step back and integrate
     // properly. Doubles the usable range for free.
     if(density>0) { mode=0; t-=stepLength*stepping.z; }
     else t+=stepLength*stepping.z;
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

// Top-down transmittance of the deck toward the light, rendered into a
// small map centred on the camera so the world below gets the moving cloud
// shadows that sell the whole effect, and supplies cloud visibility to
// atmospheric rays. Low-frequency planar approximation of the march field:
// same weather/shape/profile/wind, without detail erosion or viewer curvature.
const char* kCloudShadowSource = R"HLSL(
sampler3D shapeMap:register(s1); sampler2D weatherMap:register(s3);
float4 celestial:register(c1);
float4 shell:register(c7);
float4 centre:register(c8);
float4 window:register(c9);
float4 scales:register(c10);
float4 motion:register(c11);
float4 motion2:register(c12);
float4 lighting:register(c14);
float4 area:register(c21);          // xy=world origin, z=world extent, w=receiver height

float Gradient(float h,float type) {
 float stratus=saturate(h*14.2857)*saturate((.32-h)*8.3333);
 float cumulus=saturate((h-.08)*4.1667)*saturate((.96-h)*2.6316);
 float tower=saturate((h-.04)*7.1429)*saturate((1-h)*5.5556);
 return stratus*saturate(1-type*2)+cumulus*saturate(1-abs(type-.5)*2)+tower*saturate(type*2-1);
}

float Density(float3 p) {
 // A local planar approximation for the low-frequency shadow field.
 // Neither camera XY nor camera height changes the optical path.
 float h=saturate((p.z-(shell.x+centre.z))*shell.z);
 float3 w=tex2Dlod(weatherMap,float4(p.xy*motion.z+motion2.xy,0,0)).rgb;
 float cov=saturate((w.r-window.x)*window.y);
 float g=Gradient(h,saturate(w.g+window.z));
 if(cov*g<=0) return 0;
 float4 s=tex3Dlod(shapeMap,float4((p.xy+motion2.zw*h)*scales.x+motion.xy,p.z*scales.y,0));
 float fbm=s.g*.625+s.b*.25+s.a*.125;
 float d=saturate((s.r-fbm+1)/max(2-fbm,1e-4))*g;
 return saturate((d-1+cov)/max(cov,1e-4))*(.45+.55*cov)
       *lerp(.45,1.25,saturate(h*1.4))*lerp(.75,1.2,w.b)*lighting.x;
}

float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float3 light=normalize(celestial.xyz);
 // A disc on the horizon casts shadows a hundred kilometres long; there is
 // nothing useful left to project, so hand back "lit".
 if(light.z<=.03) return float4(1,1,1,1);
 float3 p=float3(area.xy+uv*area.z,area.w);
 float base=shell.x+centre.z;
 float entry=max((base-p.z)/light.z,0);
 float exitDistance=(base+shell.y-shell.x-p.z)/light.z;
 // More steps than the old 8: at grazing sun angles the slant path through
 // the deck is long, and 8 widely-spaced samples band the transmittance
 // into concentric stripes - the "lines on the shadow near the light
 // source" players report. 16 keeps the banding below the deck's own
 // feature scale even when the sun is low.
 float stepLength=max(0,exitDistance-entry)/16.f;
 if(stepLength<=0) return float4(1,1,1,1);
 float optical=0;
 [loop] for(int i=0;i<16;i++) optical+=Density(p+light*(entry+stepLength*(i+.5)));
 return exp(-optical*stepLength).xxxx;
})HLSL";

// Full-resolution composite. The low-resolution deck is a smooth signal
// with no depth edges of its own, so a plain bilinear fetch is enough -
// geometry pixels carry transmittance 1 and therefore fade the clouds out
// across a silhouette instead of haloing it. The same pass drops the cloud
// shadow onto everything the client drew.
const char* kCloudCompositeSource = R"HLSL(
sampler2D sceneMap:register(s0); sampler2D cloudMap:register(s1);
sampler2D depthMap:register(s2); sampler2D shadowMap:register(s3);
float4 texels:register(c0);      // x=1/full width, y=1/full height, z=1/low width, w=1/low height
float4 depthRange:register(c1);  // x=viewport MinZ, y=viewport depth range, z=sky threshold, w=debug
float4 cameraPos:register(c2);
float4 invView0:register(c3); float4 invView1:register(c4); float4 invView2:register(c5);
float4 projection:register(c6);  // x=A, y=B, z=P00, w=P11
float4 area:register(c7);        // xy=shadow world origin, z=1/extent, w=shadow strength
float4 lightProjection:register(c8); // xy=light XY/Z, z=cloud base, w=thickness
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float4 scene=tex2D(sceneMap,uv);
 if(depthRange.w>3.5) return tex2D(shadowMap,uv).rrra;
 float linearRaw=tex2D(depthMap,uv).r;

 // Depth-aware upsample of the quarter-resolution march. The deck is no
 // longer sky-only - it is clipped by the scene, so a cloud in front of a
 // mountain is a real pixel - and a plain bilinear fetch would drag the
 // far cloud across the near silhouette as a halo. The four taps are
 // low-resolution texel centres, so point-sampling the depth map at the
 // same places returns exactly the depth each of those marches used.
 float2 lowTexel=texels.zw;
 float2 grid=uv/lowTexel-.5;
 float2 base=(floor(grid)+.5)*lowTexel, fr=frac(grid);
 float4 taps=float4(tex2D(depthMap,base).r,tex2D(depthMap,base+float2(lowTexel.x,0)).r,
                    tex2D(depthMap,base+float2(0,lowTexel.y)).r,tex2D(depthMap,base+lowTexel).r);
 // Compare in linear view depth: raw depth-buffer values crowd into the
 // last fraction of their range at distance, where most of the sky is.
 // Both reciprocals are clamped away from the projection's pole, which a
 // far plane at infinity sits exactly on: two infinities one subtraction
 // apart are a NaN, and a NaN weight poisons the whole pixel.
 float centreZ=projection.y/min(linearRaw-projection.x,-1e-6);
 float4 weight=float4((1-fr.x)*(1-fr.y),fr.x*(1-fr.y),(1-fr.x)*fr.y,fr.x*fr.y)
              /(.002+abs(projection.y/min(taps-projection.x,-1e-6)-centreZ)/max(centreZ,1));
 float4 cloud=(tex2D(cloudMap,base)*weight.x+tex2D(cloudMap,base+float2(lowTexel.x,0))*weight.y
              +tex2D(cloudMap,base+float2(0,lowTexel.y))*weight.z+tex2D(cloudMap,base+lowTexel)*weight.w)
              /max(dot(weight,1),1e-5);

 if(depthRange.w>.5&&depthRange.w<1.5) return float4(cloud.rgb,1);
 float raw=linearRaw>=.99999?1:saturate((linearRaw-depthRange.x)/max(depthRange.y,.001));
 float sky=smoothstep(depthRange.z,1,raw);
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
 return float4(lit*saturate(cloud.a)+max(cloud.rgb,0),scene.a);
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
 std::ofstream out(std::filesystem::path(path),std::ios::binary|std::ios::trunc);
 if(!out)return;
 const CacheHeader header{kCacheMagic,kCacheVersion,kShapeSize,kDetailSize,kWeatherSize};
 out.write(reinterpret_cast<const char*>(&header),sizeof(header));
 out.write(reinterpret_cast<const char*>(shape.rgba.data()),std::streamsize(shape.rgba.size()));
 out.write(reinterpret_cast<const char*>(detail.rgba.data()),std::streamsize(detail.rgba.size()));
 out.write(reinterpret_cast<const char*>(weather.rgba.data()),std::streamsize(weather.rgba.size()));
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
 // Steps, light steps, base step and resolution move together: a longer
 // base step with more samples is wasted, and a short one without the
 // samples to back it just stops short of the far clouds.
 static constexpr uint32_t kSteps[]={48,64,96,128};
 static constexpr uint32_t kLightSteps[]={4,5,6,6};
 static constexpr float kBaseSteps[]={130.f,100.f,70.f,55.f};
 static constexpr float kScales[]={.25f,.25f,.25f,.5f};
 const uint32_t level=std::min(quality,3u);
 m_settings.quality=level;
 m_settings.sampleCount=kSteps[level];
 m_settings.lightSampleCount=kLightSteps[level];
 m_settings.baseStep=kBaseSteps[level];
 m_settings.resolutionScale=kScales[level];
}

void VolumetricClouds::Configure(const std::wstring& basePath)
{
 const std::wstring ini=basePath+L"GraphicsEffects.ini";
 m_logPath=basePath+L"Clouds.log";
 m_cachePath=basePath+L"CloudFields.cache";
 auto read=[&](const wchar_t* key,int fallback)->int{return static_cast<int>(renderer::locationtuning::ReadInt(L"VolumetricClouds",key,fallback,ini.c_str()));};
 m_settings.enabled=read(L"Enabled",1)!=0;
 ApplyQualityPreset(static_cast<uint32_t>(std::clamp(read(L"Quality",2),0,3)));
 // Every preset value stays overridable: the presets only pick the defaults.
 m_settings.sampleCount=static_cast<uint32_t>(std::clamp(read(L"SampleCount",int(m_settings.sampleCount)),8,160));
 m_settings.lightSampleCount=static_cast<uint32_t>(std::clamp(read(L"LightSampleCount",int(m_settings.lightSampleCount)),1,6));
 m_settings.baseStep=float(std::clamp(read(L"BaseStep",int(m_settings.baseStep)),8,600));
 m_settings.resolutionScale=std::clamp(read(L"ResolutionPercent",int(m_settings.resolutionScale*100.f+.5f)),20,100)*.01f;

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
 m_settings.maxDistance=float(std::clamp(read(L"MaxDistance",26000),2000,200000));
 m_settings.aerialPerspective=std::clamp(read(L"AerialPerspectivePercent",60),0,100)*.01f;
 m_settings.groundShadows=read(L"GroundShadows",1)!=0;
 m_settings.groundShadowStrength=std::clamp(read(L"GroundShadowPercent",55),0,100)*.01f;
 m_settings.groundShadowExtent=float(std::clamp(read(L"GroundShadowExtent",24000),2000,80000));
 m_settings.sunRays=read(L"SunRaysEnabled",1)!=0;
 m_settings.sunRayStrength=std::clamp(read(L"SunRayPercent",80),0,200)*.01f;
 m_settings.debugMode=static_cast<CloudDebugMode>(std::clamp(read(L"DebugMode",0),0,4));
 m_settings.weatherCoverageBoost=0.f;
}

void VolumetricClouds::Reset(IDirect3DDevice9* device)
{
 if(m_owner&&m_owner!=device)return;
 m_cloudSurface.Reset();m_cloudTexture.Reset();
 m_shadowSurface.Reset();m_shadowTexture.Reset();m_shadowValid=false;
 m_marchShader.Reset();m_compositeShader.Reset();m_shadowShader.Reset();
 // A new device (or a resize, which is rare) is worth one more attempt.
 m_shadersFailed=false;
 m_owner=nullptr;m_fullWidth=m_fullHeight=m_lowWidth=m_lowHeight=0;m_shadowSize=0;
 // The noise fields live in D3DPOOL_MANAGED and survive a device reset, so
 // an alt-tab does not pay for regenerating them. Only a different device
 // invalidates them.
 if(device==nullptr)
 {
  m_shapeTexture.Reset();m_detailTexture.Reset();m_weatherTexture.Reset();
  m_fieldsReady=false;m_fieldsFailed=false;
 }
}

bool VolumetricClouds::EnsureShaders(IDirect3DDevice9* d)
{
 if(m_marchShader&&m_compositeShader&&m_shadowShader)return true;
 // Compiling a 460-slot ps_3_0 shader costs tens of milliseconds. Without
 // remembering the failure, a driver that rejects the march would be asked
 // to reject it again on every single frame - turning "this machine has no
 // clouds" into "this machine has no frames". The fields cache their own
 // failure for the same reason.
 if(m_shadersFailed)return false;
 if(Compile(d,kCloudMarchSource,m_marchShader.GetAddressOf())&&
    Compile(d,kCloudCompositeSource,m_compositeShader.GetAddressOf())&&
    Compile(d,kCloudShadowSource,m_shadowShader.GetAddressOf()))return true;
 m_marchShader.Reset();m_compositeShader.Reset();m_shadowShader.Reset();
 m_shadersFailed=true;
 if(!m_logPath.empty())
 {
  std::ofstream out(std::filesystem::path(m_logPath),std::ios::app);
  out<<"clouds: pixel shader compilation failed on this device; the deck is off\n";
 }
 return false;
}

bool VolumetricClouds::EnsureFields(IDirect3DDevice9* d)
{
 if(m_fieldsReady)return true;
 if(m_fieldsFailed)return false;
 D3DCAPS9 caps{};
 if(FAILED(d->GetDeviceCaps(&caps))||!(caps.TextureCaps&D3DPTEXTURECAPS_VOLUMEMAP)||
    caps.MaxVolumeExtent<kShapeSize)
 {
  m_fieldsFailed=true;
  return false;
 }
 cloudnoise::Volume3D shape,detail;
 cloudnoise::Image2D weather;
 // Roughly a second of work on a cold start, so it is written next to the
 // DLL and read back on every later launch. Generation happens on the
 // first frame that would draw clouds, which in practice is a loading
 // screen rather than live gameplay.
 if(!LoadFields(m_cachePath,shape,detail,weather))
 {
  shape=cloudnoise::GenerateShape(int(kShapeSize));
  detail=cloudnoise::GenerateDetail(int(kDetailSize));
  weather=cloudnoise::GenerateWeather(int(kWeatherSize));
  SaveFields(m_cachePath,shape,detail,weather);
 }
 if(!UploadVolume(d,shape,m_shapeTexture.ReleaseAndGetAddressOf())||
    !UploadVolume(d,detail,m_detailTexture.ReleaseAndGetAddressOf())||
    !UploadImage(d,weather,m_weatherTexture.ReleaseAndGetAddressOf()))
 {
  m_shapeTexture.Reset();m_detailTexture.Reset();m_weatherTexture.Reset();
  m_fieldsFailed=true;
  return false;
 }
 m_fieldsReady=true;
 return true;
}

bool VolumetricClouds::EnsureResources(IDirect3DDevice9* d,uint32_t w,uint32_t h)
{
 const uint32_t lw=std::max(1u,uint32_t(float(w)*m_settings.resolutionScale));
 const uint32_t lh=std::max(1u,uint32_t(float(h)*m_settings.resolutionScale));
 if(m_owner==d&&m_fullWidth==w&&m_fullHeight==h&&m_lowWidth==lw&&m_lowHeight==lh&&m_cloudTexture)return true;
 Reset(d);m_owner=d;m_fullWidth=w;m_fullHeight=h;m_lowWidth=lw;m_lowHeight=lh;
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

bool VolumetricClouds::Render(IDirect3DDevice9* d,const FrameContext& f,IDirect3DSurface9* target)
{
 m_shadowValid=false;
 VolumetricCloudSettings settings=m_settings;
 settings.coverage=std::clamp(settings.coverage+settings.weatherCoverageBoost,0.f,1.f);
 if(!settings.enabled||settings.coverage<=0.f||settings.density<=0.f)return false;
 if(!d||!target||!f.cameraValid||!f.depthAvailable||!f.depthTexture||!f.sceneColor)return false;

 D3DSURFACE_DESC desc{};
 if(FAILED(target->GetDesc(&desc))||!EnsureResources(d,desc.Width,desc.Height)||!EnsureShaders(d)||!EnsureFields(d))return false;

 const ULONGLONG tick=GetTickCount64();
 const float dt=m_lastTick?std::clamp(float(tick-m_lastTick)*.001f,.001f,.1f):1.f/60.f;
 m_lastTick=tick;m_elapsed+=dt;

 const float thickness=std::max(settings.thickness,20.f);
 const float featureSize=std::max(settings.featureSize,1.f);
 const float detailSize=std::max(settings.detailSize,1.f);
 const float weatherSize=std::max(settings.weatherSize,1.f);
 const float innerRadius=settings.planetRadius+settings.baseAltitude;
 const float outerRadius=innerRadius+thickness;
 // The planet centre tracks the camera horizontally: the deck is a dome
 // over the viewer, which is what gives the horizon its curve. The field
 // itself is sampled in absolute world coordinates, so the clouds stay put
 // while the curvature follows.
 const float centreZ=-settings.planetRadius;
 const float cameraRadius=f.cameraPosition.z-centreZ;
 const float region=cameraRadius<innerRadius?1.f:(cameraRadius>outerRadius?2.f:0.f);

 const float windRadians=settings.windAngle*.01745329252f;
 const float windX=std::cos(windRadians)*settings.windSpeed*m_elapsed;
 const float windY=std::sin(windRadians)*settings.windSpeed*m_elapsed;
 // Every field tiles over one uv unit, so wrapping the accumulated offset
 // is exact - and it keeps the constants small enough that texture
 // coordinate precision never drifts during a long session.
 auto wrap=[](float v){return v-std::floor(v);};

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
 const float daylight=std::clamp(f.daylightFactor,0.f,1.f);
 const float moonlight=std::clamp(f.moonlightFactor,0.f,1.f);
 const float ambientLevel=std::clamp(std::max(daylight,moonlight*.5f),.2f,1.f);
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

 if((++m_renderFrames%600)==0&&!m_logPath.empty()){
  std::ofstream out(std::filesystem::path(m_logPath),std::ios::app);
  out<<"clouds base="<<settings.baseAltitude<<" thickness="<<thickness<<" coverage="<<settings.coverage
     <<" density="<<settings.density<<" steps="<<settings.sampleCount<<" region="<<region
     <<" moon="<<(moon?1:0)<<" intensity="<<directIntensity<<" shadows="<<(shadowsOn?1:0)
     <<" camera=("<<f.cameraPosition.x<<','<<f.cameraPosition.y<<','<<f.cameraPosition.z<<")\n";
 }

 // Shared field constants: the march and the shadow pass read the same
 // registers so there is exactly one description of the cloud field.
 const float c1[4]={lightDir[0],lightDir[1],lightDir[2],directIntensity};
 const float c7[4]={innerRadius,outerRadius,1.f/thickness,settings.maxDistance};
 const float c8[4]={f.cameraPosition.x,f.cameraPosition.y,centreZ,region};
 const float c9[4]={windowLow,1.f/std::max(windowHigh-windowLow,1e-4f),settings.cloudType*.5f,
                    settings.aerialPerspective};
 const float c10[4]={1.f/featureSize,std::max(settings.shapeAspect,.05f)/featureSize,
                     1.f/detailSize,settings.detailStrength};
 // The light march spends its last iteration on a long reach sample; how
 // long is a constant, not something to recompute per sample.
 const float lightSteps=float(std::clamp(settings.lightSampleCount,1u,6u));
 const float c11[4]={wrap(windX/featureSize),wrap(windY/featureSize),1.f/weatherSize,lightSteps*3.f};
 const float c12[4]={wrap(windX*.35f/weatherSize),wrap(windY*.35f/weatherSize),
                     std::cos(windRadians)*settings.windShear*featureSize,
                     std::sin(windRadians)*settings.windShear*featureSize};
 // Less sky reaches the base of an overcast lid than the base of a lone
 // cumulus, so the ambient term closes down as the deck fills in. Without
 // it a full-coverage sky renders as a bright white ceiling.
 const float ambientOcclusion=1.f-coverage*.45f;
 const float c14[4]={kBaseExtinction*settings.density,settings.lightAbsorption,
                     settings.sunIntensity*directIntensity,
                     settings.ambientIntensity*ambientLevel*ambientOcclusion};

 ScopedRenderState state(d);
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
  d->SetPixelShaderConstantF(12,c12,1);d->SetPixelShaderConstantF(14,c14,1);
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

  const float c0[4]={f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,m_elapsed};
  const float inv[3][4]={{f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},
                         {f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},
                         {f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0}};
  const float c5[4]={f.projUnpack[0],f.projUnpack[1],f.projUnpack[2],f.projUnpack[3]};
  const float c6[4]={f.viewport.MinZ,f.depthMaxZ-f.viewport.MinZ,kSkyThreshold,
                     float(std::clamp(settings.sampleCount,8u,160u))};
  const float c13[4]={std::max(settings.baseStep,1.f),settings.stepGrowth,settings.coarseScale,settings.jitter};
  const float c15[4]={sunR,sunG,sunB,settings.powder};
  const float c16[4]={skyR*ambientLevel,skyG*ambientLevel,skyB*ambientLevel,settings.silverLining};
  // Ground bounce keeps the tuned relationship with whatever palette the
  // zone pushed: cooler, much darker, never a flat grey.
  const float c17[4]={skyR*ambientLevel*.62f,skyG*ambientLevel*.52f,skyB*ambientLevel*.45f,settings.horizonFade};
  const float c18[4]={settings.forwardScatter,settings.backwardScatter,settings.multiScatterEccentricity,
                      float(uint32_t(settings.debugMode))};
  const float a=settings.multiScatterAttenuation,bContribution=settings.multiScatterContribution;
  const float c19[4]={a,a*a,bContribution,bContribution*bContribution};
  const float c20[4]={wrap(windX*1.7f/detailSize),wrap(windY*1.7f/detailSize),
                      std::max(thickness*.18f,1.f),lightSteps+1.f};
  d->SetPixelShaderConstantF(0,c0,1);d->SetPixelShaderConstantF(1,c1,1);
  d->SetPixelShaderConstantF(2,inv[0],3);
  d->SetPixelShaderConstantF(5,c5,1);d->SetPixelShaderConstantF(6,c6,1);
  d->SetPixelShaderConstantF(7,c7,1);d->SetPixelShaderConstantF(8,c8,1);
  d->SetPixelShaderConstantF(9,c9,1);d->SetPixelShaderConstantF(10,c10,1);
  d->SetPixelShaderConstantF(11,c11,1);d->SetPixelShaderConstantF(12,c12,1);
  d->SetPixelShaderConstantF(13,c13,1);d->SetPixelShaderConstantF(14,c14,1);
  d->SetPixelShaderConstantF(15,c15,1);d->SetPixelShaderConstantF(16,c16,1);
  d->SetPixelShaderConstantF(17,c17,1);d->SetPixelShaderConstantF(18,c18,1);
  // Aerial perspective colour: the zone's own sky, lifted toward white,
  // so the haze belongs to the palette the atmosphere pass is already
  // using instead of being a second opinion about what distance looks like.
  const float c22[4]={(skyR+(1.f-skyR)*.28f)*ambientLevel,
                      (skyG+(1.f-skyG)*.28f)*ambientLevel,
                      (skyB+(1.f-skyB)*.28f)*ambientLevel,
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
  const float c0[4]={1.f/float(m_fullWidth),1.f/float(m_fullHeight),1.f/float(m_lowWidth),1.f/float(m_lowHeight)};
  const float c1c[4]={f.viewport.MinZ,f.depthMaxZ-f.viewport.MinZ,kSkyThreshold,float(uint32_t(settings.debugMode))};
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
  DrawScreenQuad(d,m_fullWidth,m_fullHeight);
  for(DWORD s=0;s<4;++s)d->SetTexture(s,nullptr);
 }
 return true;
}
}
