# Global illumination (screen-space bounce)

`src/Effects/GlobalIllumination.{h,cpp}` adds one cheap bounce-light pass to
the composite. It is not a path tracer and does not pretend to be one: three
low-frequency terms, evaluated together at quarter resolution, that lift the
flat ambient the client leaves behind without touching its direct light.

## What it computes

For every non-sky, non-water pixel, at low resolution:

1. **Hemispheric sky/ground ambient.** Upward faces take the sky colour,
   downward faces a warm dark ground bounce. The sky colour is the zone's own
   authored fog colour when the water draws captured one this frame (the same
   colour the atmosphere and the cloud deck already use), otherwise a day/night
   blend of the client's directional light over a cool floor. A subtle
   sun-lean brightens the sky side toward the stabilized light.
2. **One-bounce colour from the scene itself.** A hardware-downsampled
   quarter-res copy of the scene is sampled along a fixed golden-angle spiral
   (8 taps at BALANCED/HIGH, 4 at LOW), weighted by a hemisphere gate around
   the depth-derived normal and a bilateral depth weight. Surrounding surfaces
   donate their colour; sky and water taps are rejected.
3. **Soft lamp bleed.** The same dynamic point/spot selection the direct lamp
   pass uses (up to 8, cone-aware) is re-evaluated diffuse-only with a wider,
   softer falloff, so lamp pools bleed onto nearby walls and ground instead of
   ending at the pool edge.

A **depth-cavity term** from the same spiral keeps all three out of creases:
surroundings closer than the pixel hide its sky. The low-res result is
bilaterally upsampled with the full-res depth (slope-aware tolerance, the
same pattern as the direct lamp composite) and added to the scene with half a
level of static dither.

## Pipeline position and wiring

The pass runs inside `volume::Composite`, after the cloud composite and the
direct dynamic lights, and before the atmosphere:

```
clouds -> dynamic lights -> GLOBAL ILLUMINATION -> atmosphere -> rays -> post
```

It publishes back into `FrameContext::sceneColor/sceneSurface`, so the
atmosphere scatters the bounced light and the fog wash applies on top of it
with no further plumbing. When the atmosphere is off, the lit surface is
blitted to the target by the same fallback that serves the lamp pass.

| Source | How it is followed |
|---|---|
| Dynamic lights | Consumes `LocalLightManager::Selected()` (populated earlier in the same composite, never re-selected, so smoothing runs once), `IntensityScale()`, and the on/off switch. `LampBouncePercent` scales the bleed; 0 keeps direct pools only. |
| Native shadows | Implicit: the bounce source is the already-shadowed scene, so a native cascade darkens the bounced light exactly where it darkened the direct light. Nothing is double-counted. |
| Cloud shadows | The bounce respects them implicitly (same reason), and the sky ambient is additionally dimmed per-pixel through the 512 cloud shadow map with the same projection the atmosphere uses. Strength follows `CLOUD SHADOWS`; `GI CLOUD SHADOWS` decouples it for A/B. |
| Cloud lighting / weather | While the deck renders, sky ambient, bounce and sun-lean close down with the live overcast (`VolumetricClouds::OvercastFactor()`: authored coverage plus smoothed storm amount), which is what dims the world when native rain/snow/sand fills the sky. Disabled clouds or a genuinely clear sky leave the ambient alone. `GI STORM DIMMING` decouples it. |
| Day / night | Sky palette follows the captured fog colour (which already reflects zone and time); the fallback path blends the client's directional colour by the daylight/moonlight factors, and the sun-lean uses the stabilized direct light (`CelestialStability.h`), so panning the disc off-screen never blinks the ambient. |
| Atmosphere / fog | Runs before it; fog scatters the result. No setting on either side needs to know about the other. |
| Water / sky / UI | Water pixels (water mask) and sky pixels (viewport-MaxZ-aware depth test) are excluded from both the integrate and the composite; the composite runs before UI draws, so the interface is never touched. |
| Location profiles | The master multiplier rides on the location `LightingMultiplier`, and every `[GlobalIllumination]` key is location-tunable (`LocationGraphics.ini` zone/subarea scopes) like the other lighting settings. |

Deliberately not temporal: single-frame bilateral filtering has no history to
invalidate on camera cuts, Alt+Tab, or light-selection changes, so the pass
cannot ghost or flash.

## Cost

- One hardware `StretchRect` downsample (no shader), one low-res integrate
  (~30 texture fetches, two short `[loop]`s: 4/8 taps + 4/8 lamps), one
  full-res bilateral composite (4 taps + 4 depth taps).
- Quality: 0 = quarter-res, 4 taps/lamps; 1 = quarter-res, 8 taps/lamps;
  2 = half-res, 8 taps/lamps, 30% wider radius.
- Extra VRAM: one quarter-res scene copy, one quarter-res float GI buffer,
  one full-res lit copy (all released with the device; HIGH doubles the two
  low-res buffers).
- The integrate shader is budgeted at 512 instruction slots in
  `tools/ValidateShaders.ps1`, like the other march-style shaders.
- Fail-closed with the shared `RetryGate`: a lost device or a card without
  float render targets skips the pass (leaving the scene untouched) and
  retries after the standard cooldown. Failures are logged once per streak to
  `GlobalIllumination.log`.

## Tuning

`[GlobalIllumination]` in `GraphicsEffects.ini`, mirrored on F7 under
`GLOBAL ILLUMINATION`. The sliders are artistic multipliers on top of base
gains chosen so the shipped defaults lift shadows without washing the
client's direct light out:

- `IntensityPercent` (master), `SkyAmbientPercent`, `BouncePercent`,
  `OcclusionPercent`, `LampBouncePercent`, `RadiusPercent` (world size of the
  bounce/cavity footprint), `Quality`, `DebugMode`.
- `FollowCloudShadows` / `FollowWeather` decouple the two cloud couplings.
- `DebugMode`: 0 final, 1 bounced light only, 2 occlusion only, 3 bounce only.

If interiors go too bright, lower `SkyAmbientPercent` first (it is the only
term that does not need a nearby lit surface); if lamp bleed smears across a
wall, lower `RadiusPercent` before touching `LampBouncePercent`.

## Limits

- Screen-space information only: what is off-screen cannot bounce, and the
  depth-derived normals inherit the usual silhouette limits (mitigated by the
  neighbour-pick and the bilateral weights, not eliminated).
- No specular or glossy bounce; no multi-bounce. The second bounce is the
  atmosphere's fog wash, not this pass.
- Native shadow *strength* changes the bounce implicitly through the scene
  copy; there is deliberately no direct coupling to the shadow softness or
  strength sliders.
