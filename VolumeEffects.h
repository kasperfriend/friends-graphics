#pragma once
// Shared shader for the isolated test and opt-in game integration.
// Height fog is integrated along the view ray. Shafts are SCREEN-SPACE visibility,
// not 3D shadow-map ray marching: off-screen occluders are not represented.
inline const char* volumePixelSource=R"HLSL(
sampler2D scene:register(s0);
sampler2D depthMap:register(s1);
sampler2D noiseMap:register(s2);
sampler2D cloudMap:register(s3);
float4 cloudMask:register(c14);
// x=projection A,y=projection B,z=projection scale X,w=scale Y
float4 projection:register(c0);
// x=base height,y=height falloff,z=density,w=max march distance
float4 medium:register(c1);
// xy=screen sun position,z=shaft strength,w=depth viewport maximum
float4 sun:register(c2);
float4 fogColor:register(c3);
float4 inverseView[4]:register(c4);
// x=0 linear depth debug, 1 height fog, 2 fog+screen-space shafts, 3 atmospheric composite
float4 mode:register(c8);
// x=time,y=world noise scale,z=variation,w=low-layer strength
float4 detail:register(c9);
// xyz=view-space direction toward the light
float4 lightDirection:register(c10);
float4 directColor:register(c11);
// xy=1/render size,z=softness in pixels,w=radial falloff
float4 rayTuning:register(c12);
// Second, independent height-fog layer (ground mist): x=height,y=falloff,z=density
float4 groundMist:register(c13);
// God-ray look (src/Effects/GodRayCore.h): x=sunward glow spread (per screen-height^2),
// y=glow floor in open sky, z=gain on the whole-frame mask, w=unused
float4 rayLook:register(c15);
// The depth-writing vanilla sun disc is itself the ray source, not an occluder.
// It is opened only at the tracked centre and at sky/far depth or its own depth.
float4 trackedSunDisc:register(c16); // xy=tracked centre, z=vertical UV radius, w=valid
float4 trackedSunDepth:register(c17); // x=horizontal scale, y=expected raw depth, z=tolerance
float TrackedSunDiscOpen(float2 uv,float raw) {
    float2 delta=float2((uv.x-trackedSunDisc.x)*trackedSunDepth.x,uv.y-trackedSunDisc.y);
    float disc=1-step(trackedSunDisc.z*trackedSunDisc.z,dot(delta,delta));
    float sky=step((sun.w+1)*.5,raw);
    float depthMatch=1-step(max(trackedSunDepth.z,1e-5),abs(raw-trackedSunDepth.y));
    return disc*trackedSunDisc.w*max(sky,depthMatch);
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float raw=tex2D(depthMap,uv).r;
    // INTZ contains viewport-mapped depth. Normalize for the projection
    // reconstruction, but retain the raw value for sky tests: clear depth
    // remains 1.0 while geometry is bounded by viewport MaxZ.
    float linearDepth=saturate(raw/max(sun.w,.001));
    float z=raw>=.9999 ? medium.w : projection.y/(linearDepth-projection.x);
    z=clamp(z,0,medium.w);
    if(mode.x<.5)return float4(z/medium.w,z/medium.w,z/medium.w,1);
    if(mode.x>5.5) {
        // Depth-based occlusion ONLY - is this pixel actually open sky.
        // Geometry/cloud can attenuate this, never emit: brightness never
        // enters the emission term below. Geometry maps to <=MaxZ while the
        // midpoint between those values rejects every occluder and keeps the
        // clear sky open, including when MaxZ itself is nearly one.
        float skyOpen=step((sun.w+1)*.5,raw);
        // The original sun billboard writes a far depth slightly nearer than
        // the clear value, so raw sky-only openness cut a dark hole exactly at
        // the source while the surrounding halo/rays were added. Admit only
        // its independently tracked circle at the sun's own depth; a nearer
        // roof, wall, or character still blocks the ray source.
        float openness=max(skyOpen,TrackedSunDiscOpen(uv,raw));
        // Aspect-ratio-correct distance to the real, CelestialTracker-
        // confirmed disc position (sun.xy). Raw UV-space distance is
        // squashed on non-square screens - a 0.1 horizontal delta covers
        // far more pixels than 0.1 vertical at 16:9 - which skewed the mask
        // into an ellipse. Express both axes in the same physical unit
        // (screen-height pixels) before measuring.
        float aspect=rayTuning.y>0.0 ? rayTuning.y/max(rayTuning.x,1e-6) : 1.0;
        float2 delta=float2((uv.x-sun.x)*aspect,uv.y-sun.y);
        float sunDist=length(delta);
        // Pure position mask, tight and hard-clipped around the real disc.
        // A bright cloud anywhere else on screen contributes nothing here -
        // it can only show up via `openness` failing to be 1 (occlusion),
        // never by adding to `sunMask`. This is the fix for "bright cloud
        // becomes a second sun".
        float discFalloff=(sun.x<-.5) ? 0.0 : exp(-sunDist*sunDist*260.0);
        // Wide, soft truncation of the Gaussian tail: the old 0.10..0.16
        // band ended the mask abruptly enough that the radial blur below
        // smeared the cut into a visible ring around the disc.
        float discCutoff=1.0-smoothstep(0.08,0.22,sunDist);
        // Whole-frame god-ray source (GodRayCore.h SourceWeight): a compact
        // sunward glow stays near the tracked disc, and the gather below carries
        // its visibility away from the source. Trees, walls, characters and
        // ground have openness 0 and cut those beams; cloud alpha thins them.
        // The disc core is kept inside the max, so the source itself stays brightest.
        float glow=rayLook.y+(1-rayLook.y)*exp(-sunDist*sunDist*rayLook.x);
        float sunMask=(sun.x<-.5) ? 0.0 : max(discFalloff*discCutoff,glow)*rayLook.z;
        // mode.y is now the SUN GLOW slider directly (0..3, 1.0 = the old
        // "100%" default) - it used to only nudge this by +-0.35 around a
        // fixed 0.62 floor, so dragging it to 0 barely dimmed the blob at
        // all. It now scales the near-disc glare/blob linearly, with a
        // small floor so it never hard-cuts to nothing.
        float transmission=lerp(1,saturate(tex2D(cloudMap,uv).a),cloudMask.x);
        float amount=openness*transmission*sunMask*sun.z*max(mode.y,.08);
        if(mode.x>6.5)return float4(amount,amount,amount,1);
        float3 color=lerp(fogColor.rgb,max(directColor.rgb,.1)*1.35,.75);
        return float4(color*amount,1);
    }
    float3 viewEnd=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*z;
    float3 origin=inverseView[3].xyz;
    float3 delta=viewEnd.x*inverseView[0].xyz+viewEnd.y*inverseView[1].xyz+viewEnd.z*inverseView[2].xyz;
    float rayLen=length(delta);

    // Analytic exponential height fog integral - continuous without horizon discontinuities
    float hCam=origin.z-medium.x;
    float dz=delta.z;
    float u=clamp(medium.y*dz,-12.0,12.0);
    float factor=(abs(u)<0.02) ? (1.0-0.5*u+(1.0/6.0)*u*u) : ((1.0-exp(-u))/u);
    // Clamp was +-8 (exp(8)~=2981x): a camera sitting even moderately below
    // BaseHeight - the normal case, since BaseHeight is an absolute world Z
    // and most zones don't put ground at Z=0 to match a default/tuned
    // value - blew this multiplier up catastrophically, whiting out the
    // whole scene instead of giving a graceful "thicker in the valley"
    // falloff. +-3 (exp(3)~=20x) still gives real height response without
    // the blowout.
    float eCam=exp(-clamp(hCam*medium.y,-3.0,3.0));
    float optDepth=medium.z*rayLen*eCam*factor;
    optDepth=max(optDepth,0.0);

    // Second layer: low, dense ground mist. Same analytic integral, stacked
    // on top by summing optical depth (the physically correct way to
    // combine two independent scattering media along one ray) rather than
    // replacing the broader haze above.
    float hCamMist=origin.z-groundMist.x;
    float uMist=clamp(groundMist.y*dz,-12.0,12.0);
    float factorMist=(abs(uMist)<0.02) ? (1.0-0.5*uMist+(1.0/6.0)*uMist*uMist) : ((1.0-exp(-uMist))/uMist);
    float eCamMist=exp(-clamp(hCamMist*groundMist.y,-3.0,3.0));
    optDepth+=max(groundMist.z*rayLen*eCamMist*factorMist,0.0);

    // Atmospheric rolling noise modulation
    float2 wind=float2(detail.x*.0021,-detail.x*.0013);
    float3 hitPos=origin+delta;
    float broad=tex2Dlod(noiseMap,float4(hitPos.xy*detail.y*.32+wind*.22,0,0)).r;
    float cloud=smoothstep(.25,.75,broad);
    optDepth*=lerp(.75,1.25,cloud*detail.z);

    float transmission=exp(-clamp(optDepth,0.0,12.0));
    // Classify clear sky from viewport-mapped depth before normalization.
    // Geometry is bounded by MaxZ; the midpoint leaves only the gap to the
    // D3D clear value (1.0) on the sky side, even when MaxZ is near one.
    float isSky=step((sun.w+1)*.5,raw);
    float distFog=1.0-exp(-min(z,medium.w)*medium.z*0.06);
    float fogAmount=max(1.0-transmission,distFog*0.35);
    // Smooth transition into sky distance fog rather than a hard cut
    fogAmount=lerp(fogAmount,distFog*0.15,isSky);

    float3 rayDir=normalize(viewEnd);
    float cosAngle=dot(rayDir,normalize(lightDirection.xyz));
    // Broad soft directional tint - whole-sky lean toward the light.
    float softLobe=pow(saturate(cosAngle*0.5+0.5),4.0);
    // Tight forward-scattering core (single-term Henyey-Greenstein,
    // g=0.82) on top - this is what makes looking toward the sun/moon
    // through haze actually glow, instead of just a flat sky-wide tint.
    // Cheap: one pow + one divide, evaluated per pixel, no extra passes.
    float g=0.82; float g2=g*g;
    float hg=(1.0-g2)/pow(max(1.0+g2-2.0*g*cosAngle,1e-4),1.5)*0.0795774715;
    float miePhase=saturate(softLobe*0.55+hg*2.2);
    float3 scattering=lerp(fogColor.rgb,max(directColor.rgb,.15)*1.6,miePhase);
    float shaftAmount=sun.z*(.35+1.0*fogAmount)*(.35+.85*miePhase);
    if(mode.x>4.5||mode.z>.5)return float4(fogAmount,fogAmount,fogAmount,1);
    if(mode.x>2.5)return float4(scattering,fogAmount*saturate(mode.y*1.5));
    float4 base=tex2D(scene,uv);
    float3 fogged=base.rgb*transmission+lerp(base.rgb,fogColor.rgb*.82,mode.y)*fogAmount;
    return float4(fogged+scattering*shaftAmount*.55,base.a);
}
)HLSL";

