# Modern WoW Renderer — TODO

## Current stabilization priorities

- [x] Make the sea hold still in the world: the lattice coordinate is read from the draw's own matrix rows (world axes in view space) instead of across them, and the wave phase is anchored to the camera of the draw being shaded rather than to a frame-captured one, so turning or moving the camera no longer moves the water. Live comparison against a 3.3.5 coast remains.
- [x] Give waves a readable pace and a body: the lattice orientation is a constant now (it used to rotate about the world origin, which translates the sea at hundreds of yards a second at any camera distance), crests travel at the dispersion speed of their own wavelength, and geometry waves lift along world up and slide toward the crest (Gerstner), with the shading slope divided by the Jacobian and the steepness clamped fold-safe on the CPU.
- [x] Foam at the edges of waves: whitecaps from the Gerstner pinch plus the crest mask and the shoal, all gated by each body's authored WIND and sea state, damping the reflection and the sun road - a calm zone stays foamless, a storm on an exposed coast caps every third crest.
- [x] Re-measure the water pixel stage's instruction slots with fxc on the Windows runner. Round 4's
  six trains and grouping envelope measured **642**, not the estimated ~607 (the estimate was 4x low:
  vectorised `sincos` batches hide their per-lane price). The 621 budget is gone by decision, not by
  drift: water now sits in `$measureOnly` in `tools/ValidateShaders.ps1` - counted, annotated on every
  run, still a hard failure if the count cannot be read - because a ceiling that starts deciding which
  waves are allowed to shade has stopped being a guard and become a design input. The device answer was
  always the runtime one: a card reporting fewer slots than the shader needs fails `CreatePixelShader`
  and keeps native water. What the count is for now: it is visible in the Checks tab, so a change that
  moves it by tens has to be argued in the pull request that makes it.
- [x] Stop the replacement water from showing square patterns: the wave field is anchored to the terrain instead of to a tile's uv, the noise hash is bounded so it cannot collapse onto its lattice, and the unresolved octave fades in value as well as in slope. Live comparison against a 3.3.5 coast remains.
- [x] Stop the waves marching in lines: the swell is six trains whose wavelengths are never an
  octave apart and whose crest lines are never closer than 21 degrees, each starting at an
  irregular point of its own wavelength; the crest warp is strong enough to bend a train instead
  of wobbling a line; and a world-anchored grouping envelope multiplies the lift, the slide, the
  shading slope, the pinch, the chop and the whitecaps together, so water arrives in sets instead
  of at constant height everywhere. Live comparison against a 3.3.5 sea remains.
- [x] Break the lines that survived at range: two shorter trains (2.55 and 1.95 cells, at 65 and
  128 degrees, on their own pixel register c218) now cross the coarse crest families at the
  scale distance resolves, the crest warp rose to 6.5 cells, and a finer counter-phased chop
  octave removes the last regular pattern standing. `tools/WaterRegression.cpp` measures it:
  along the dominant train's own crest line the field must keep varying (0.71 of its overall
  spread today, 0.00 for a lone sine). Zones also author their own water colours now
  (`WaterColour`/`DeepColour`/`AbsorptionColour` in EnvironmentProfiles.ini - azure Booty Bay,
  darker Duskwood/Plaguelands/Felwood, icy Winterspring/Icecrown), resolved and blended by the
  same area/zone machinery as the agitation they sit next to. Live comparison remains.
- [x] Stop whole liquid tiles from staying native: the documented water *pixel* shader plus the two-texture signature identifies the material at any surface size, the fourth live-confirmed vertex variant is listed, and both fog paths share the water register table instead of naming one variant. Unmatched liquid layouts are now reported by hash in `WaterEffect.log`.


- [x] Restore native shadow-map creation with the actual 3.3.5 CVars (`extShadowQuality=5`, `projectedTextures=1`). Softness and strength are live-verified across both receiver families (`c12.xy` and terrain/WMO `c8.yz`).
- [x] Match water by its stable two-texture liquid signature rather than shader hashes. Live logs confirmed a fourth VS variant (`70faf83955e2b668`) on the same water; rejecting it caused triangular canal/coast holes and angle-dependent missing chunks. Runtime stress test remains for Stormwind, Stranglethorn, Booty Bay, and Northrend.
- [x] Use that same structural identity for the pre-water SSR/refraction capture. The leftover three-VS whitelist delayed capture until mid-surface, so later tiles sampled a scene copy that already contained earlier water tiles and formed large camera-dependent seams.
- [x] Occlude the Sun/Moon water glint with a short screen-space light ray through the captured pre-water depth buffer.
- [x] Replace the broad white water blanket with a narrow, broken reflected-sun path derived from `references/water-sun-path.png`.
- [x] Protect already-dark source mortar from double cavity darkening and give stone terrain layers extra normal/parallax response for paved-road depth. Runtime visual tuning remains.
- [x] When both celestial discs are drawn, select the tracked Moon during moon-lit conditions and the Sun during daylight. This prevents a still-visible sun candidate from owning a full solar halo at night while the actual moon has no shafts.

## Later: interactive local volumetric fog

- [x] Add a separate local ground-fog density layer integrated physically with the existing atmospheric compositor (not a second fullscreen alpha overlay). Runtime visual tuning remains.
- [x] Prototype a DX9-friendly world-space interaction field around the player/camera: noise-shaped density, directional capsule wake, smoothed motion, decay, and gradual refill. A persistent 2.5D texture atlas remains a possible later upgrade if the analytic field proves insufficient.
- [x] Add local environment-light injection to surfaces and the atmospheric raymarch, with depth-derived normals, screen-space occlusion, water/UI exclusion, temporal-history invalidation, and a fail-closed DX9 resource path.
- [ ] Populate the authoritative world-space WMO/M2 light manifest from placement exports; ambiguous emissive/model-local candidates remain in `MaterialCache/LocalLightReview.json` and are never auto-enabled.
- [ ] Extend the local fog with location/weather/indoor profiles and native-shadow sampling before considering a full froxel atlas.

## Sky

- [x] Add a raymarched volumetric cloud deck integrated into the scene before the atmosphere compositor, sharing the existing noise atlas, sky-masked by depth, and coupled to detected weather. Runtime visual tuning remains.
- [ ] Project the deck's transmittance onto the ground as moving cloud shadows, reusing the same slab and wind so the shadows match the clouds that cast them.
- [ ] Give the deck location/weather profiles (overcast valleys, clear deserts) instead of one set of zone-tuned values.
