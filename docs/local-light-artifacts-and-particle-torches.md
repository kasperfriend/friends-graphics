# Local light artifacts and particle torch follow-up

2026-09-28: user's screenshots show distant orange influence patches in LIGHT
DEBUG=1, no attached torch lights, horizontal light stripes, and a second shadow.

Changes:
- Removed screen-depth contact visibility from the injected surface light pass.
  Native shadows remain owned by the game; this avoids a second inferior shadow.
  Reorienting native shadows for each point light would require a different
  light-space rendering pipeline, not moving a composited shadow image.
- Surface light quality now uses 50/75/100% resolution. Bilateral reconstruction
  tolerance includes the continuous receiver depth slope, using the smaller
  one-sided depth differences to avoid treating silhouettes as slopes.
- LIGHT DEBUG=1 now shows actual shaded, attenuated contribution, rather than
  a bright artificial visualization of each source's radius. Mode 3's injected
  contact-occlusion view is empty now that the duplicate shadow is removed.
- Volumetric lamp scattering additionally decays with squared perpendicular
  distance to the emitter; broad remote banks no longer receive uniform light.
- Torch observation supports verified unskinned c31..c33 shader layouts, indexed
  and nonindexed buffers, and both UP APIs. Known texture signatures include
  shipped mip payloads down to 16 pixels. Separated flames in one batch are
  clustered separately, avoiding a fictitious averaged midpoint source.
- The WMO manifest generator now uses the winning MPQ version of each model and
  ADT, instead of unioning lights/placements from superseded versions. WMO count
  drops 5506 -> 5277; total with preserved M2 sources is 6412. No obsolete WMO
  entry was found within 150 units of the Goldshire comparison point, so this
  cleanup is not presented as the cause of those particular screenshot patches.

Validation: ten active ps_3_0 shaders compile (surface 196, reconstruction 123,
volume integration 502 slots); Release Win32 build and production GPU suite pass.
The new sloped-plane comparison checks the local interpolation envelope against
full resolution (maximum dark deficit 2/255, tolerance 6), rather than demanding
identical pixels around a saturated nonlinear lamp core. Real flame mip tests
cover skinned and unskinned draws, UP/nonindexed/indexed APIs, animated movement,
two spatially separate flames, and rejection of an ordinary texture. Existing
water boundary, fog, viewport depth, device-state and proxy ON/OFF/Reset tests pass.

The live log confirmed attached=0 in the previous version; no new live game
capture is available yet. Unknown custom particle shaders and unreadable buffers
remain unsupported. Native shadow direction is unchanged; no complete local
shadow-map implementation is claimed. Native map shadows and baked shading
cannot generally be repositioned per lamp by changing the injected light pass.

## Dynamic light noise: the window, not the tuning

2026-10-16: player report - "the light is fully made of noise now", with no F7
setting able to fix it. The previous round had traded the 1-2 pixel "lines" for
grain, and the grain was not a tuning problem.

Root cause, from a CPU reconstruction of the pass over a known scene
(`tools/LocalLightingRegression.cpp`): the tangent handed to the normal was
`(left + right) / 2 - receiver`, not `right - left`. For a symmetric window the
tangential part of that midpoint difference cancels and what remains is the
*curvature* residual of the window - a vector a hundred times shorter than the
window and pointing along the view ray, not along the surface. The cross product
of two such vectors is noise: on ground whose true normal is exactly straight up
(0, 0, 1) it reported a mean normal of (-0.04, -0.01, 0.28), i.e. almost
horizontal, and 84% of pixels were more than 30 degrees off. That is the grain.
It got worse with quality because the residual scales with the window: at
quality 1 the normal was the noisiest, which is why quality 2 "looked better".

Fixed, in `LocalLightingCore.h` and the two ps_3_0 passes that mirror it:

- the tangent is the difference between the two taps, i.e. a real central
  difference, and slides continuously to a one-sided difference where a side is
  off-surface instead of switching;
- the window is sized against the light buffer's own texel (so a 0.75x buffer
  no longer shades pixels with a narrower normal than the pixel it describes),
  rounded to whole depth texels per side so the two taps mirror the receiver;
- every tap is sampled and reconstructed from the same saturated, snapped uv,
  so an off-frame tap shrinks the window to the frame and a point-sample's depth
  is never reconstructed along a texel it does not belong to;
- the edge test runs in inverse depth, where every plane is flat, with a
  partner-relative veto for a side that is a different surface entirely, and the
  depth map's clear value tells the weights which taps found a surface at all;
- the highlight is gated by how much of the window survived, and the composite
  blends a 1.5-texel tent with a continuity floor.

`NormalSpreadPercent` (F7 "LIGHT SMOOTHING", 25-400%) scales that window, for
players who want a sharper or a softer pool than the shipped 100%.

Measured, quality 1/0.75/0.5 on flat ground: mean normal error 0.00 degrees
(was 64-71), high-frequency content of the light 0.0002-0.002 (was 0.8-2.1),
and grain now falls as the buffer gets finer instead of rising. On undulating
ground at a grazing angle: 5.2-5.7 degrees (was 64). The composite's largest
per-pixel step near the camera is 0.009 of the local level (was 2.6) and a
silhouette base keeps its own light to within 15% (was 344%).