// The shafts are generated at half resolution and softened in two separable
// passes.  Linear sampling plus a nine-tap Gaussian removes the pixel-grid
// stair steps without blurring the underlying world or UI.
inline const char* volumeBlurPixelSource=R"HLSL(
sampler2D image:register(s0);
// xy = one blur step in UV space
float4 blurStep:register(c0);
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float4 c=tex2D(image,uv)*.227027;
    c+=(tex2D(image,uv+blurStep.xy*1.384615)+tex2D(image,uv-blurStep.xy*1.384615))*.158108;
    c+=(tex2D(image,uv+blurStep.xy*3.230769)+tex2D(image,uv-blurStep.xy*3.230769))*.113546;
    return c;
}
)HLSL";

inline const char* volumeCopyPixelSource=R"HLSL(
sampler2D image:register(s0);
float4 main(float2 uv:TEXCOORD0):COLOR0 { return tex2D(image,uv); }
)HLSL";

// Real motion-compensated reprojection: reconstructs this frame's world
// position from full-res depth, transforms it by LAST frame's view matrix
// (+ this frame's projection, assumed FOV-stable across one frame) to find
// where that same world point sat on screen last frame, and samples
// history there instead of at the naive same UV. Previously this shader
// just clamped history sampled at the current UV to the current frame's
// local neighborhood - correct for a static camera, but under camera
// motion that UV corresponds to a DIFFERENT world point each frame, so the
// old approach was really just heavy smoothing, not reprojection (it
// worked only because the neighborhood clamp kept it from diverging
// badly). Falls back to that same-UV clamp for sky pixels (depth==far
// plane), where a stable reprojection target doesn't exist.
inline const char* volumeTemporalPixelSource=R"HLSL(
sampler2D currentFrame:register(s0);
sampler2D historyFrame:register(s1);
sampler2D depthMap:register(s2);
// xy=one low-res texel, z=history weight, w=history valid
float4 temporal:register(c0);
// x=P22,y=P32,z=P00,w=P11 (this frame's projection)
float4 projUnpack:register(c1);
float4 invView0:register(c2);
float4 invView1:register(c3);
float4 invView2:register(c4);
// xyz=camera world position (this frame)
float4 cameraPos:register(c5);
// x=viewport MinZ, y=viewport Z range, z=viewport MaxZ
float4 depthRange:register(c10);
// LAST frame's raw forward (world->view) matrix rows (row-vector
// convention: pos_view = pos_world * M, row3 = translation)
float4 prevView0:register(c6);
float4 prevView1:register(c7);
float4 prevView2:register(c8);
float4 prevView3:register(c9);

