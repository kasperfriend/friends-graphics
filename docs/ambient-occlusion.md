# Ambient occlusion

Ambient occlusion (AO) darkens the places ambient light struggles to reach:
where an object meets the ground, in the crease of a step, in a corner, inside a
cavity. It is a screen-space effect built from the scene depth the renderer
already captures, so it needs no extra geometry, no extra draw calls and no
cooperation from the client's shaders.

- **F7 → AMBIENT OCCLUSION** holds the whole feature: the on/off switch,
  `AO STRENGTH`, `AO RADIUS`, `AO DISTANCE FADE` and an `AO DEBUG VIEW`.
- Settings live in the `[AmbientOcclusion]` section of `GraphicsEffects.ini` and
  obey the same per-location overrides as every other section (global, zone,
  subarea). `F12` reloads them without restarting.
- It runs inside the atmosphere composite, which means it needs
  `[Volume] Enabled=1` in `ModernWoWRenderer.ini`. Everything else about it is
  independent of fog, rays and clouds.

## How it works

Two pixel shaders (`src/Effects/AmbientOcclusion.cpp`) do the work, and their
math has exactly one definition: `src/Effects/AmbientOcclusionCore.h`. That
header compiles without `windows.h` so the offline tools run the same code.

1. **Generate** (`kAmbientOcclusionSource`), at full resolution. For each pixel
   the view-space position and a depth-derived normal are reconstructed from the
   INTZ depth buffer, then 12 directions from a golden-angle hemisphere are
   rotated per 4x4 pixel block and traced: each sample point is projected back
   to the screen, and the surface found there counts as an occluder when it sits
   in front of the sample.
2. **Apply** (`kAmbientOcclusionApplySource`). A depth-aware 4x4 box blur of
   that term, multiplied into the scene. Gentle slopes blur normally; a
   silhouette gets zero weight, so occlusion never smears across an object edge
   onto the background behind it.

The pass is the **first** effect applied to the scene, before clouds, local
light, fog and glare. That ordering is the whole reason the effect only ever
darkens world geometry: everything composited afterwards is untouched, and the
user interface is drawn later still and is never sampled.

Sky, water (via the per-frame water mask) and anything behind the far plane are
excluded outright.

## Why the defaults are what they are

The tuning question is never "how much occlusion can we get" but "how much can
we get before open surfaces start to darken". A wide kernel is the one thing
that turns AO into dirt, and it does it invisibly - nothing in a single frame
looks wrong, the world just gradually goes grey.

Measured on a synthetic scene with a known answer
(`tools/AmbientOcclusionRegression.cpp`), at the shipped strength of 60 %:

| radius | open floor | open wall face | isolated open top | right-angled corner |
| ------ | ---------- | -------------- | ----------------- | ------------------- |
| 1 (shipped) | 1.000 | 1.000 | 1.000 | 0.746 |
| 2      | 0.950 | — | 1.000 | — |
| 3      | 0.950 | — | 1.000 | — |
| 4      | 0.882 | — | — | — |

At **radius 1** every kind of open surface reads exactly `1.000` - not
"nearly" untouched, untouched - while a corner still reaches `0.75` and the
ground against a boulder reaches `0.69`. At radius 2 the open floor has already
lost 5 % to a shadow that is not there. So the radius ships at 1 and the
authored range stops at 6; raising it is a deliberate, visible trade, not a
free win.

The two other constants were tuned the same way:

- **Softness** (`radius * 0.25`) is how far in front of a sample point an
  occluder must sit before it counts in full. A wider ramp silently discards
  most of the occlusion and leaves AO invisible.
- **Locality** (full credit within 2 radii of the receiver's depth, nothing
  past 2.45) stops a wall metres in front of the ground from darkening the
  ground behind it. It is generous enough to keep genuine contact occlusion -
  2.0 beats a tight 0.55 by a wide margin on real corners.

In the shipped preview the effect darkens 22 % of the visible geometry by more
than 3 %, averages 3 % across the frame, and bottoms out at 0.62 in the deepest
crease: enough to seat objects and read every corner, not enough to grey
anything.

## Preview shots

`tools/AmbientOcclusionPreview.cpp` renders a synthetic stone ruin on the CPU
and runs the core's own math over the resulting depth buffer, producing the on
/ off sheet this look was tuned against:

```text
g++ -O2 -std=c++20 tools/AmbientOcclusionPreview.cpp -o build/aopreview
build/aopreview --out Screenshots/ambient-occlusion.png --stats
```

`--stats` also probes named points on the model (open ground, the foot of the
steps, the column base, the boulder contact, and so on) and prints the
occlusion at each. Every probe is checked for visibility by marching the scene
toward it, so a reading can never be attributed to a surface the label does not
describe.

## Known limitations

- **Contact normals are contaminated by the adjacent surface.** The normal is
  reconstructed from neighbouring depth samples, and right where a column meets
  the floor those neighbours can belong to the column. The hemisphere then opens
  slightly sideways and the contact darkens less than it should - the column
  base in the preview reads 0.94 where a true normal would give about 0.7. This
  is inherent to screen-space reconstruction, not a bug, and it errs toward
  *too little* occlusion rather than too much.
- **Only visible geometry occludes.** An off-screen wall cannot darken the
  ground beside it. This is why AO is a contact and crease effect here and not a
  substitute for the client's own shadow maps.
- **Thin geometry can be missed.** With radius 1, an occluder thinner than about
  a yard may be stepped over by the samples.
- **Cost.** Roughly 50 texture fetches per pixel at full resolution (12 samples
  plus the normal and mask in the generate pass, then 16 depth and 16 occlusion
  taps in the blur). If that matters on a given machine, turn the effect off in
  F7 - it is independent of every other feature.
- **Final verification needs the client.** The regressions pin the math and the
  portable tools render it, but how it reads over real Ascension terrain, in
  rain, and at night still has to be judged in game.
