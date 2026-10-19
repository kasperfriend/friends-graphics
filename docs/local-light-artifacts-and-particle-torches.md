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

The extra world-space smoothing floor has been removed. Local-light normals
now use the light-buffer texel footprint with a small, fixed distance-growth
term; there is no LIGHT SMOOTHING control. `Quality=2` (the new default) uses a
full-resolution light buffer, while lower F7 quality levels trade resolution
for performance.

Measured, quality 1/0.75/0.5 on flat ground: mean normal error 0.00 degrees
(was 64-71), high-frequency content of the light 0.0002-0.002 (was 0.8-2.1),
and grain now falls as the buffer gets finer instead of rising. On undulating
ground at a grazing angle: 5.2-5.7 degrees (was 64). The composite's largest
per-pixel step near the camera is 0.009 of the local level (was 2.6) and a
silhouette base keeps its own light to within 15% (was 344%).

## Character facets, resolution and ray tracing

A follow-up screenshot showed the local light exposing polygonal facets on a
large character. Quality 2 now runs the light/normal pass at full screen
resolution by default, reducing light-map upsampling. The pass still derives
normals from scene depth, however; it cannot see WoW's interpolated mesh vertex
normals, so flat-looking facets in the character's geometry can remain. Hiding
them with a broad normal blur was the same tradeoff that produced object
copies. A true smooth character response requires normals from the model draw
(or a character normal buffer), which this D3D9 post-process does not have.

The current effect is direct diffuse/specular lighting, not light bouncing.
This client uses Direct3D 9 shader model 3 and the renderer does not own scene
geometry or acceleration structures. It has no DXR/compute path for hardware
ray tracing; a real path tracer would require replacing the render backend and
integrating model geometry, not adding screen-space rays. No fake ray-tracing
pass was added.

## Dynamic light copies: the object beside the player, painted in the pool

2026-10-17: the player reported that near characters and objects the light pass
drew illusionary shape-copies of them, dark slabs floating beside the real
geometry. At the time, the F7 "LIGHT SMOOTHING" (`NormalSpreadPercent`) control
made them worse; the control was later removed after a zero-setting A/B test.
The pass has no temporal history, so this was never ghosting: it was the pool's
normal being steered by the object's own depth.