float4 clampedHistory(float2 uv,float4 c) {
    float4 a=tex2D(currentFrame,uv+float2(temporal.x,0));
    float4 b=tex2D(currentFrame,uv-float2(temporal.x,0));
    float4 d=tex2D(currentFrame,uv+float2(0,temporal.y));
    float4 e=tex2D(currentFrame,uv-float2(0,temporal.y));
    float4 lo=min(c,min(min(a,b),min(d,e)));
    float4 hi=max(c,max(max(a,b),max(d,e)));
    return clamp(tex2D(historyFrame,uv),lo,hi);
}

float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float4 c=tex2D(currentFrame,uv);
    if(temporal.w<.5)return c;

    float raw=tex2D(depthMap,uv).r;
    float sky=step((depthRange.z+1)*.5,raw);
    // Keep reconstruction depth separate from clear/geometry classification.
    raw=saturate((raw-depthRange.x)/max(depthRange.y,.001));
    if(sky>.5) {
        // Sky: no stable world point to reproject - same-UV clamp only.
        return lerp(c,clampedHistory(uv,c),temporal.z);
    }

    float z=projUnpack.y/(raw-projUnpack.x);
    float3 viewPos=float3((uv.x*2-1)/projUnpack.z,(1-uv.y*2)/projUnpack.w,1)*z;
    float3 worldPos=cameraPos.xyz
        +viewPos.x*invView0.xyz+viewPos.y*invView1.xyz+viewPos.z*invView2.xyz;

    // The raw view matrix is stored as rows, but a row-vector world*view
    // transform produces each output from a matrix column. Using row dots
    // transposes the camera motion and sends shaft history to the wrong
    // screen location whenever the camera rotates.
    float3 prevViewPos=float3(
        worldPos.x*prevView0.x+worldPos.y*prevView1.x+worldPos.z*prevView2.x+prevView3.x,
        worldPos.x*prevView0.y+worldPos.y*prevView1.y+worldPos.z*prevView2.y+prevView3.y,
        worldPos.x*prevView0.z+worldPos.y*prevView1.z+worldPos.z*prevView2.z+prevView3.z);

    if(prevViewPos.z<=0.05) return lerp(c,clampedHistory(uv,c),temporal.z);

    float2 prevUv=float2(
        0.5+0.5*(prevViewPos.x/prevViewPos.z)*projUnpack.z,
        0.5-0.5*(prevViewPos.y/prevViewPos.z)*projUnpack.w);

    // Out of bounds (off-screen last frame, camera cut/teleport, or no
    // valid previous frame at all): no reprojection target - same-UV clamp.
    if(prevView0.w<0.5||prevUv.x<0.0||prevUv.x>1.0||prevUv.y<0.0||prevUv.y>1.0)
        return lerp(c,clampedHistory(uv,c),temporal.z);

    float4 lo=c,hi=c;
    float4 a=tex2D(currentFrame,uv+float2(temporal.x,0)); lo=min(lo,a); hi=max(hi,a);
    float4 b=tex2D(currentFrame,uv-float2(temporal.x,0)); lo=min(lo,b); hi=max(hi,b);
    float4 d=tex2D(currentFrame,uv+float2(0,temporal.y)); lo=min(lo,d); hi=max(hi,d);
    float4 e=tex2D(currentFrame,uv-float2(0,temporal.y)); lo=min(lo,e); hi=max(hi,e);
    float4 h=clamp(tex2D(historyFrame,prevUv),lo,hi);
    return lerp(c,h,temporal.z);
}
)HLSL";

