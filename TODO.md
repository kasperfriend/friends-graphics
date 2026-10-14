# Modern WoW Renderer — TODO

## Current stabilization priorities

- [x] Stop the replacement water from showing square patterns: the wave field is anchored to the terrain instead of to a tile's uv, the noise hash is bounded so it cannot collapse onto its lattice, and the unresolved octave fades in value as well as in slope. Live comparison against a 3.3.5 coast remains.
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