Root cause, from a CPU reconstruction over an eye-level scene with undulating
ground, a character-like capsule and a crate a few yards away: when the normal
window straddles an object's silhouette at a grazing angle, the window's own
slope step across the ground is comparable to the object's depth jump. The old
edge test compared the two-sided disagreement (which is exactly the jump - the
plane's slope cancels out of it) against the window's one-sided changes (which
are that slope), so the jump slid under the band and the object's tap kept
steering the tangent. Even when the band fired, the close was partial and the
outlier slide was scaled by the same drowned comparison, leaving about half of
the object's tap in the tangent. Half of a point many yards off the surface is
enough to paint the object into the pool. Everything the tests could see was
flat ground, where the one-sided changes are exactly zero and the bug cannot
occur - which is why the suite never caught it.

Fixed, in `LocalLightingCore.h` and the ps_3_0 surface pass that mirrors it
(pinned in `tools/LocalLightingRegression.cpp` by a scene that reproduces the
report):

- the window carries outer taps at twice the tangent distance. A per-side
  second difference over (outer, inner, receiver) is slope-independent and -
  unlike a three-tap window, where the slope step and the jump trade off
  exactly - names WHICH side of the window the step is on. The side is chosen
  by that signal alone: a rule keyed on the deviation gap guesses wrong the
  moment a jump is smaller than the slope step and pins the tangent onto the
  object, painting dark streaks out of it;
- the edge test fires at once instead of sliding its old [.30,.85] band open,
  and it fires against both scales the window knows - the yard noise floor, and
  the window's own one-sided changes, which are the terrain's own relief. The
  relief comparison is what keeps real ground (whose curvature lives at that
  scale) on the central difference;
- a tap that snapped onto the receiver's own texel - the frame clamp of an
  off-frame window - measures nothing and is marked like the sky, instead of
  reading as the flattest possible neighbour and taking over the axis at the
  frame edge.

Measured on the report scene, error against true normals on ground within a
band of every silhouette: 0.118 at the shipped smoothing (was 0.203 with the
old weighting) and 0.028 at 25% (was 0.040); at 400% every window near an
object is contested and the pool is soft and wrong in both versions - the pin
there is parity. Flat ground and the grazing far field are unchanged from the
shipped look; undulating ground at a grazing angle now measures 6.8-7.2 degrees
(was 5.2-5.7 with the old weighting, 64 before the reconstruction fixes), the
cost of dropping one side at genuine relief creases.

## Dynamic light copies under a tilted camera

2026-10-18: a follow-up report showed that nearby-character copies still grew
when the then-existing LIGHT SMOOTHING control was increased with the camera
pitched. The near-object gate had measured standing height along inverse-view
row 1 (camera-up), not world vertical. A forward/back tap displacement could
cancel a character's height, so the foreign tap survived when the wider window
reached it.

The gate now measures height along WoW world +Z in both the CPU core and the
surface shader. The pre-removal regression compared world-up with camera-up
under the former slider's 0%, 25%, 100% and 400% scales. Zero world spread
lowered modeled near-silhouette copy error to 0.008, compared with 0.022 at 25%
and 0.040 at 100%; the mandatory one-texel footprint remains. At 400%, the
camera-up version's error is 0.455 versus 0.093 with world-up; the fixed result
stays under the regression's 0.12 ceiling.

## Dynamic-light activation, selection and diagnostics

The direct local-light pool is now an independent volume-composite effect:
enabling `[DynamicLighting]` no longer silently depends on fog, clouds, glare or
another volume feature being active. Volumetric scattering still uses the active
global/local fog medium. Debug modes 1-4 also run with an empty selected
light set and are composited in isolation, before noise textures, atmosphere,
clouds or grading can hide the diagnostic. Mode 1 shows each source after
surface shading and attenuation but before the user/environment multipliers;
mode 2 shows the final direct contribution; mode 3 remains black because the
removed screen-space visibility pass intentionally has no duplicate-shadow
replacement; mode 4 shows the normal-window confidence. For local-light
volumetric scatter, use `AtmosphereDebugMode=13`, separate from the atmosphere's
own debug modes. The unused `[Atmosphere] LocalLightPercent` bright-pixel controls have been
removed; `[DynamicLighting]` is the supported path for selected world-space
sources.

The per-frame source scan now rejects distant manifest/native/attached sources
in squared-distance space and uses a partial top-four/top-eight sort rather
than sorting every candidate. Surface and portable pool shading reject lights
outside their finite radius before square roots, specular powers and shading.
Manifest IDs hash the full record (not position alone), so two intentionally
layered emitters at one position remain distinct. Persistent D3D lights stay
eligible while the device still reports them enabled; draw-scoped enables keep
the existing short grace window. Reaching the attached-torch cap skips only
additional emitters instead of aborting observation of the remaining batch.
Every 300 selection frames, `LocalLighting.log` now records the selected counts
by source type, active normal-spread percentage and each selected source's type,
ID, position, radius, intensity and score. This makes it possible to tell
whether nearby flame captures are competing as separate lights instead of
inferring from the total attached-emitter count alone.

## Pool membership: hysteresis and cross-fade (2026-10-19)

Four (quality 0) or eight (quality 1-2) emitters fit in the constant table, so
membership is a decision taken every frame. It used to be the raw score
ordering, which meant two lamps of similar strength traded places as the player
walked between them - one switching off and the other on in the same frame,
reported as flicker near a group of lights.

`renderer::local_light_detail::StablePool` (src/Lighting/LocalLightSelection.h)
now keeps incumbents: a challenger must beat the weakest incumbent by
`kDisplacementMargin` (30%) to take its slot, and at most one slot changes
hands per frame. `FadeToward` cross-fades every slot - in over `kFadeInMs`
(140 ms), out over `kFadeOutMs` (220 ms) - and a light that falls out of the
candidate set keeps its slot while it dissolves. `SelectionSignature` includes
the quantized effective intensity so the water pass's per-frame constant cache
notices a fade in progress, not only a changed id set.

`tools/CameraAnchoringRegression.cpp` pins the one-swap-per-frame rule, the
margin, the tie behaviour and the fade curve.

## **And it was not only the score: the pool itself was view-dependent.** A native
source is observed only through the draw calls that consume it, and the client
submits those only for emitters inside the frustum. Selection still required
re-observation inside a 1.4 s batch-cadence window, so turning away retired the
lamp after 1.4 s - its ground lighting going dark while that ground was still on
screen - and turning back needed six fresh frames before it lit again. That is
the reported asymmetry exactly: yaw sweeps emitters across the frustum edge far
more often than pitch does, and no change to scoring could reach a lamp that had
already left the pool.

**And every pass that draws a lamp has to agree about the pose.** The surface
lighting was given the `kCameraEyeHoldMs` hold in this round; the volumetric
march that draws each lamp's shaft and halo was still gated on a bare
`f.cameraValid`, so on a frame whose draw stream carried no fresh eye the ground
stayed lit while the glow around it vanished - a lamp that visibly broke. There
is now one definition, `FrameContext::PoseUsable()`, and both passes ask it.

**And a refusal now says so.** Every gate in that pass drops the whole light
pass, and the reasons look identical on screen - the lamps are simply not there.
`LocalLighting.log` therefore names the gate that fired, at most once a second,
with the numbers behind it:

```
lighting pass refused: camera pose not usable cameraValid=0 poseAgeMs=1400 depth=1 scene=1 selected=6
```

`selected=6` with a refusal is a gate; `selected=0` with no refusal is the pool
or the discovery, and the two need different fixes. Without this line the
distinction costs a reproduction.

A confirmed source (`stableFrames >= 6`) is now held across
`kNativeConfirmedHoldMs = 15000` of silence, while an unconfirmed one keeps the
old five-part test. The window is finite on purpose: a brazier that is put out
or a door that closes must not glow forever. A lamp now leaves the pool only
through slot reuse (a new generation and stable id), expiry of that hold, or the
distance cull - none of which a head turn can do.

The camera's heading is not a fact about a lamp (2026-10-19)

Two view-dependent gates were left in the pass, and both made the same report -
"dynamic lights break at some angles and flicker":

- **Selection scored the yaw.** A native emitter's score carried
  `facing = clamp(dot(delta, forward)/distance * .35 + .65, .15, 1)` - a factor of
  six between a lamp in front of the camera and one behind it. Turning the head
  re-ordered the four/eight slots, so a different set of lamps lit the same
  ground, and `StablePool`'s 30% margin turned the re-ordering into swaps and
  cross-fades rather than a clean picture. A reduced tie-break (`* .08 + .92`)
  was still visible in game - "breaks more on left-right rotation than up-down"
  is that term, and it is exactly the axis it is sensitive to - so the score now
  reads `intensity * radius^2 / distance^2` and nothing else. `cameraForward`
  stays in the signature (the volume march and the water stage share the pose)
  and is deliberately unread.
- **One radius behind the camera was a hard cut.** `forwardDistance < -radius`
  deleted a lamp from the candidate list the moment a yaw carried it across that
  line, so it left the pool and faded - and faded back in on the way past. That
  test is gone: the frustum already culls what is not drawn, and a lamp just off
  the edge of the screen still lights ground at its edge. A lamp behind the
  player is lighting ground the player can turn onto.
- **The pass refused un-recaptured frames.** `LocalLightingRenderer::Render`
  required `FrameContext::cameraValid`, which is published only by a draw that
  carries the eye. A frame with no terrain in view (looking at the sky or a wall)
  left the flag false while the retained inverse view and camera position were a
  frame or two old, and every lamp in the city blinked off. The retained pose is
  now used for `FrameContext::kCameraEyeHoldMs`, exactly like the underwater
  medium and the lamps' own observation gate, and refused past it.

`tools/LocalLightingRegression.cpp` pins all three: the two selection expressions
and the absence of the old cut, and the pose hold in the renderer source.