inline const char* rayCompositePixelSource=R"HLSL(
sampler2D rays:register(s0);
sampler2D resolvedFog:register(s1);
sampler2D sceneDepth:register(s2);
// x=valid current-frame fog, y=raw diagnostic mask
float4 fogLink:register(c0);
// Viewport MaxZ; D3D's depth clear remains 1.0.
float4 depthInfo:register(c1);
float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
    float4 ray=tex2D(rays,uv);
    if(fogLink.x>.5&&fogLink.y<.5) {
        float transmission=saturate(1-tex2D(resolvedFog,uv).a);
        // Shared integrated medium: opaque banks extinguish rays, and maximum
        // visibility occurs at intermediate thickness. The atmosphere's sky
        // optical depth is analytically ~0 above the horizon (horizon^2 in the
        // integrate pass), so looking at the sun through clear sky gives T~1 and
        // the bell curve alone would erase exactly the rays this pass draws.
        // depthInfo.y is the clear-air floor: it keeps those rays while banks
        // still extinguish them, since the floor scales with T^2 and vanishes
        // in thick fog.
        float clearFloor=depthInfo.y;
        float visibility=max(4*transmission*(1-transmission),clearFloor*transmission*transmission);
        ray.rgb*=visibility;
    }
    // Radial beams are an overlay: they may fall across clouds, characters,
    // buildings and trees instead of being clipped to open sky. The source
    // mask above already keeps the origin on the sun/moon disc; the blur is
    // allowed to land on any pixel so the rays read as beams in front of
    // the world rather than only in the gaps between silhouettes.
    // Last quantization before the additive blend onto the 8-bit scene:
    // half a level of fixed dither keeps the ray falloff from banding into
    // rings around the source (signed, so the blend can round either way).
    ray.rgb+=(frac(52.9829189*frac(dot(vpos,float2(.06711056,.00583715))))-.5)/255.0;
    return ray;
}
)HLSL";

