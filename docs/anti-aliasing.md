# Anti-aliasing, and why the client belongs at 1x multisampling (2026-10-17)

Reported as: *"moving from x1 multisampling to x2 and more completely removes most of
effects"*. That is not a bug in any one effect. It is what D3D9 does.

## The constraint

A multisampled depth buffer cannot be sampled by a pixel shader in D3D9 at all. There is no
resolve for it either: `StretchRect` will average a multisampled **colour** surface into a plain
one, but depth has no equivalent, and a `D3DUSAGE_DEPTHSTENCIL` texture cannot be created
multisampled in the first place. So the moment the client's own Multisampling goes above 1x,
`src/D3D9/DepthCapture.cpp` cannot install its INTZ replacement, and every effect that reads
scene depth loses its input at once:

- water refraction, depth absorption, shore foam and the shoal reading (`WaterEffect.h` gets
  `depthValid = 0`, which makes every pixel open ocean at a nominal 400 yard column);
- ambient occlusion and local lighting, which reconstruct normals from depth;
- rain roof occlusion and the distance-gated mist;
- ground fog and the local fog field.

They do not degrade. They disappear, which is exactly what was reported. `WaterEffect.h`'s MRT
binding is a second, smaller loss for the same reason: D3D9 requires every bound render target to
share one multisample type and quality, and the water mask/depth targets are created
non-multisampled. That guard stays - it is a real limit - but it costs the water's own outputs to
the atmosphere pass, not the water draw.

Two of the guards that followed from this were wrong, though, and are fixed:

- `WaterReflection.h` refused to capture the colour source when the target was multisampled, even
  though the very next line `StretchRect`s into a plain `D3DUSAGE_RENDERTARGET` texture - which
  *is* the colour resolve. Reflections and refraction were therefore throwing away their colour
  source for nothing above 1x. They now take it, and if a driver refuses the resolve the HRESULT
  path is unchanged: the frame is counted as failed and nothing else is affected.
- There was no anti-aliasing anywhere in the renderer, so a player who did the right thing and ran
  at 1x had no way to get their edges back.

## What the pass does

FXAA, in the existing image pass (`VolumeEffects.h`'s `postProcessPixelSource`, drawn through
`src/Effects/ImagePostProcess.h`). It runs where that pass has always run: at the first UI draw, so
the world is complete and the client's UI has not been drawn yet. Geometry edges are smoothed and
text stays crisp, with no UI detection of its own.

It is spatial rather than temporal on purpose. A temporal resolve needs sub-pixel jitter injected
into the client's projection matrix before it draws, and this renderer only ever *reads*
`D3DTS_PROJECTION` (`GetTransform` in `CameraCapture` and `CelestialTracker`) - there is no
`SetTransform` hook to put jitter into. Adding one is a different and much larger change, and it
would put the renderer's fingerprints on the client's own geometry. What FXAA needs instead is
already there: the backbuffer copy the pass was making for the grade.

The switch is `[PostProcess] AntiAliasing`, in F7 under **IMAGE & COLOR** next to Sharpness, on by
default. It is deliberately *not* a passenger on the `Enabled` master above it: that master means
"I want my colours graded" and ships off, while this one means "I want my edges smoothed", and
burying the replacement for hardware multisampling inside a colour-grading switch would hide it
from everyone who has never opened the tab. When the master is off and AA is on, the grade is
forced to identity in `VolumeIntegration.h` (`gradeWanted`), so a player cannot get their
brightness applied through the back door of a pass they turned on for their edges. The pass's own
`hasPost` gate and `PostProcessWithoutCamera` both accept AA as a reason to run.

## The detail that decides whether it works

The four diagonal taps are **half** a texel out, not a whole one:

```hlsl
float3 nw=tex2D(scene,uv+float2(-texel.x,-texel.y)*.5).rgb;
```

At ±0.5 texel a tap lands exactly on the corner four pixels share, so the sampler's own bilinear
filter returns that 2x2 box average. Four taps read sixteen pixels and the luma gradient is
estimated at sub-pixel resolution.

At ±1.0 texel the same code compiles, runs, costs eight fetches, and does nothing on the commonest
aliased edge there is. A 45 degree staircase then measures as two single black samples and two
single white ones; the gradient comes out running exactly *along* the staircase; the step along it
samples only pixels that are already on this side of the step; and the pixel is returned
untouched. It is the kind of mistake no amount of looking at the screen would find, because the
screen looks exactly like it did before.

The rest of the stage is the console-quality algorithm: a local-contrast early-out
(`lumaMax - lumaMin < max(.0312, lumaMax * .125)`) so flat water, sky and texture interiors are
returned untouched rather than smeared; a step along the edge clamped to 8 texels so a
near-axis-aligned edge cannot reach across the screen; and a choice between the narrow and wide
resamples, taking the wide one only while its luma stays inside this pixel's own range, which is
what stops the dark halo on a bright edge.

Sharpening interacts with it and had to be told. The Laplacian takes its four neighbours straight
from the un-anti-aliased scene, so on a pixel the AA has already blended, centre and neighbours are
no longer samples of the same image and their difference rings as a halo along exactly the edge
being smoothed. `AntiAlias` therefore also returns whether it blended, and the sharpen is
multiplied by `1 - aaEdge`. Flat and textured pixels are returned untouched by AA, so there the
sharpening is still correct and still runs at full strength.

The stage reads its switch from `c2.x`, and `ApplyImagePostProcess` takes that constant as a
required argument rather than a defaulted one: a register this pass does not write keeps whatever
the previous pass left in it, so an optional argument would be a way for a call site to silently
leave anti-aliasing switched on.

## Verification

`tools/AtmosphereRegression.cpp` builds a 45 degree staircase in the 128x128 test backbuffer out of
per-row clears and reads single pixels back. The assertions are written so that the tap-offset
mistake above cannot pass:

| pixel | AA off | AA on |
| --- | --- | --- |
| (63, 64) dark side of the step | 0 | 49 |
| (64, 64) bright side of the step | 255 | 207 |
| (20, 64) flat dark | 0 | 0 |
| (110, 30) flat bright | 255 | 255 |

With ±1.0 texel taps the two step pixels would read 0 and 255 with AA on, and the middle two
assertions would fail.

`tools/DeviceHealthRegression.cpp` pins the wiring without a device: that the colour guard is gone
from `WaterReflection.h` while the MRT guard stays in `WaterEffect.h`, that the taps are at ±0.5 and
the whole-texel spelling is not left anywhere, that the local-contrast early-out and the
`1 - aaEdge` sharpen suppression are present, that `c2` is always uploaded, that AA runs on its own
switch with an identity grade, and that the F7 item and the shipped INI key agree on a default.

## In-game check

Set the client's Multisampling to 1x and restart. Walk a shoreline or stand in a city and look at
railings, roof edges and rigging against sky: with **ANTI ALIASING** on in F7 → IMAGE & COLOR they
should be smooth, and text in the UI should be unchanged. Raise **SHARPNESS** if the image reads
soft. Set Multisampling to 2x and restart to see the other side of the constraint: the edges come
back from hardware, and water refraction, ambient occlusion, local lighting and rain occlusion all
go dark, because there is no depth to read. `GraphicsEffects.log` carries
`PostProcess effective ... anti-aliasing=N`, and `WaterReflection.log` records the depth loss.
