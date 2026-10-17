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
        float openness=step((sun.w+1)*.5,raw);
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
        // Whole-frame god-ray source (GodRayCore.h SourceWeight): a sunward
        // sky glow reaches across the frame, so the gather below draws beams
        // away from the source. Trees, walls, characters and ground have
        // openness 0 and cut those beams; cloud alpha thins them. The disc
        // core is kept inside the max, so the source itself stays brightest.
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

// Depth-derived screen-space directional contact shadows. A ray is marched
// from each visible surface toward the real sun; only a depth crossing along
// that ray darkens the pixel, so flat ground is never globally multiplied down.
inline const char* contactShadowPixelSource=R"HLSL(
sampler2D depthMap:register(s0);
// xy=1/full render size, z=strength, w=max view-space trace distance
float4 tuning:register(c0);
// x=projection A, y=projection B, z=P00, w=P11
float4 projection:register(c1);
// xyz=view-space direction toward the sun, w=viewport depth maximum
float4 light:register(c2);
float linearZ(float raw) {
    // Classify clear depth in the original viewport range, then normalize
    // separately for projection reconstruction. MaxZ can itself be near one.
    float clearThreshold=(light.w+1)*.5;
    return raw>=clearThreshold ? 100000 : projection.y/(raw/max(light.w,.001)-projection.x);
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float raw=tex2D(depthMap,uv).r;
    float center=linearZ(raw);
    if(center>99999)return 1;
    float3 viewPos=float3((uv.x*2-1)*center/projection.z,
                          (1-uv.y*2)*center/projection.w,center);
    float3 towardLight=normalize(light.xyz);
    float occlusion=0;
    [unroll] for(int i=0;i<12;++i) {
        float q=(i+1)/12.0;
        float traceDistance=.30+q*q*tuning.w;
        float3 samplePos=viewPos+towardLight*traceDistance;
        float2 sampleUv=float2(.5+.5*samplePos.x*projection.z/max(samplePos.z,.05),
                               .5-.5*samplePos.y*projection.w/max(samplePos.z,.05));
        float inside=step(0,sampleUv.x)*step(sampleUv.x,1)*step(0,sampleUv.y)*step(sampleUv.y,1)*step(.05,samplePos.z);
        float sceneZ=linearZ(tex2Dlod(depthMap,float4(sampleUv,0,0)).r);
        float delta=samplePos.z-sceneZ;
        float bias=.055+center*.0012;
        float thickness=.30+traceDistance*.075;
        float crossing=smoothstep(bias,bias+thickness*.25,delta)*
                       (1-smoothstep(thickness,thickness*1.45,delta));
        occlusion=max(occlusion,inside*step(sceneZ,99999)*crossing*(1-q*.35));
    }
    float screenEdge=saturate(min(min(uv.x,uv.y),min(1-uv.x,1-uv.y))*24);
    float distanceFade=saturate(1-center/320.0);
    float shade=1-tuning.z*saturate(occlusion)*distanceFade*screenEdge;
    return float4(shade,shade,shade,1);
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
        // Shared integrated medium: clear air has little scatter; opaque banks
        // extinguish rays. Maximum visibility occurs at intermediate thickness.
        float visibility=4*transmission*(1-transmission);
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
    // Contrast lift: a plain weighted average reads as a soft, low-contrast
    // glow (the "blob" look) because most of the sampled path is usually
    // empty sky diluting the mean. A sub-1 power pulls dim/mid values up
    // faster than bright ones, so partial-occlusion streaks (branches,
    // rooflines) read as more defined beams instead of a uniform wash.
    light.rgb=pow(saturate(light.rgb),0.72)*1.2;
    // Half a level of dither into the 8-bit ray target: without it the
    // smooth radial falloff quantizes into concentric rings around the
    // source, and the 0.72-weighted temporal history toggles a level at a
    // time trying to sit between two of them.
    light.rgb+=(jitter-.5)/255.0;
    light.a=1;
    return light;
}
)HLSL";

inline const char* localLightPixelSource=R"HLSL(
sampler2D scene:register(s0);
sampler2D depthMap:register(s1);
float4 localTuning:register(c0);
float4 localMode:register(c1);
float4 depthInfo:register(c2); // viewport MaxZ
float3 sourceAt(float2 uv) {
    float3 c=tex2D(scene,uv).rgb;
    float peak=max(c.r,max(c.g,c.b));
    float bright=saturate((peak-localTuning.w)/max(1-localTuning.w,.01));
    float warm=smoothstep(.010,.11,c.r-c.g)*smoothstep(.025,.20,c.g-c.b);
    float blueShoulder=smoothstep(.001,.025,c.b);
    float amberRatio=1-smoothstep(.90,.98,c.g/max(c.r,.01));
    float raw=tex2D(depthMap,uv).r;
    // Clear depth is 1 while geometry is capped by viewport MaxZ; use the
    // gap between them rather than a fixed threshold that can leak bright
    // foreground into the local-light source mask.
    float worldGeometry=1-step((depthInfo.x+1)*.5,raw);
    return c*bright*warm*blueShoulder*amberRatio*worldGeometry;
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float3 raw=tex2D(scene,uv).rgb;
    float rawPeak=max(raw.r,max(raw.g,raw.b));
    float rawBright=saturate((rawPeak-localTuning.w)/max(1-localTuning.w,.01));
    float3 glow=(localMode.x>.5?float3(rawBright,0,0):sourceAt(uv))*localTuning.z*24.0;
    return float4(glow,1);
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
float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
    float4 c=tex2D(scene,uv);
    float3 col=c.rgb;
    if(postParams.w>.005) {
        float2 off=renderSize.xy;
        float3 up=tex2D(scene,uv+float2(0,-off.y)).rgb;
        float3 down=tex2D(scene,uv+float2(0,off.y)).rgb;
        float3 left=tex2D(scene,uv+float2(-off.x,0)).rgb;
        float3 right=tex2D(scene,uv+float2(off.x,0)).rgb;
        float3 lap=col*4.0-(up+down+left+right);
        col=saturate(col+lap*(postParams.w*.75));
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