inline const char* solarRadialPixelSource=R"HLSL(
sampler2D sourceMask:register(s0);
// xy=sun position, z=total ray reach, w=per-sample decay
float4 radial:register(c0);
float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
    // 28 taps (was 16): smoother falloff/less banding on long reaches,
    // still cheap at half-res.
    // 48 taps reach the whole segment to the source at this spacing; the old 28
    // covered only the last ~12% of the path, which is why rays never left the disc.
    static const int kSamples=48;
    float2 stepUv=(radial.xy-uv)*(radial.z/float(kSamples));
    // Every pixel used to march at exactly the same 28 phases toward the
    // sun, so the taps landed on the same half-res texels in lockstep and
    // the result showed discrete spokes/rings around the disc. Offset the
    // phase per pixel with the fixed stratification hash (never animated,
    // so this cannot flicker): the spokes become fine grain, which the
    // separable blur and the temporal accumulation below average away.
    float jitter=frac(52.9829189*frac(dot(vpos,float2(.06711056,.00583715))));
    float illumination=1;
    float weightSum=0;
    float4 light=0;
    float2 q=uv-stepUv*(1.0-jitter);
    [unroll] for(int i=0;i<kSamples;++i) {
        q+=stepUv;
        float weight=illumination;
        light+=tex2D(sourceMask,q)*weight;
        weightSum+=weight;
        illumination*=radial.w;
    }
    light.rgb=light.rgb/max(weightSum,.001);
    // Keep the gather's contrast lift restrained: the wider sub-1 curve used
    // to raise faint source-mask values into a broad circular wash around the
    // disc. Compact falloff plus a near-linear lift preserves the beams and
    // the shadows they pick up from branches and rooflines.
    light.rgb=pow(saturate(light.rgb),0.9);
    // Half a level of dither into the 8-bit ray target: without it the
    // smooth radial falloff quantizes into concentric rings around the
    // source, and the 0.72-weighted temporal history toggles a level at a
    // time trying to sit between two of them.
    light.rgb+=(jitter-.5)/255.0;
    light.a=1;
    return light;
}
)HLSL";

// Debug-only crosshair drawn at the exact screen position rays/glare use for
// the celestial source. If this ring does not sit on the real sun/moon disc
// at any yaw/pitch, the source projection is still wrong - this is the check.
inline const char* celestialMarkerPixelSource=R"HLSL(
// xy=target uv, z=valid, w=unused
float4 markerTarget:register(c0);
float4 markerColor:register(c1);
// xy=1/render size (pixels -> uv)
float4 markerSize:register(c2);
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float valid=markerTarget.z;
    float2 d=(uv-markerTarget.xy)/markerSize.xy;
    float dist=length(d);
    float ring=abs(dist-16.0);
    float ringMask=1-smoothstep(1.2,2.6,ring);
    float onXAxis=(1-smoothstep(1.2,2.2,abs(d.y)))*step(6.0,dist)*step(dist,26.0);
    float onYAxis=(1-smoothstep(1.2,2.2,abs(d.x)))*step(6.0,dist)*step(dist,26.0);
    float mask=saturate(ringMask+onXAxis+onYAxis)*valid;
    return float4(markerColor.rgb*mask,mask);
}
)HLSL";

inline const char* postProcessPixelSource=R"HLSL(
sampler2D scene:register(s0);
// x=brightness, y=contrast, z=gamma, w=sharpness
float4 postParams:register(c0);
// xy=1/renderWidth, 1/renderHeight
float4 renderSize:register(c1);
// x=anti-aliasing enabled (0/1), yzw spare
float4 aaParams:register(c2);

// ANTI-ALIASING, and the reason it is here rather than in the client's own video options: a
// multisampled depth buffer cannot be sampled by a pixel shader in D3D9 at all, so setting the
// client above 1x multisampling takes the depth away from every effect that reads it - water
// refraction, absorption and shore foam, ambient occlusion, local lighting, rain roof occlusion,
// fog - and they do not degrade, they disappear. The client is therefore meant to run at 1x and
// the edges come from this pass instead.
//
// This is FXAA: find the contrast edge nearest the pixel, work out which way it runs, and resample
// along it. It is spatial, so it needs no motion vectors, no history buffer and no sub-pixel
// jitter in the client's projection matrix - which is exactly what makes it addable from a proxy
// at all, because a temporal resolve would need jitter injected into D3DTS_PROJECTION before the
// client draws, and hooking the client's own transforms is a different and much larger change.
// The cost of being spatial is that it cannot recover detail the geometry edge destroyed, so it
// reads as slightly softer than 2x hardware multisampling on thin railings and ropes; the
// Sharpness slider next to it in F7 is the companion knob for that.
//
// It runs on the scene exactly as the client left it, which is already display-referred 8-bit
// colour, so the luma measured below is the luma the eye sees - the space FXAA's thresholds are
// written for. The user's grade is applied AFTER, so a brightness or gamma setting cannot move an
// edge past a threshold that was tuned for ungraded values.
float Luma(float3 c){return dot(c,float3(.299,.587,.114));}

// `blended` comes back 1 only on a pixel that actually sat on an edge, and that is not cosmetic:
// the sharpening Laplacian below takes its four neighbours straight from the un-anti-aliased
// scene, so on a pixel this has already blended, centre and neighbours would no longer be samples
// of the same image and their difference would ring as a halo along exactly the edge being
// smoothed. Flat and textured pixels are returned untouched, so sharpening stays correct - and at
// full strength - everywhere it is safe.
float3 AntiAlias(float2 uv,float2 texel,float3 centre,out float blended){
    blended=0;
    // The four taps are HALF a texel out, not a whole one, and that is the part of this algorithm
    // that is easy to get wrong: at +-0.5 texel a tap lands exactly on the corner four pixels
    // share, so the sampler's own bilinear filter returns the average of that 2x2 box. Four taps
    // therefore read sixteen pixels, and the luma gradient below is estimated at sub-pixel
    // resolution rather than at pixel centres. At +-1.0 texel the same code reads four single
    // pixels, the gradient is then blind to anything finer than a whole pixel, and a 45 degree
    // staircase - the commonest aliased edge there is - measures as two black samples and two white
    // ones, produces a step that runs exactly ALONG itself, and comes back untouched.
    float3 nw=tex2D(scene,uv+float2(-texel.x,-texel.y)*.5).rgb;
    float3 ne=tex2D(scene,uv+float2(texel.x,-texel.y)*.5).rgb;
    float3 sw=tex2D(scene,uv+float2(-texel.x,texel.y)*.5).rgb;
    float3 se=tex2D(scene,uv+float2(texel.x,texel.y)*.5).rgb;
    float lumaCentre=Luma(centre);
    float lumaNW=Luma(nw),lumaNE=Luma(ne),lumaSW=Luma(sw),lumaSE=Luma(se);
    float lumaMin=min(lumaCentre,min(min(lumaNW,lumaNE),min(lumaSW,lumaSE)));
    float lumaMax=max(lumaCentre,max(max(lumaNW,lumaNE),max(lumaSW,lumaSE)));
    // Local contrast first, because most of a frame is not an edge: flat water, sky, the interior
    // of a texture. Below this the five samples are the same colour as far as the eye is
    // concerned and blending them only costs detail. Both numbers are FXAA's own - an absolute
    // floor, so a dark gradient is not mistaken for an edge, and a fraction of the local maximum,
    // so a bright one is not either.
    if(lumaMax-lumaMin<max(.0312,lumaMax*.125))return centre;
    // dir is the luma gradient ACROSS the edge. Dividing by its smaller component (biased, so a
    // perfectly axis-aligned edge cannot divide by zero) and clamping to 8 texels turns it into a
    // step that runs ALONG the edge - which is the whole trick: two more samples along it, instead
    // of a search, and the clamp is what stops a near-axis-aligned edge from reaching across the
    // screen and smearing an unrelated pixel into this one.
    float2 dir=float2(-((lumaNW+lumaNE)-(lumaSW+lumaSE)),((lumaNW+lumaSW)-(lumaNE+lumaSE)));
    float dirReduce=max((lumaNW+lumaNE+lumaSW+lumaSE)*.03125,1./128.);
    float rcpDirMin=1./(min(abs(dir.x),abs(dir.y))+dirReduce);
    dir=min(float2(8.,8.),max(float2(-8.,-8.),dir*rcpDirMin))*texel;
    // The narrow pair straddles the edge a third and two thirds along that step; the wide pair adds
    // the two samples at the step's ends and is sharper but can overshoot.
    float3 narrow=.5*(tex2D(scene,uv+dir*(1./3.-.5)).rgb+tex2D(scene,uv+dir*(2./3.-.5)).rgb);
    float3 wide=narrow*.5+.25*(tex2D(scene,uv+dir*-.5).rgb+tex2D(scene,uv+dir*.5).rgb);
    blended=1;
    // Take the wide result only while it stays inside this pixel's own luma range. Overshooting
    // that range is the classic FXAA dark halo on a bright edge, and falling back to the narrow
    // pair is what stops it - the range is already measured above, so the check is free.
    float lumaWide=Luma(wide);
    return (lumaWide<lumaMin||lumaWide>lumaMax)?narrow:wide;
}

float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
    float4 c=tex2D(scene,uv);
    float3 col=c.rgb;
    float aaEdge=0;
    // No [branch] here, and that is not an oversight. Forcing a dynamic branch around a body that
    // samples with an implicit LOD is rejected outright by fxc on ps_3_0 - X3528, "can't force
    // branch with gradients on non-inputs" - because derivatives are undefined inside flow control,
    // and all eight taps in there are tex2D. A plain if costs nothing in this case anyway: the
    // condition is a shader constant, so it is uniform across the whole frame and the compiler is
    // free to branch on it itself, which is what the sharpening if below has always done.
    if(aaParams.x>.5)col=AntiAlias(uv,renderSize.xy,c.rgb,aaEdge);
    if(postParams.w>.005) {
        float2 off=renderSize.xy;
        float3 up=tex2D(scene,uv+float2(0,-off.y)).rgb;
        float3 down=tex2D(scene,uv+float2(0,off.y)).rgb;
        float3 left=tex2D(scene,uv+float2(-off.x,0)).rgb;
        float3 right=tex2D(scene,uv+float2(off.x,0)).rgb;
        float3 lap=col*4.0-(up+down+left+right);
        col=saturate(col+lap*(postParams.w*.75)*(1.0-aaEdge));
    }
    col=col+postParams.x;
    col=(col-.5)*postParams.y+.5;
    col=pow(max(col,0.0),1.0/max(postParams.z,.01));
    // Gamma/contrast remapping stretches the 8-bit steps of smooth
    // gradients (the sky around a light source first) into visible bands;
    // half a level of fixed dither breaks them without animating.
    col=saturate(col)+(frac(52.9829189*frac(dot(vpos,float2(.06711056,.00583715))))-.5)/255.0;
    return float4(col,c.a);
}
)HLSL";
