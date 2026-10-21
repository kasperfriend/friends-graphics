# Water rendering audit

## Full rework round 21: the crossfade was measured against the mean plane (2026-10-20)

The report, after round 20: the underwater shader is secured against the exit rule
but still "disables mostly near surfaces" - a swimmer a yard or two under the
waterline watches the medium (fog, beams, net, grade) thin to almost nothing and
breathe back, over and over, wherever he looks.

**What the crossfade was reading.** The tracker's plane is the liquid tile's MEAN
lattice plane, and the ramp ran over depth above that mean: `Smoothstep(.22, 1.5,
yards)`. But the visible surface is the mean plane plus the geometry waves, and at
the shipped sliders a set's crest rides about a yard and a half over the mean
(`1.62 x amplitude x kGroupMax`, amplitude capped at `1.2 x cell`). A camera one
yard under the mean plane is therefore alternately two yards deep and at the
surface - and the shipped crossfade band spans exactly that range, so every swell
cycle swept the ramp from ~1 toward 0 while the diver watched the seabed. Still
water never showed it; any sea did. (The same lens explains why it read as "near
surfaces": two yards down, the reach can no longer reach the band.)

**The fix.** The hysteresis and the ramp are measured against the visible surface:
`Tracker::Advance` takes the caller's `waveBandYards` - the displaced trains' own
height at THIS camera (the field phases already carry the camera's projection, so
`SwellTheta(train, 0, 0, phase)` is the absolute cell under the swimmer), times the
grouping envelope's worst case so a set's top is covered, floored at zero (a trough
below the mean must not subtract water from a swimmer the mean plane holds) and
capped by `kMaxWaveBandYards`. WATER QUALITY 8 renormalizes its third displaced
train to the two-train peak, so the two-train evaluation is the peak at every
quality; the production caller in `WaterEffect.h` builds the same
`BuildGeometryWaves` the draws build and gives zero band when waves or geometry
waves are off or the camera is unknown - still water is band zero and behaves
exactly as shipped. Depth below the mean and the wave's reach now add up: the ramp
breathes with the sea but can no longer fall away while the depth alone would hold
it, and surfacing still fades the medium out because the depth term falls with the
swimmer.

Verification: UnderwaterRegression 209 (six new: the still-water identity half a
yard down, the swell hold at the same depth, ten seconds of swell cycle with the
ramp breathing but never falling away, air above the crests that the reach must not
hold, a NaN reach, and a trough that subtracts nothing). The pin was proven to bite
by reverting the band, which fails the swell fixture alone. The rest of the
portable battery is green, sanitizers included.


## Full rework round 20: the exit that caught the diver (2026-10-20)

The report: the underwater shader "gets lost periodically, any angle" - a diver
swimming at depth watches the medium (the fog, the beams, the caustic net, the
grade) drain away over a few seconds and pour back, over and over, whichever way
he faces.

**What the exit was reading.** Round 17 bought the remembered plane its exit: a
camera that travels from where a sheet last covered it, while known footprints in
view keep saying "none of them is over you", is on dry ground below the sheet's
level, and three quarters of a second plus eight yards of that retires the memory.
The evidence cannot distinguish that walker from a swimmer. A liquid body is drawn
chunk against the client's frustum, so a diver under open water receives exactly
the walker's testimony - local sheets, every footprint answering "not over YOU" -
because his own covering chunk is behind or above the view he is actually looking
at. The two states are identical in every number the tracker holds; the retirement
resolved the ambiguity toward the walker, and every swim paid for it. Unobserved
frames (the seabed filling the view) accrue the outside count, eight yards of
travel pass in moments, the plane retires, `cleared` answers air, and the ramp
drains to zero in about a fifth of a second. Looking forward or up again puts a
sheet containing the camera back in frame, the memory re-enters, and the medium
pours back - which is the report's "periodically".

**The fix.** The exit keeps its walker and loses the swimmer by counting the one
thing that differs between them, which is where the camera is looking. `Tracker::
Advance` now takes the caller's word on whether the covering region - the
remembered plane over the camera's own position, sampled again ten and twenty-five
yards along the view's horizontal forward - was inside the client's frustum this
frame. The test lives beside the tracker (`CoveringPlaneInView` in
`src/Water/UnderwaterMedium.h`), spends the capture's own projection scales
(P00/P11, the screen mapping the celestial tracker reads), keeps a quarter frame
of slack (the covering chunk only has to be eligible to draw, not centred), and is
unit-tested against level, forty-five-down, straight-down, shore and degenerate
views. On a frame that could not have seen the sheet over the camera, "no sheet
over the camera" is a fact about the view: the outside count does not accrue, and
it restarts, so a long swim cannot earn the exit one glance at a time. The
production caller in `WaterEffect.h` spends the frame's own pose - this frame's
capture, or the retained pose inside the same short hold every consumer keeps -
and hands over no evidence whenever the pose or the projection cannot be tested:
refusing the test retires nothing, while passing it wrongly retires a diver. The
walker is untouched: his view of the shore puts the sheet's region in frame, which
is what "still looking at it" always meant.

Verification: UnderwaterRegression 202 (thirteen new: the diver who swims fifty
yards at depth with no surface in frame and holds the medium throughout, the same
walk with the covering region in view still retiring, the glance-and-look-away
sequence that must never accrue against the sustained look that must, and the
covering-region test's own views). The swimmer pin was proven to bite by reverting
the gate, which fails that fixture alone. The rest of the portable battery is
green, sanitizers included.


## Full rework round 19: the ticks on the shallow bed (2026-10-19)

The report: dark ticks in rows on the submerged sand in the shallows, gone the moment
foam is switched off. The second half of that sentence is the diagnostic - the foam
switch zeroes both foam strengths together - so the hunt stays inside the foam terms,
and of those only one is built from a screen-space derivative: the bed slope behind the
shoal term and the beach/wall split. The ticks in the screenshot measure two pixels
wide in every depth band, near shore and far, which is the size of a derivative quad
and of nothing else in the foam chain: the lace, the broad warp and the chop are all
world-anchored and yards across, and round 16's anchored fleck fades out well above
that scale.

**What the slope was reading.** `bedSlope` came from `cross(ddx(bottomView),
ddy(bottomView))` - the reconstructed bed point differenced against its immediate
neighbours. For a plane in exact arithmetic that ratio is the plane's true slope at any
resolution, which is how it survived review; but the depth it reconstructs from is
*stored*, and stored depth has a step. Modeled end to end (a 0.28-slope bed under a
pitched camera, the client's projection, point-sampled capture, 2x2-quad derivatives,
the shader's own thresholds): against a true 24-bit fixed depth buffer the reading is
clean to three decimals, and against a short-mantissa floating-point depth - what an
INTZ fetch hands back on AMD hardware, where INTZ is DF24 - the storage step at a few
tens of yards is a sizeable fraction of the bed's rise between neighbouring pixels, and
the slope comes back as quad-scale jitter, an order of magnitude above the bed's own
variation. Every shallow bed whose true slope sits near the shoal threshold
(`bedSlope*2-.3` - ordinary beach aprons and sand bars live exactly there) then
switches its shoal foam a quad at a time along its depth contours, with the beach/wall
split and the swash width jittering behind it. On fixed-24 hardware there was never
anything to see, which is why it presented as a machine-specific artifact and why the
CPU preview - whose `FieldSlope` has always measured the authored bed over +-1.5 yards
- could not reproduce it: the mirror and the shader were reading different numbers.
("Shore understanding" below had even named this failure and its cure, a span named
`kBedSlopeSpanYards`, for an ancestor of the slope code; the constant was never in the
shader. It is now.)

**The fix.** The bed normal is built from bed points reconstructed at taps a fixed
world distance out: `kBedSlopeSpanYards` (1.5 yards, `FieldSlope`'s own baseline)
either side of the pixel along its screen axes, marched along the derivatives of
`screenUV` (this pixel's own texel steps, smooth whatever the depth does), the texel
distance derived from the pixel's footprint in yards grown by the bed's extra distance
along the ray and capped at sixteen texels - past which one texel is already most of
the span wide. A tap that lands on sky falls back to the centre point, turning that
axis into a one-sided reading instead of a cliff invented out of the far sentinel. The
model above has the jitter collapse by more than an order of magnitude at every storage
precision while the reading stays on the true slope, and that round trip is a regression
fixture now, not a paragraph. Four extra depth taps in the surface stage are the whole
cost.

**One gate that was simply missing.** The backwash term never carried
`foamDepthValid`. Where the captured depth behind a water pixel is sky - open sea
running to the horizon, no bed in the capture at all - the foam depth clamps to zero,
the swash mask reads ankle-deep, and at quality 7 and above the backwash painted
streaks across deep water whenever the sheet was draining. Shore and shoal always had
the gate; backwash has it now, in the shader and in the mirror. The preview's coast
frame moves in 29 of its 139,200 pixels (the waterline fringe where the clamped depth
was lying) and the sea frame not at all, which is the size of the look change over any
bed that is really there: none.

Verification: WaterRegression 5636 (three new - the captured-depth round trip above,
asserting the one-pixel stencil's jitter, the span's suppression of it, and the span's
accuracy - plus the source pins); the underwater and shader-source suites unchanged and
green.

## Full rework round 18: the optimization and bug sweep (2026-10-19)

A sweep rather than a report: profile what round 17 left on the hot path, hunt for the
bugs a careful read and a sanitizer can find, and fix what is real. No pixel changes -
the round's visual claim is the opposite, proved rather than asserted.

**The coverage scan was the new hot path.** Round 17 put a per-triangle barycentric scan
in `PlaneOverCamera`, which runs on *every* liquid draw of *every* frame. Benchmarked
portably (identity basis, the shapes production feeds it), a chunk's 128 triangles cost
3.4 µs with the camera over it, 5.0 µs two yards ashore, 5.2 µs from 300 yards away -
and a cap-sized 1296-triangle sheet 30-50 µs. Flying over a lake draws dozens of chunks
the camera is not in, so the frame was spending a visible slice of itself refining
distances for sheets that could only ever answer "not yours". Three cuts, each beneath
the decisions rather than through them: the reader now measures the coverage vertices'
exact local extremes once as it copies a draw in (it already validates every position
finite there) and hands them over as `LiquidCoverage::boundsKnown`, so the reading
skips its own validation pass; a sheet whose whole bounding box is past the locality
reach the tail of the function judges by (with a hair of guard for the float the answer
is reported in) is refused from the box alone - not local, not containing, the distance
reported being the box's lower bound and the height the measured ends' mean, two
numbers only the log spends on a sheet that observes and classifies nothing; and inside
the scan a triangle whose own box is no nearer than the best already found is retired
in six compares before any division. After: 1.1 µs over the chunk, 1.3 µs ashore, 0.08
µs from afar; the big sheet 7.1 / 12.2 / 0.08 µs. A differential fuzz against the
pre-sweep header - 400,000 randomized sheets (grids with dry holes, soups, strips,
fans) under identity, yawed, scaled and mirrored bases, bounded and not - found **zero**
divergent decisions, identical numbers wherever a reading is local or containing, and
the far distance a true lower bound everywhere else.

**A smaller per-draw cost went with it.** The liquid read re-parsed the bound vertex
declaration's element array on every draw to rediscover where POSITION lives; liquid
draws share one declaration, so the parse is now remembered against the declaration
itself (held like the cached buffers beside it, dropped in the same reset). Where the
position lives is a fact about the declaration, not about the draw, and it was the only
part of the parse any consumer kept.

**Bugs found and fixed.** `LiquidElementCount` multiplied in UINT: past ~1.4e9
primitives `3*count` wraps, and a count wrapped small would have had the coverage read
swear the first few emitted vertices were the whole draw, footprint and all. It now
counts in 64-bit under a ceiling no liquid draw approaches, past which the read samples
the first vertex only - the same fail-safe as an unknown primitive type. The tracker's
idea of "have a camera" checked only the height's finiteness while the jump test and
the remembered plane's anchor spend x and y: a NaN in either would have sailed through
every comparison and anchored the plane somewhere it could never expire from; all three
axes must now be finite. Both suites also ran clean under ASan/UBSan with
`-fno-sanitize-recover=all` before and after - no UB was hiding in the portable core.
The two visual candidates a sweep would want next remain exactly as blocked as the
TODO records: world-anchoring the sediment patches needs shader slots the 508/512
stage does not have, and fading the client's own underwater fog needs a client to
answer which fog that is. Neither is guessable from here, and neither was guessed.

### Verification

`tools/UnderwaterRegression.cpp` is **189 assertions**: bounded coverage answers
bit-for-bit as unbounded (contained, dry-corner distance, yards, span), a sheet past
locality reach is refused from its box with the box's distance (300 yards short on
each axis reads 424.264, bounded or not), and a NaN-horizontal camera anchors no plane.
`tools/WaterRegression.cpp` is **5633**, pinning the bounds hand-off, the 64-bit ceiling
and the declaration cache in the Windows-only reader by source. The differential fuzz
and benchmarks above were run against a verbatim copy of the pre-sweep header. No HLSL
changed; the fxc counts stand at rounds 16-17's (underwater 508/512, surface 1042).

## Full rework round 17: memory may not start what the gate refused (2026-10-19)

One report from the round-15/16 build: the underwater shader was on while standing on ground
near water in Sholazar basin basin. Round 14 was supposed to have closed exactly this - only being
under the sheet may start a submersion - and its direct gate does hold. What it left behind was
the memory underneath the gate, and the sequence is short enough to state as a walk: stand
**above** a nearby lake and its reading quite properly observes "air" (testimony of air was
never gated) - and was *remembered*, as every accepted local plane was. Walk downhill
**beside** that lake, into the basin below its level. Every direct reading is now positive and
is footprint-refused, frame after frame - but those frames are therefore *unobserved*, and an
unobserved frame re-measures the camera against the remembered height, which says three yards
of water overhead. The medium starts from memory; the old loose sustain ("nearness may sustain
an established swim") then accepts the beside-the-lake readings at face value and keeps it
forever. Reproduced against the core in thirty lines: air above the bank, then the refused
reading below it, and the tracker settles at `ramp 1, submerged` on dry ground. The direct
reading was never the problem again; the problem was that three other paths spent weaker
evidence than the gate demanded. All three are closed, everywhere the answer is spent:

**Memory has provenance, and memory may not start.** Only a sheet actually over the camera
(`MayStartSubmersion` on a positive reading) may create or renew the remembered plane; air
testimony and beside-the-water heights create none. A remembered plane may finish an overhead
entry for one second (the smoothing crosses its threshold over frames in which the sheet can
leave the frustum) and may sustain a swimmer already in, but a dry camera is answered "air"
from memory however far below some remembered lake's level it has walked, and a dry tracker's
stale plane is dropped outright. Sustaining changed sides with it: a positive reading that is
not overhead no longer observes at all - the remembered plane does that job - and a known
footprint that keeps saying "outside" while the camera travels away from where a sheet last
covered it retires the memory, which is the exit for climbing out onto ground below a
neighbouring sheet's level (outside evidence alone retires nobody: a visible neighbour not
containing a diver proves nothing about his own culled chunk overhead). The jump reset
still lands first, and a teleport *into* water remembers its landing frame's own overhead
plane, pinned as its own case.

**A rectangle is not a sheet.** The footprint itself was the next weakest evidence: first and
last consumed vertices bound a chunk's rectangle, but a liquid chunk enables its tiles one by
one, so that rectangle routinely covers dry shore cells and the inside of a bend. The caller
now reads the draw's emitted vertices in order - one range lock per buffer inside the existing
one-new-chunk-per-frame budget, cached with the draw, capped at 4096 elements and a megabyte
of vertex span - and `PlaneOverCamera` measures containment and distance against the actual
triangles (0.75 yards of seam slack, not a shoreline's three), taking the height from the
nearest triangle at the camera's own point instead of the mean of a river's two ends. The
rectangle remains the fallback when a draw could not be read whole, and the no-footprint entry
fallback shrinks from 24 yards to the 4 yards that mean "measured water at the camera" rather
than "water near a player on the ground".

**The surface lane spends the same rule.** The tracker was only one consumer: the water stage
classified its own draw with any local positive yards, so a sheet beside a dry camera shaded
itself as seen from under water even while the full-screen gate stayed shut. Both now spend
`ClassifiedYards`/`MayStartSubmersion` - one entry rule, in the core, for the medium, the
memory and the lane. The status line gains the outside count and `cover=`/`contains=` so the
next report from any shore says which evidence it ran on.

### Verification

`tools/UnderwaterRegression.cpp` is **186 assertions**: the walk-downhill-beside-the-lake
sequence stays dry end to end, an L-shaped emitted sheet refuses the dry corner inside its own
rectangle while a strip spelling of the same triangles contains, a tilted sheet answers by its
triangle's height at the camera, a neighbouring sheet no longer observes and its outside
evidence retires the memory, the one-point fallback stops at a shoreline's scale, a teleport
into water remembers its landing plane, and the three halves of the gate are pinned against
each other in source. `tools/WaterRegression.cpp` stays **5632** with its classification pin
moved to the shared helper. No HLSL changed - the lane's *value* is chosen on the CPU - so the
fxc counts stand where round 16 left them.

## Full rework round 15: the login screen, the plane itself, and the orange underside (2026-10-19)

Three reports from the round-14 build: starting the game could leave a white login window with
only the audio running - sometimes cured by a restart; looking up from under water still showed
far terrain through the plane in water that should be too dirty for it; and a lake in Mulgore that
is blue on its surface was orange on its underside.

**No optional effect may prepare itself inside the first frame of a launch.** The underwater
pass's `Prewarm` ran from the compositor's `BeforeClear` hook - the first clear of the first
frame - and `EnsureShader` was synchronous end to end: on a cold bytecode cache (which is every
new build's first launch, because the key is the shader source) that first frame paid a full fxc
run plus the driver's own JIT before the client had drawn anything at all, and a restart found
the cache the abandoned launch had written and sailed through. Whether that is the whole of the
white screen only a stuck launch's log can prove; that nothing may stand there is not in
question, so the cost is simply gone from the frame path. The bytecode half is now
`shaderbytecode::GetBytecode` (load-or-compile, no device), run on a worker from `Configure`
and published under one mutex with its error and its elapsed time; `EnsureShader` consumes the
blob and owes only `CreatePixelShader`, and even that waits in `Prewarm` until 180 frames have
been presented - a few seconds of login screen the player can see - while a player who dives
sooner gets it from `Render` out of bytecode the worker had the whole login to finish. A diver
who beats the worker outright keeps the client's own fog for those frames instead of paying fxc
mid-dive; a blob the driver refuses is recompiled once, on the worker again; and the pass logs
`bytecode ready in N ms (worker thread…)`, so the next cold launch says what preparation cost
and that no frame waited for it.

**The plane and the gaps in the plane answer the same question now.** Round 13 closed Snell's
window and added the turbidity wash - in the full-screen pass, which deliberately returns the
liquid's own pixels untouched. The water stage marched its own pixels over the plain
camera-to-surface segment at every angle, raised the Fresnel at total internal reflection, and
then let the sheet's native alpha leak most of the live scene through anyway, because alpha -
not Fresnel - is what a blended sheet transmits by. So the far shore stood crisp *through the
plane* while the same shore was correctly impossible everywhere around it. The surface's
underwater column is now `renderer::underwater::InterfaceYards`: inside the surface's own Snell
band it is the camera segment with `ExitWash` on it, outside it is the far column; coverage
floors at total internal reflection and otherwise at what that column extinguishes. The wash
itself also grew the piece round 13 was missing: an exiting *image* crosses a near-surface
layer of silt and rough interface that scrambles direction faster than it removes light, charged
as **4 equivalent yards per unit of turbidity** (`kInterfaceScatterYards`) folded into the wash
multiplier on the CPU. At turbidity 0 the wash is exactly 1 and the crystal pane - and the
bottom path's exact-agreement pin - are untouched; in a swept swamp (turbidity 2.1-2.4 against
a short clarity e-fold) the overhead shore dissolves into the glow from a couple of yards down,
on liquid and non-liquid pixels alike. The surface receives the wash through the light colour's
unread `w` lane, folded per draw from the submersion that draw measured, the player's turbidity
dial as published by the compositor's reload, and the zone multiplier behind the same
`ZoneWaves` gate as clarity - one dirt, spent by both stages.

**An underside reflects the water it is in, not the sky it is under.** Mulgore needed no new
colours: three terms were handing its underside the air's palette on every zone alike. The
reflection miss fell back to the sky term, which from below (`rayUp` saturated at zero) is the
bare fog colour - orange on a warm evening. An SSR hit arrived in the client's dry, in-air
shading out of the pre-water copy, because these pixels are exactly the ones the medium pass
leaves alone: the bed in its own orange dirt, mirrored over blue water. And the sheet fogged
into `fogColor` at range while the camera-segment attenuation was spent on the body before the
reflection and foam were even added. The miss now reflects the body's own asymptote
(`(deep + inScatter) * tint`), a hit is marched over the reflected leg it travelled, the camera
segment is charged once to the finished pixel, and the fog target under water is that same
asymptote. None of it is per-zone: blue water is blue from both sides, a brown swamp is brown
from both sides, and Mulgore keeps the clear blue round 14 authored for it.

### Verification

`tools/UnderwaterRegression.cpp` is **177 assertions** - the wash as numbers (exact at 0,
per-yard-overhead at the interface, far column outside the window, a shallow dirty sky already
washed at two yards) and the startup contract as source pins (worker preparation, no
synchronous compile path left in the pass, frame-gated device creation) - and
`tools/WaterRegression.cpp` is **5182**, pinning the interface column, the medium-coloured
underside, the reflected leg, the moved camera charge, the coverage floor and the fog target
as one statement. CloudClimate 92, ShaderSource 36, AO 11, LocalLighting 13, the zone audit
and the CPU preview are green as before. The fullscreen stage's only change is which constant
lane its wash reads, and the Windows fxc leg holds it at **508/512 slots**; the surface stage's
new underside work lands at **1011 slots** (vertex 188), with Win32 Release, Portable
regressions and the changelog check all green.

## Full rework round 14: under the sheet, in the water's own colour (2026-10-19)

Three asks from the shore of the round-13 build: standing outside the water, with water nearby,
and looking up started the underwater shader; give every water-bearing zone underwater defaults
of its own, according to the place; and make the medium the colour the water advertises - red
water, red underwater.

**Nearness may sustain a swim; only being under the sheet may start one.** Round 12's locality
was one-dimensional: distance to the segment between the draw's first and last consumed vertices,
against that segment's span plus a shore's slack (or a 450-yard fallback with one vertex). That
answers *which lake is this height about?* - it is not evidence the camera is in the lake. Below a
raised sheet, on the shore under a canal wall or a dam, looking up puts exactly such a local,
positive reading in frame forever. `PlaneOverCamera` now also puts the camera in the draw's own
in-plane coordinates - one projection per axis, which the already-refused-otherwise orthogonal
basis makes exact - and reports how far outside the sampled chunk's rectangle it lies
(`footprintYards`, `containsCamera`, with three yards of chunk-seam tolerance; a footprint is only
claimed when both sampled extents are real). The tracker spends that stronger fact in one place:
a positive reading may *start* a submersion only when the sheet is over the camera, or, with no
footprint to ask, when measured water is within 24 yards. Sustaining a swim keeps the loose
locality (the chunk containing a diver looking at the bed is routinely the culled one), testimony
of air is never gated, and the status line prints `foot=` beside `yards=`/`horiz=` so the next
report says which fact it ran on. One boundary is named rather than papered over: a dry pocket
*under* a sheet that genuinely covers the camera - a grotto roof, a hull - is indistinguishable
from being in the water by anything a liquid draw reports, and still reads as in it.

**The medium inherits the water's colour, not just its depths.** `MediumFor` used to carry
DeepColour through untouched and let WaterColour (the tint) weigh in at a few percent, while the
sky overhead was lifted by a fixed blue - so a place could be red on the surface and merely dark
below it. WaterColour is a multiplier, so its hue is the *proportion* between its channels: the
medium now normalises the tint by its own luminance and folds that mean-one hue into the deep
colour (DeepColour still authors how dark the column ends; WaterColour now authors what colour it
ends as), and `SkyGlowColour` lifts by `tint * .18` instead of the fixed blue. A neutral tint is
an exact identity - the shipped global water and the bottom path's exact-agreement pins are
untouched - and a one-channel tint saturates at a clamp instead of becoming a laser. The third
leg is `AbsorptionColour`: it is a per-channel death rate, so the hue that *survives* a deep
column is the one it kills slowest; the sweep below authors it accordingly (blue dies in a swamp,
green and blue die in blood-red water) where the older values killed red first everywhere.

**A sweep of the world's water.** The environment table gains five zone dials -
`WaterClarityMultiplier` (0.1-4) and `UnderwaterTurbidity/Caustic/Shaft/GlowMultiplier`
(0-4 / 0-2 / 0-2 / 0-2), all defaulting to 1, resolved Map -> Zone -> Area and blended across
borders like every other environment float, and gated by the same `ZoneWaves` switch as the zone
sea state. Clarity is spent once, on the surface's `bodyStyle.x` and the medium together (one
water, one clarity); the other four reach the pass per frame from the settled environment and
scale a copy of the player's [Underwater] dials inside the core's own bounds, so a player's 0
stays 0. Against that schema, **199 of the 414 root zones** (plus Booty Bay's area override) now
author a complete colour set and underwater tuple, classified by the water they actually hold:
snowmelt and frozen seas run clear with low silt; Stranglethorn, Booty Bay and the tropical isles
run clearest with the strongest net and beams; Swamp of Sorrows, Dustwallow and Zangarmarsh are
soup (clarity 0.3-0.5, turbidity 2.1-2.7, the net nearly gone); the Plaguelands, Felwood and
Shadowmoon are murk in their own sickly hues; Bloodmyst is red and turbid; Moonglade and Hyjal
glow; canals, sewers and the Undercity slime run darkest; cave water gets almost no net, beam or
glow because there is no sky over it; and Loch Modan is authored as what it is - a clear, dammed
reservoir. Zones that already had hand-authored colours keep their Water/Deep pair; lava-only and
dry roots author nothing and stay on the global water. `docs/water-zone-profile-audit.md` (and its
generator, which now refuses partial colour sets, partial tuples and out-of-range dials) carries
the full table; as ever, addressable in the client table is not proof a root is playable.

### Verification

`tools/UnderwaterRegression.cpp` is 174 assertions - rectangle-vs-diagonal footprint fixtures,
the dry-shore/overhead/sustain/air tracker cases, red and fel hue folds, exposure invariance and
the tint-lifted sky - and `tools/CloudClimateRegression.cpp` is 92, pinning the sweep's breadth
(199 complete sets, never colours without dials), spot values a reader can check against the
world, Booty Bay's area override, border blending and transactional range refusals.
`WaterRegression.cpp` stays at 5181, ShaderSource 36, AO 11, LocalLighting 13, overlay and zone
audit green. The only HLSL touched is the sky lift (`waterTint.rgb*.18` for the fixed blue), a
variable multiply where a constant stood; fxc on the Windows leg lands the underwater shader at
**508/512 slots** (its allocator is not monotone - replacing the constant freed three), with
Win32 Release, Portable regressions and the changelog check all green.

## Full rework round 13: the lattice vertex under your feet, and the sky's picture (2026-10-18)

Two live reports from the round-12 build, both visible looking down at the bed: "one big 2x2
grid-like pattern, crossing in the middle" - also suspected looking up at the sky - and a request:
in dirty water the sky should NOT be clearly visible through the surface.

**The grid was two artifacts of the same family.** Reproduced without a GPU by rendering the
core's fields to images: the dirt patch field over an 80-yard window is literally *four flat
wedges meeting at a point*. `shadedWorld` is camera-relative, so the patch lattice (one cell per
100 yards) keeps a vertex at the point directly under the camera forever, and the four cells
meeting there print as a giant pinned cross that follows the swimmer - looking at the bed, looking
at the sky, anywhere. The fix is one constant: the patch sample is offset by a non-integer number
of patch cells (`kDirtSampleOffset = {37.31, 57.17}`), which parks the nearest vertex ~50 yards
away and leaves a slow gradient underfoot. The patch field is still camera-relative (round 12's
known debt), but it no longer carries a surveying diagram around.

Beneath that, the same images showed the caustic net cut by hard diagonal seams on both lattice
axes, in every fine-octave spelling. The shared value noise wraps the cell INDEX into [0,64)
before hashing but hashes cell 63's far corner under the id 64 while the cell that wrapped to 0
hashes the same corner as 0 - so the field is discontinuous at every wrap, every 160 yards (80 for
the doubled octave). On the surface, where the noise bends normals, that line has never been
readable; under a sharpened net at five times gain it rules straight lines across the bed. The
net now reads a corner-wrapped variant (`NoiseValue64` / `causticNoise`) that is exactly
64-periodic in both axes - which is what the camera phase's own wrap assumed all along. The cost
in ps_3_0 slots is paid by restructuring the shaft march: the light's entry point is affine along
the march, so its cell is a lerp of the two ends' cells instead of a third entry solve per tap.
Warp and dirt keep the plain noise (their seams sit 1280 cells and 6400 yards apart,
respectively); the divisor was remeasured over the periodic lattice (.195948, 5.1034).

**The sky's picture vs the sky's light.** Inside Snell's window the pass showed the client's sky
and shore through the column over the diver as though through a glass pane, because that is how
round 8 modelled it. In real water the *image* dies faster than the *energy*: scattering scrambles
the incoming sky light's direction as well as dimming it, so past a turbid column what survives is
the downwelling glow, not readable clouds. Exiting rays now pay an extra wash of the water
overhead - the column times `(1 + turbidity)` - while the in-scatter and surface-glow terms still
pay out the full light, which is the physical trade: the dirtier the water, the sooner the window
turns from a view into a lamp. At TURBIDITY 0 the wash is exactly 1 and nothing changes (the
bottom path's exact-agreement pin holds on purpose); rays that end under water are untouched.

### Verification

`tools/UnderwaterRegression.cpp` is 159 assertions: noise continuity proven by straddling the
wrap (largest step there bounded against the field's own interior slope), the dirt offset pinned
mid-cell with the murk around the camera's own point bounded to a slow gradient, the wash measured
at turbidity 3 vs 0 and pinned OFF at 0, and re-measured net normalization. The fields themselves
were rendered to images before and after: wedge-cross and diamond seams in, seamless net and a
directionless murk gradient out. `WaterRegression.cpp` stays at 5181.

## Full rework round 12: whose water is it, and which camera is it for (2026-10-18)

Three live reports, between them they touch every layer of the underwater path: a teleport to
Mulgore put the underwater medium on dry land and it survived a restart; under water, the caustic
net moved with the camera - turning or swimming dragged it across the bed; and in Loch Modan the
medium never engaged at all.

**A height is not a place.** The plane reading answered one question - how far is this draw's water
above the camera - and the tracker kept the nearest such plane as the camera's interface. But a
plateau lake drawn anywhere in view reads the same yards as the lake the camera is actually
floating in, so a player who teleported onto dry land under raised water kept *observing* the lake
every frame, and the teleport reset that should have forgotten the old state only ran on frames
that observed nothing: it never fired. Four guards replace the one coincidence:

* The reading now measures **sideways as well as up.** The caller reads the draw's first AND last
  consumed vertices (the hint carries the primitive so the last one can be located), and
  `PlaneOverCamera` reports the camera's horizontal distance to the segment between them and that
  segment's span. A reading only counts (`local`) when the camera is within about the span of the
  water that measured it - the gate scales with the water body itself instead of with a guessed
  yardage. Tilted sheets answer by the mean of their ends. Non-local readings observe nothing and
  classify nothing, and the status line counts them apart (`uwPlane` gains a seventh field, plus
  `horiz=`): on the offending client the log should show the plateau lake being refused by name.
* The camera-jump reset now runs **every frame**, not only on unobserved ones, and the remembered
  plane is **forgotten after 450 horizontal yards** from where it was observed - its height stops
  answering for the world once the camera has walked away from the water that gave it. A deliberate
  clear also latches (`cleared`), because otherwise the frame after a forget quietly re-holds the
  last smoothed yards forever, which is the exact false-positive all this exists to prevent.
* The vertex read no longer refuses dynamic default-pool buffers outright. That refusal was
  first-dive insurance, but wherever a client's liquid chunks are (flagged) dynamic it made every
  reading answer from the draw origin's plane forever - one way a whole zone's water never engages
  (the Loch Modan report). The one-locking-chunk-per-frame budget is the safety; the pool never
  was.

**The field belongs to the camera that consumes it.** The wave field's phases carry an anchor
camera's own place in the lattice, and the full-screen pass reconstructs bed positions relative to
the frame context's camera, so relative-plus-phase is the absolute cell ONLY when the two are the
same camera. They never quite were: the field was published by the last water draw (anchored to
the draw), or rebuilt at a frame's `Present` for the frame that was ENDING and then consumed by
the next frame's composite - which runs *before* that `Present`. Either way the net, the shafts
and the silt were shaded against a camera a frame's worth of swimming away, and crawled by exactly
the difference. There is now exactly one field construction, at composite time
(`watereffect::UnderwaterFieldAt`), from the same integrated motion clock every draw used, handed
the frame context it will be consumed against.

That left the net's fine octave, which crawled for a subtler reason: its sample was
`2.17*cell + 1.31*counterPhase`, inherited from the surface's fine ripple, and under that spelling
the octave moves at one rate with the cell and another with the phase, so it shears across the
base octave as the camera moves. A surface grain may shear under a drifting sea and nobody sees it;
a caustic net is nailed to the bed or it is a decal. The fine octave now applies ONE map -
crossed-and-doubled - to `cell + phase` together, so it reads the chop's own second harmonic of
the absolute cell. The map's entries are integers on purpose: the phase wraps at the lattice's
64-cell period, and only an integer map takes that lattice to itself, so the net does not pop at
each wrap the swimmer crosses. Measured again: the net's mean moves from .197391 to .195857, and
the divisor with it (5.0661 -> 5.1058), which is the whole of the visible difference - the pattern
stops moving with the camera and starts lying still on the bed.

The sediment patches had the family fault in its simplest form: their noise sampled the
camera-relative position at a fortieth scale with no camera phase at all, so the patches travelled
with the swimmer. "Both are properties of the place, not of the camera," the comment said. The
anchored form - the broad warp read at the bed point - was built, then unbuilt under protest: fxc
measures the full-anchored shader at 522 of its 512 ps_3_0 slots (the warp coordinate costs five,
and a rotated fine octave five more: the net's fine octave is an un-rotated exact doubling for the
same reason), and a phase of the patch field's own would need a constant lane the stage does not
have (`warpConstants.w` is the only free lane, and a phase takes two). fxc's budget arithmetic is
also non-monotonic here - three spellings of the same shader measured identical totals - so the
round took the slot it could prove: dirt stays the one camera-relative sample in the pass, the
patches breathe slowly with the swimmer, and TODO.md carries the item against a budget or ABI
opening. Anchoring is no longer a comment for the rest: `TestWorldAnchoring` puts two cameras
fifty-seven yards apart through the real field builder and Requires the net and the warp at a fixed
bed point to be the same numbers from both - and a stale field to be different, which is the bug,
quantified.

### Verification

`tools/UnderwaterRegression.cpp` grew to 153 assertions: the reading's locality geometry (a lake 700
yards away at swimming height measures truly and is not local; a swimmer mid-lake is distance
zero; the lone-vertex fallback radius both ways), a teleport that lands under somebody else's
lake and never re-observes it, the remembered plane's expiry under walking, the anchoring
identities above, and pins that the composite builds the field for its own frame context while
no draw and no `Present` publishes one. `WaterRegression.cpp` stays at 5181. Live confirmation
remains the client's job - Mulgore after a teleport, a Loch Modan dive, and a slow turn over a
sunlit bed - with the new `horiz=`/seventh-field counters on the water status line to read.

## Full rework round 11: the second leg of the bed's path (2026-10-18)

With the horizon opaque, the same player pointed at what was left in that direction: a submerged
hillside standing dark and crisp *through* the mid-distance water, clearly terrain behind glass
rather than terrain under a body of water. Asked for "some underwater fog visible from above,"
with a slider and a toggle.

The view down into the water had a one-legged integral. The seabed in the captured scene was lit
by the client **as though in air** - the client's terrain shading knows nothing about the water
over it - and this stage then charged the light's trip back *up* from the bed to the camera (the
Beer-Lambert column of round 6). The sunlight's trip *down* to the bed was never charged by
anyone, so a bed ten yards down was being shaded and then extinguished as though the sun had
reached it for free: roughly twice as present as the water over it allows, and dark rather than
dissolved, because one leg's extinction on an already-dark bed mostly subtracts while the body's
own colour (what the second leg's worth of absorption hands the pixel to) is what makes it read
as *depth*.

`DEPTH FOG` closes it. The captured-depth side of the optical column is multiplied by an authored
factor carried in `reflectionControl.w` (a lane uploaded unread since the depth range moved into
c203 on the CPU): 100% is the one-way column this stage always marched, 200% (shipped) charges
both legs, and up to 400% is simply murk a player may ask a lake for. It multiplies *yards*, not
colour, so ankle-deep water at a shoreline stays clear at any setting and only depth earns the
fog; the submerged branch of the column (the camera-to-surface segment, which the medium marches
in full already) is untouched; foam, shore masks and the soft contact read their own depths and
do not move. The toggle off uploads exactly `1.0f`, which makes the column bit-identical to the
pre-round-11 shader. Plumbing is the standard water path end to end: `[Water] DepthFogEnabled` /
`DepthFogPercent` (per-area like the rest), `DEPTH FOG` and `DEPTH FOG AMOUNT` in F7 next to
`WATER DEPTH`, and the CPU preview charges the same factor on its own bed (which it also shades
in open air, for the same reason), so a published preview frame is still a picture of the game.

### Verification

`tools/WaterRegression.cpp` is at 5181 assertions (the column line, the unlock-lane upload, the
settings reads at matching bounds, the ini/overlay defaults, the mirror's factor);
`tools/UnderwaterRegression.cpp` unchanged at 140. The Win32 build and the look are CI's and the
client's; the expected change is that mid-depth terrain like the Stranglethorn Vale screenshot's
dissolves into the body of the water within a few fathoms instead of standing under it, with
shallows and shores unchanged, and `DEPTH FOG AMOUNT 100` (or the toggle off) restoring the
previous picture exactly.

## Full rework round 10: a sheet is only a sheet while a pixel resolves it (2026-10-18)

The first live report on the working medium: underwater is good. Two defects rode in with the
rounds that made it work, and both are fixed here.

**The glass horizon.** From above the surface, distant water was transparent - the sky and the
land behind the sea showed through the far water, with the horizon line visible *through* it.
Round 8's far-sheet discard is the cause, and the mechanism is sampling: the discard is a
per-fragment geometric fact (this triangle is the far side of its wave), sound while a pixel
resolves a wave into many fragments. At distance and grazing angles a pixel spans whole triangles
of the ~4-yard liquid grid and whole wave faces; which sheet covers the pixel centre is then
luck, and a pixel whose only fragment is a far slope is discarded into a hole showing whatever
was behind the sea. Near the horizon that is most of the far water. The discard now fades out
with the pixel footprint (`1 - smoothstep(1.2, 3.6, max(footprint))` lattice cells, the wave
being 16 and 11.5 cells long) - the same ground on which this stage already fades every
unresolved octave. Where it fades, the two sheets shade within a fog colour of each other and
the column's coverage is opaque anyway, so keeping both sheets costs nothing visible; where
waves are resolved the factor is exactly one and the near sea is bit-identical.

**The first dive that stopped the render.** Going under for the first time in the session froze
rendering (the game kept running); a restart - warm caches - never saw it again. Two first-time
costs were waiting on that exact frame: the underwater pass compiled its shader lazily at first
submersion (a full compiler run plus the driver's JIT, on the render thread), and every newly
seen liquid chunk paid a read-only buffer lock for its vertex height, a call that can wait on
the GPU being finished with the buffer. Neither is allowed to wait for the dive any more: the
compositor pre-warms the pass's shader on the ordinary frames it prepares (`Underwater::Prewarm`,
through the same retry gate failure handling already used), and the vertex read spends at most
one lock per frame (further chunks answer from their origin plane until their turn, so the cache
is fully warm within a second of looking at any water) and, as a further belt, never locks a
dynamic default-pool buffer at all. The underwater shader itself has no dynamic loops (a fixed
three-tap unroll), so there is nothing left on that path that can spin. (Round 12 later removed
the pool refusal: wherever a client's chunks are flagged dynamic it made every reading answer
from the origin plane forever, and the budget alone is the safety it was always really resting on.)

### Verification

`tools/UnderwaterRegression.cpp` is at 140 assertions (the footprint gate and its ordering, the
pre-warm and lock-budget pins); `tools/WaterRegression.cpp` unchanged at 5179. The Win32 build
and `fxc` slots are CI's; the live proof is a horizon with opaque water up to its far edge, and a
cold first dive with no hitch.

## Full rework round 9: ask the draw, and only the draw (2026-10-18)

The same player, on the round 8 build, this time with `Underwater.log` attached: dozens of tuning
lines reached the effect, and then one transition for the whole session - `underwater pass idle
(2): ramp 0.000000, medium valid 1`. The ramp never moved, so nothing downstream of it ever ran,
and the toggle and the debug modes - which live inside the pass the ramp gates - could not have
changed anything on that client. The question was no longer what the medium looks like. It was why
the tracker never once believed the camera was wet.

Round 7 had blamed the scale refusal and fixed it, and the fix was real - but it kept the reading's
two other outside dependencies, and both turned out to be choices rather than facts:

1. **The live view.** The reading dotted the draw's origin with the live `D3DTS_VIEW`'s up row and
   demanded the draw's basis agree with it direction-for-direction. A fully shader-driven client
   uploads its view as shader constants and touches the fixed-function state only on the draws
   that still need it, so "the live view" at a liquid draw is whatever the last fixed-function
   draw left there - plausibly stale, plausibly identity. Every honest draw then reads as
   rotated against it and is refused, and the refusal leaves no other trace.
2. **The determinant's sign.** Reflections were refused by `det(basis) < 0`. Nothing in this tree
   had ever established which sign an ordinary draw carries - a right-handed z-up world through a
   left-handed view can carry the negative one in the *normal* case - so the test could as easily
   refuse every real draw and admit the mirrored one. It was a coin the code had flipped without
   looking.
3. **The origin's plane.** The reading measured the draw origin's plane, which is the liquid's
   plane only if the water level lives in the matrix. Where the level lives in the vertices, the
   answer is one water level off in whichever direction the tile chose - wrong even when every
   gate above passed.

The reading now asks the draw, and only the draw. In row-vector convention the rows of
`modelView` are the view-space images of the liquid grid's own axes: two in the plane, one up out
of it. The plane's normal is the cross of the first two rows, oriented to the third; a vertex at
local height `h` lies on the plane at `row3 + h*row2`; the camera is the view-space origin; so
`dot(row3 + h*row2, n)/|n|` is the plane's signed height over the camera - exact for translation,
rotation and per-axis scale, needing no second matrix, no world up, and no opinion about
handedness. Shear is still refused (rows not mutually orthogonal), because there the cross is not
the third axis's direction and "up out of the plane" stops being one direction, and the 4000-yard
bound stands. The determinant is still computed - for the log line, where its sign settles the
convention question empirically instead of by assumption - and never acted on.

The two facts the draw's matrix cannot carry are plumbed to it instead of derived:

* **Which pass this is** is read off the render target. The composite publishes the surface the
  player's own view is drawn into (`watereffect::mainTargetSurface`); a liquid draw aimed
  anywhere else - the client's reflection texture above all - does not reach the tracker and does
  not publish a medium. The backbuffer always counts as the player's view, so a client running
  without the volume path classifies the same way, and a failed target query answers "main"
  rather than silencing everything.
* **Where the plane sits along the draw's own up axis** is read from a vertex the draw actually
  consumes - its first one - using LocalLightManager's torch pattern verbatim (declaration, stream
  and index queries, bounds against the buffers' own descriptors, read-only single-vertex locks,
  the draw parameters handed over as a hint by the four detours, which are the only place those
  parameters exist). A chunk's height is read once and cached against its buffers; the cache
  holds the buffers so a freed-and-reallocated pointer cannot alias, and it is dropped at device
  reset, where an extra reference to a client default-pool buffer would fail the client's own
  reset. A draw whose height cannot be read still reports, from its origin's plane, and the
  status line counts those separately (`noVertex`).

The status line grew the rest of the answer: `uwPlane=valid/degenerate/shear/range/offscreen/
noVertex[/far] yards=<last reading> [horiz=<its horizontal distance>] vertexZ=<last height>
detNeg=<negative-determinant readings>` - the seventh field and `horiz=` arrived with round 12's
locality gate, counting the heights that belonged to water somewhere else. If the medium ever goes
missing again (or ever turns up where it should not), one `WaterEffect.log` line says which door is
shut, and the last reading says what the water thought the truth was while it was shut.

### Verification

`tools/UnderwaterRegression.cpp` is at 139 assertions: the reading re-derived against every
camera rotation, a vertex-carried level, a mirrored basis *measured and reported rather than
refused*, yaw-in-plane, both scales, shear and garbage refused with their reason codes, and
wiring pins for the target publication, the four detour hints and the cache's death at reset.
`tools/WaterRegression.cpp` re-pins the new reading on its side (5179, passing). The Win32 build
is CI's; the live proof is a swim where `Underwater.log` says `underwater pass drawing` and the
status line's `yards=` goes positive.

## Full rework round 8: the far side of a wave is not drawn (2026-10-18)

The same player, on the round 7 build, still saw the front face of a wave through the back of it,
and said the thing that turns out to name the mechanism: it can be related to how the water
treats its underwater part, "so I don't see *through* the wave, because it's underwater."
Round 6 fixed the two halves it could see from the shader text - the camera-side classification
and the coverage ceiling - and both were real. But coverage has a limit round 6 stated as a
virtue and never followed to its end: alpha says how strongly a face paints, it cannot say
that a face should not have painted. A displaced crest is a hump of water; a ray from a low
camera produces two fragments for it, the face it enters by and the far face it would leave by.
The far fragment is shaded like a legitimate surface (its normal is flipped, so it carries the
same foam and glint as the front), it writes no depth, and it blends over or under the front
face in whatever order the liquid mesh happened to be indexed in. At `depthAlpha=1` the far
sheet drawn second simply wins. No alpha fixes that; the sheet has to not be drawn.

Which side is far is a fact about the mesh, so it is read off the mesh. The derivatives of the
interpolated displaced position give the triangle's own plane - a `cross(ddx,ddy)`; the bed
slope further down the same stage is the same move over a yard-scale span - oriented to the
surface's top by world up,
which neither winding nor a mirrored reflection view can subvert (a cross product flips under a
mirror and re-orienting by up flips it back). Two other normals were candidates and both are
lies for this question: the interpolated vertex normal is the flat tile's up vector and knows
nothing of the displacement, and the shading normal's slope is scaled by NORMAL STRENGTH, not
by the lift in the geometry. `wrongSide` is positive for the underside seen from the air and
for the top seen from inside the medium - the part of the wave that is underwater *from the
camera's side of it*, which is the player's sentence as an equation. It fades over about
fifteen degrees past the silhouette and discards outright beyond the fade: the discard also
keeps the far sheet out of the mask and surface-depth outputs, and it refunds the eight
trains, three noise octaves and two ray marches on near half the fragments of a low-camera
sea. At the middle of the camera-side band it is exactly zero, because an eye in the surface
has both sides legitimately in front of it. Above the band it does not depend on the tracker
at all: with no plane reading the lane reads "above", and undersides are discarded, which is
the correct above-water answer with a dead tracker as well as a live one.

### The underwater fog, audited

Asked directly whether far horizon and land are hidden under water, and whether there is an
underwater fog: yes, and it is the medium rather than a fog. Submerged terrain is marched
through per-channel Beer-Lambert over its true distance with the deep colour as the asymptote
(at the shipped clarity, green is at 11% by sixty yards and 3% by a hundred, before the silt
of round 7 deepens it), and a level ray that never meets the plane runs to the far cap, so
there is no distance at which a submerged shore can be read clearly. One gap did exist, on
exactly the pixels the screenshot showed: land and sky *beyond* the plane, on rays the column
clamp cuts back to the surface, were transmitted by the short camera-to-surface column alone -
as through glass, at angles glass does not transmit at. A ray leaving water for air only
leaves inside the critical cone (cos .6613); outside it the plane is a mirror from below. The
water stage already states that on its own pixels (`totalInternal`); the medium now states it
on the pixels no tile covered: such a ray is marched as though it never leaves, to the far cap,
and the in-scatter and asymptote fill it. Horizon land from under water is therefore not a
matter of clarity or dirt any more: outside Snell's window it cannot arrive, and inside the
window it arrives through exactly the water over the diver's head. The regression states it as
an equality - a bright shore and a black shore at the horizon shade identically from two yards
down, and differ by half the frame nearly overhead.

All of it still has the one precondition round 7 built the diagnostics for: the medium only
exists while the pass draws. `Underwater.log` says `underwater pass drawing` or names the
guard keeping it idle, and the water status line counts the plane reading's answers. If those
say the pass is not drawing, the fog in frame is the client's flat one, and that is a log
question, not a tuning one.

### Verification

`tools/UnderwaterRegression.cpp` is at 134 assertions (the sail re-derived through the window's
edge, the horizon equality, the overhead contrast, and mirror pins for both new terms);
`tools/WaterRegression.cpp` unchanged in count and passing (5179); the full portable replica is
green. `fxc` slots and the live look are CI's and the client's, as ever.

## Full rework round 7: silt, and a reading that can say why it said no (2026-10-18)

A player report with a screenshot: submerged in Stranglethorn Vale, dome hills clear as day across
the valley, no caustic net, no murk, and the UNDERWATER toggle and UNDERWATER DEBUG changing nothing
at all. Everything in the frame - the deep-tinted waterline, the distance softness - was client
shading. Round 6's medium had never engaged on that client, and the worst part of the failure is
that nothing anywhere said so.

### The ice-out: a scale cancels, and the reading refused it anyway

Round 6's plane reading refused any draw whose basis differed from the live view by more than .02
or whose rows were not unit length - including a draw whose WORLD matrix carries a *scale*. Row 3
of S*T*V does not depend on the scale S: a scale's fourth row is (0,0,0,1), so a scaled tile
reports exactly the same plane as an unscaled one. The refusal was therefore always wrong, and it
fails in the worst way: silently, for every draw, on a client that scales its liquid draws, so the
camera-side classification, the medium, the caustics and the debug overlays are all dead at once
and every round-6 fix with them (`c218.w` reads the held default, the facing test's replacement
never lands). The reading now compares directions: each row of the draw's basis must point where
the live view's row points (cosine >= .9998), which admits any per-axis scale and still refuses a
rotation or a shear. The regression pins the same plane reported through a doubled-x and a
non-uniform scale, and the shear refused.

The deeper correction is that a reading which refuses must now say *how*: rigid(1), mirrored(2),
rotated(3), out of range(4). The water status line counts valid planes and each refusal per
interval, plus the `c218.w` lane the last draw uploaded and whether a frame published the medium;
the underwater pass logs which of its guards is keeping it idle, on transition. The next report of
this kind is answerable from two lines of log instead of another audit.

### Dirt: water is not uniformly clear

Round 6's medium was one flat extinction, which is the one thing that makes a medium read as a
fog: distant terrain through crystal water reads as terrain, because it *is* seen through crystal
water. Real dirt is a property of the place, not of the camera. The medium now carries:

* a **silt gradient**: suspended sediment hands in the column, so extinction grows with the water
  over the shaded point - `1 + .9*saturate(under/18)`, 1.0 at the surface, 1.9x eighteen yards
  down - which is also a depth cue in still water too flat to carry a net;
* a **patch field**: the wave field's broad warp read at an offset cell - about fifty yards across,
  anchored to the world by the warp's own camera phase, a +/-35% multiplier on the extinction.
  (As first shipped this sampled the camera-relative position at a fortieth scale with no phase at
  all, so the patches travelled with the swimmer; round 12 put it on the warp.) Swimming through a
  patch changes what the water looks like the way swimming into a shadow changes the light;
* `TurbidityPercent` scales the *deviation*: 0 is the zone's own medium with no silt in it (and
  the pass then agrees with the surface's bottom path exactly, as before - that pin survives),
  100 is the shipped silt, 300 triples it. There is still one clarity and the zone still owns it.

At the shipped clarity the deep water a diver actually moves through now extinguishes at up to
2.57x the zone rate (1.9 silt x 1.35 patch): green visibility at fourteen yards of depth is about
ten yards instead of twenty-seven, and distant land goes through it the way distant land goes
through a lake in August.

### Caustics

Unchanged in construction - the world-anchored net of round 6 - and now reachable in the first
place. The regression re-pins its measured mean, plateau count and the twin `[branch]` gates.

### Verification

`tools/UnderwaterRegression.cpp` is 130 assertions; `tools/WaterRegression.cpp` re-pins the tap
under the counter change. All portable steps pass locally; the Win32 build and fxc slot counts are
CI's. The live proof is the same swim with the two logs beside the screenshot.

## Full rework round 6: the camera's side of the water (2026-10-18)

Round 5 left one thing a player can point at: standing on a shore, the front faces of waves show
through the backs of them. A crest that should hide the sea behind it is translucent, and the sheet
behind it is drawn again on top of itself, so the water reads as two overlapping films rather than as
one body with a surface. This round is that bug, and the second half of the same question - what the
world looks like from under the surface - because both halves turn out to be one missing fact: which
side of the interface the camera is on.

### What the double wave actually was

Water writes no depth and blends, and the surface is drawn two-sided, so every fragment the sea puts
up has to decide for itself how much of what is behind it to let through. Two independent things were
getting that decision wrong, and they compounded:

* **A per-fragment facing test was standing in for a per-camera fact.** Round 2's underwater optical
  path - the water-to-air critical angle, the camera-column Beer-Lambert, the coverage floor that goes
  with them - was gated on `underwater = 1 - step(-1e-4, dot(n, v))`, the sign of the *triangle's*
  facing. A facing test is true of every face near edge-on, and at a low camera above water the whole
  sea out toward the horizon is within a degree or two of edge-on, so a large fraction of an
  above-water frame took the underwater path: total internal reflection where there is no interface to
  reflect off, absorption over the full camera-to-fragment distance in a column that is air, and the
  coverage floor. Those patches read as dark mirror sheets with a bright crest composited over them -
  which is exactly "the front of one wave seen through the back of the next", drawn twice, once for
  each face.
* **Coverage was a hand-tuned fraction of the column instead of the column.** `depthAlpha =
  absorption * (.78 - .20 * fresnel)` let at least 22% of whatever was behind a face through, and 42%
  at the grazing angles where wave faces overlap most. Whatever was behind was very often another face
  of the same wave, and a fifth of it is a visible second sea.

Neither is a tuning mistake, which is why no look-setting could reach it: the first asks a triangle a
question only a camera can answer, and the second caps a number that the physics already gives.

### The camera's side, read from the draws

The renderer cannot ask the client where the water is, but every liquid draw it replaces carries two
matrices that say it. In row-vector convention (`p_view = p_local * M`), row 3 of `modelView` is the
plane's own origin in view space and rows 0..2 are the view-space images of the world axes, so with
`up` in view space:

```
dot(up, row3(modelView)) = plane.z - camera.z          // yards of liquid over the camera
```

when WORLD is a pure translation - and a liquid tile's WORLD *is* a pure translation, which is the
same fact `AnchorFromModelView` already relies on. It holds for any camera rotation because the rows
are orthonormal, and it is exact: no depth buffer, no per-pixel test, no ray against a mesh.
`renderer::underwater::PlaneOverCamera` in `src/Water/UnderwaterMedium.h` computes it, and refuses -
returning `valid=false` rather than guessing - a mirrored view (`det(basis) < 0`, the client's own
reflection pass, which would report twice the depth with the wrong sign), a rotated or sheared liquid
(a *scaled* one was initially refused too; round 7 shows why that was wrong and what it cost)
object (not a plane this camera can stand in), a non-finite register, and a reading further than
`kMaxPlaneYards = 4000`.

`watereffect::Scope` reads it on every matched liquid draw and hands it to a `Tracker`, which keeps
the plane **nearest** the camera - a draw that reports a lake the player is standing beside must not
outvote the tile under their feet - and settles the frame's answer in time:

* smoothing at `kSmoothingRate = 9` per second, so a new reading is 95% arrived in about a third of a
  second: fast enough that surfacing feels immediate, slow enough that a bobbing swimmer cannot
  strobe;
* hysteresis, submerged at `kEnterYards = .22` and not surfaced again until `kExitYards = .06`, so the
  answer cannot oscillate on a camera sitting exactly at the plane;
* a ramp, `Smoothstep(.22, 1.5, yards)`, which is the one number every pass reads;
* **the remembered plane, not the remembered distance.** A frame usually draws no liquid at all -
  looking at the seabed, the surface is behind the camera and frustum-culled - and simply holding the
  last answer would keep the medium on a diver (right) and also on a player who walked up a beach with
  the lake behind them (wrong). So the tracker stores the plane's own height in the world
  (`camera.z + yards` on any reading that produced one) and, on a frame that drew nothing, re-measures
  the current camera against that height. A diver looking down stays in; a swimmer who rises leaves; a
  player who climbs out onto the sand leaves too, all with no draw to say so. With no camera at all
  there is nothing to measure against and the answer is held exactly.
* a camera-jump reset at `kCameraJumpYards = 50` in one frame - a teleport, a hearth, a zone change -
  which drops the remembered plane *and* the settled answer, for the same reason the atmosphere drops
  its temporal history at the same distance: the state being left behind is not a measurement of
  anything the player is now inside. It is checked on every frame, and the remembered plane also
  expires after `kPlaneForgetYards = 450` horizontal yards from where it was observed, with both
  clears latching until the new place itself observes water. (Round 12: the reset originally ran
  only on unobserved frames and the memory never expired, which between them is how a teleport put
  the medium on dry land in Mulgore and kept it there - the reading that feeds this tracker also has
  to be *local* water at all, which is `PlaneOverCamera` measuring sideways as well as up.)

The same fact - a frame usually draws no liquid at all - reaches everything else the medium reads, and
not only the plane. A diver looking at the seabed has the surface behind the camera and frustum-culled,
so no `Scope` runs and nothing publishes: that is the most common underwater view there is. The up axis
and the light therefore have per-frame sources of their own, backfilled by `AdvanceUnderwater` on a
frame that drew nothing. The wave field goes one step further and is not published or backfilled at all:
the composite builds it every frame, through `UnderwaterFieldAt`, for the very frame context it
reconstructs against - the motion is integrated once at `Present`, so the field can be built without a
draw, and it MUST be built against the consuming camera rather than merely this frame's, because the
phases carry an anchor camera's place and a field from the previous frame's `Present` is the previous
frame's camera. (Round 12: a per-draw publication, and a `Present` rebuild before it, both failed that
test by one frame's worth of swimming, and the net crawled.) World +Z in view space is row 2 of this frame's captured view, and the
source the surface is lit by is `CelestialTracker`'s confirmed disc, which is a singleton rather than a
per-draw constant. Only the light's *colour* is a per-draw constant (c35), so only that one is held; it
moves on the scale of a weather front, not of a frame.

Holding the rest instead was two visible bugs: the caustic net froze on the bed while the wobble clock
kept running, so the frame moved and the net did not, and the up axis stayed pointed where the camera
was looking when it last saw water, which puts the beams, the sky glow and the bed's facing test on the
wrong axis for every turn of the diver's head after that. The same row-versus-column convention was
wrong in one other place, and it predates this round: the anchor fallback a draw takes when it has no
rigid basis of its own read the up axis *across* the rows of the captured view, which yields the view
axes expressed in world coordinates and applies the camera's rotation twice over - exactly the mistake
`AnchorFromModelView`'s own comment names, and exactly what `PlaneOverCamera`'s identity cannot
survive, since `dot(row2, row3)` is the plane's height over the camera only because the rows are
orthonormal. It now reads row 2 like everything else in the tree, and the tests that read the plane
under every camera rotation are what prove that convention.

There is deliberately no switch on any of this. Which side of the interface the camera is on is a fact,
and the water stage's use of it is the bug fix; a tuning key that could turn the classification off
would be a key that could turn the bug back on. `[Underwater] Enabled` switches the *medium* (the
full-screen pass and the ramp the air passes fade out on), not the reading.

### What changed in the water pixel stage

Three lines, and no register, no interpolant and no texture:

* `float underwater = smoothstep(-1.0, 1.0, waveSwell2.w);` — c218.w, previously unused, now carries
  `clamp(submersion / band, -2, 2)` where `band = max(kShaderBandYards, geometry.amplitude *
  kGroupMax)`. Dividing by the band is what makes the classification cross over the distance the sea
  is actually rough enough to need instead of snapping at the mean plane: a third of a yard on a flat
  lake, the full group envelope on a swell that lifts three. Above that band - above the waves, not
  merely above their mean - every fragment of every tile now gets exactly 0, which is the identity
  round 2 claimed and could not keep. Inside the band the answer is deliberately partial, because at
  the scale of the waves the interface is not a plane but a thickness and a camera inside it has water
  both over and under it. What that costs an above-water frame is very little: the camera-to-surface
  segment there is a third of a yard or less, so the blend moves the column a few percent toward it,
  and the deep tint never lands on a frame the camera is not inside - which is exactly what the old
  facing test did to every near-edge-on face in the sea.
* `float column = min(lerp(effectiveDepth, cameraColumn, underwater), 400.0);` — under the surface,
  what is "behind" a fragment is *air* (the sky, a shore, a hull), and the captured-depth column
  measured from the surface down to it was a length of air being mistaken for a length of water: a
  hundred yards of murk for a camera three yards down, which is why every upward view collapsed onto
  the deep colour. The water a submerged ray really crosses is the camera-to-surface segment, so that
  is the column: looking up from just under the surface shows the sky and the shore almost undimmed,
  and the murk grows as the swimmer goes down.
* `float depthAlpha = absorption;` — coverage is what this column extinguishes, `1 - T`, the same
  number the colour already converged to the deep tint by. Deep water is now the deep colour rather
  than the deep colour over a fifth of whatever was standing behind it; shallow water is untouched,
  because there `T` is near one and the bottom still shows through, which is the only translucency this
  pass owes.

### The medium: what the world looks like from inside it

The client's own answer to being underwater is a flat fog - one colour, lerped in by distance, the
same for a pebble at the diver's feet and a wreck at forty yards, with no red in it and no light in
it. It is applied per object at draw time, so nothing downstream can fix it up, and the water stage
cannot either: the water pixel stage only runs on the liquid's own pixels, and a diver looking at the
seabed is looking at pixels that are not water at all. The medium therefore has to be applied to the
frame after the frame is drawn, which is where every other whole-frame term in this renderer lives.

`src/Effects/Underwater.h` is that pass: one ps_3_0 draw, one full-screen quad, no history, no
intermediate target. It reads the compositor's scene copy, the depth it already reconstructs for
ambient occlusion, and the water mask that marks the liquid's own pixels, and writes the mixed frame
back into the target with blending off. Per pixel, in the order and with the literals of
`renderer::underwater::ShadePixel`:

The column all of it is integrated over is the distance to the point the ray ends at, capped at the
same `kFarColumnYards = 250` the water stage caps its own camera ray at - and then cut back to the
liquid plane on any ray that reaches the plane first. Looking up at sky through an opening, at a sail,
at a pier's upper works, at a cliff face the water was never tiled over, the segment past the surface
is *air*, and counting it as water would extinguish the diver's brightest view by a column that is not
there: a straight-up opening from eight yards down is eight yards of water, not two hundred and fifty.
That case is not hypothetical either - a diver looking up at a pier gets exactly those pixels, because
the liquid tile fails the depth test behind the pier and so no mask covers them. Both halves of the
renderer now say the same thing about where the water stops, though they read the plane from different
places: the water stage bands the *raw* per-draw reading, falling back to the tracker's held yards when
a frame draws no liquid, and this pass uses the tracker's *settled* yards, which is the height the
ramp, the beam march and the net's depth fade are already built on. The two differ by at most the
smoothing lag - about a third of a second of the camera's own motion - and both sit inside the band the
classification crosses over. Downward and
level rays never meet the plane and keep the full distance to the bed they are looking at; a grazing
upward ray meets it so far away that the minimum picks the real distance. `ShadePixel` publishes the
yards it used as `PixelOutput::columnYards`, and the net and the beam march both read the end of the
*water* column rather than the end of the ray, so a beam is never marched from a point in the air
above the surface and a net is never focused onto geometry that is not under water.

1. **Beer-Lambert per channel** over the column with the *same* per-yard extinction the surface uses -
   `bodyStyle.x * waterStyle.y * absorbTint.rgb` - so a seabed fades out with the coefficient the sea
   above it fades out with and the two meet without a seam. `MediumFor` authors nothing: it re-spells
   the registers the water draw was handed, in the same units, per zone like the rest of the water.
2. **Caustics**, applied to the world *before* the medium eats it, because a net thirty yards away is a
   net the water has already taken the red out of. The net is the two octaves the surface's own chop is
   made of (`noiseGradient`, on the same wrapped lattice), crossed as ridges and sharpened
   (`pow(net, 2.2)`), normalized by `kCausticNormalization = 5.0661` - the reciprocal of the net's
   measured mean - so caustics move light around on the bed instead of creating it: the modulation
   averages exactly one. They need water over them to focus through, an upward-facing bed to land on,
   and a source above the horizon, and they are gone by `kCausticReachYards = 28`, which is the physics
   of a shallow-water phenomenon and not a slider.
3. **Henyey-Greenstein in-scatter** on the water stage's own normalized lobe, `g = .3 + .34 *
   saturate(sunElevation)`, with the light arriving through the surface carrying the broad warp's own
   structure: each ray is walked back up to the plane and `kShaftSamples = 3` points of that surface
   are sampled where it entered, averaged and scaled by `kShaftGain = 2.2`. The beams a swimmer sees
   *are* the patches of surface the light came through.
4. **The sky's downwelling glow** on up-looking rays, which is what keeps a pixel the client left as
   sky - no liquid tile drawn there - reading as water overhead rather than as a flat deep colour. With
   the column cut back to the plane, that pixel is sky seen through exactly the water over the diver's
   head: bright and still blue from just under the surface, and from down deep almost entirely the
   water's own scattered light, which is the honest answer and the one this term exists to give.
5. **The column's asymptote**: the authored deep colour and nothing else.

Plus one term that is not a light term: the whole frame wobbles, by at most `kMaxDistortionTexels =
1.6` texels at `WobblePercent = 100`, because a camera inside the medium is looking through a moving
interface it cannot focus past. It is applied to the sampling uv uniformly - the liquid's own pixels
wobble with everything else, so the waterline is not a seam where the frame moves on one side and holds
still on the other - from four rates whose shared period (`kWobblePeriodSeconds = 20pi`) the CPU wraps
the clock into, so the argument never grows past the point where a float32 stops having fraction bits.

The pass does not touch the liquid's own pixels: the water stage has already marched that column with
the same coefficient, and applying the medium twice would be applying it twice.

### One ramp, and the air passes

`FrameContext::underwaterRamp` is published by the compositor from the water stage's settled tracker
before any pass reads it, and gated on the medium's own switch so `UNDERWATER=0` restores the air
passes exactly as they were. Three passes read it:

* the atmosphere scales every density it integrates by `1 - ramp` - haze, height fog, local fog,
  scatter and the distance-fog vignette all fade out together, and the pass does not run at all once
  the ramp reaches 1, because a diver is not in that medium and marching it would cost a
  low-resolution march and a full-resolution composite for zeros;
* the cloud deck stops landing on a surface seen from underneath (`submergedWater` in the composite:
  emission to zero, transmittance to one, which is the identity for that pixel), because a cloud
  painted over water the water stage has already shaded is a full-brightness cloud ten yards under it.
  Above water this is an exact identity and the sea keeps the deck it has always had;
* the underwater pass fades its own medium in on it.

Ordering is what makes this one number rather than three opinions: the ramp is published before the
air passes read it, the medium is applied after them and before the grade, and the scene copy the grade
and the anti-aliasing read is republished after the medium is applied.

The ramp is folded into the medium's *strength* - `saturate(deepColour.w) * ramp` in the stage,
`Saturate(settings.strength) * Saturate(in.ramp)` in `ShadePixel`, the same product - rather than being
applied as a second blend over the finished frame. That is what makes the crossing one medium becoming
another instead of two pictures of the world dissolving: at half a yard under, the transmittance sits
exactly halfway between 1 and the full column (scaling the *effect* of the exponent, not the
coefficient, so the crossing is linear in what a swimmer sees), the extinction and the caustic net are
exactly half, and the in-scatter is a quarter, because both the absorption it rides and the strength it
is gated by are halved - the murk thins out faster than the extinction does, which is the right shape
for a surface you are rising toward. `PixelInput::ramp` defaults to 1, the settled state a diver is in,
so at ramp 0 the core is an exact identity and at ramp 1 it is the full medium, with the same numbers
the stage uses. The crossing is monotone by construction and pinned that way, so a camera bobbing in a
swell cannot flicker between two looks.

### What the pass refuses to draw

The pass holds the finished frame, so every way it could be handed something it cannot interpret ends
in a refusal that leaves that frame exactly as it was, rather than in a draw that guesses:

* no ramp (`ramp <= 0`), or a medium that extinguishes nothing - `MediumFor` reports that as
  `valid = false`, because a pass that would draw an identity is a full-screen quad spent on nothing;
* no camera this frame: `viewRawValid`, and `cameraValid`, the flag the camera capture publishes the
  inverse view, the projection unpack, the far Z and the camera position under;
* a projection that cannot be inverted - `projUnpack` non-finite or degenerate, `depthMaxZ <= 0`;
* any of the three inverse-view rows non-finite or collapsed. Those rows turn a view-space ray into
  world yards, and every world-space term reads the result: the net's lattice coordinate, the beam
  march's entry point, the bed's facing test, and now the ray's meeting with the plane. One NaN in one
  row is a NaN in every pixel, and a collapsed row puts the whole world at the origin, so they are
  validated on the CPU where a refusal is free rather than per pixel where it would cost the frame;
* a non-finite up axis or light direction, and a non-finite `underwaterYards` - guarded with
  `std::isfinite` rather than clamped, because `std::max(NaN, 0.f)` is not specified to be `0.f`, and a
  NaN would otherwise sail through the clamp into every beam and every net in the frame;
* MSAA, which is not a choice this pass makes: `src/D3D9/DepthCapture.cpp` refuses INTZ depth capture
  under multisampling ("Set in-game Multisampling to 1x"), so `depthTexture` is null and every
  depth-based pass in the compositor stands aside, ambient occlusion included. The water mask's own
  MRT is refused under MSAA as well, so the pass would have nothing to reject the liquid's pixels with
  even if it ran;
* and last, the stage's own output: `dot(colour, colour) < 1e30` before the frame is written, so a NaN
  or an infinity that got past everything above returns the frame the pass was handed instead of
  poisoning every pixel of it with one bad value. A NaN compares false against everything, which is
  what makes that spelling a guard rather than a comparison. The CPU validates the same numbers before
  it uploads them, so this is belt and braces - and "should be unreachable" is not a property of a
  driver.

Two more refusals are about cost rather than safety, and both are exact. The net's work is gated twice:
once on `causticScale`, the product of the authored amount, the source's elevation and the frame's
strength, which is a *constant* for the whole draw, so a tile skips the fade, the net and its two noise
evaluations together whenever caustics are off, the sun is under the horizon or `STRENGTH` is 0 - the
same shape as the water stage's quality gates, which are comparisons against a constant register for
exactly this reason; and once on the net's own amount, which is per-pixel, so every wall, every point
past `kCausticReachYards` and every pixel of sky skips the two noise evaluations as well. The
per-pixel gate is a mathematical no-op rather than an approximation - `lerp(1, net * N, 0)` is `1` for a
bounded net - and `ShadePixel` carries both gates in the same order, so the mirror stays one mirror. And the wobble clock is
wrapped where it is *kept*, in `WaterEffect::AdvanceUnderwater`, by subtracting whole
`kWobblePeriodSeconds` instead of resetting to zero: the four rates are all even multiples of pi over
`20pi`, so they stay continuous across the wrap, and a published clock inside one period keeps the
fraction bits the wrap depends on at any session length. `Wobble` wraps its own argument too, but a
wrap is only as precise as the number it wraps.

### Authoring

`[Underwater]` in `GraphicsEffects.ini`, reachable from F7 in the WATER tab (they are the same water,
and the tab count is pinned at eleven): `Enabled`, `StrengthPercent`, `CausticPercent`,
`ShaftPercent`, `SurfaceGlowPercent`, `WobblePercent`, `DebugMode` (0 off, 1 the gates, 2 the
in-scatter, 3 the extinction as a grey, 4 the caustic modulation). There is no clarity key and no
colour key: the medium's clarity *is* `WATER CLARITY` and its colours *are* `DeepColour` /
`AbsorptionColour` / the zone's authored water, resolved through the same three-way precedence the
draw resolves them through (`ResolveWaterColourSet`), so a diver and the surface above the diver
cannot disagree.

### The client's own underwater fog, and one thing this round deliberately leaves alone

The client fogs the world itself while the camera is under water: a flat colour lerped in by distance,
applied per object at draw time. This round does not touch it, so a diver gets that fog *and* the
medium - two extinctions over the same column, which means the far distance is murkier than `WATER
CLARITY` on its own implies. That is a real interaction and it is named here rather than discovered in
a forum thread.

Fading the client's curve out on the ramp is the obvious follow-up and it is not free: it would mean
overriding a fog constant on every world draw, keyed by a table of known shader families
(`distancefog::Scope`), inside a scope that already stands aside when the atmosphere owns distance fog,
and the client's underwater fog may not even ride those constants. Three unknowns that need a GPU and a
3.3.5 client to answer, for a change whose failure mode is world geometry losing its fog at the wrong
moments. `TODO.md` carries it as an open item.

In the meantime `StrengthPercent` is the lever: it interpolates the medium's whole effect - extinction,
caustics, in-scatter and asymptote together - so a diver who finds the combination too murky dials it
down instead of off, and `0` leaves the client's fog standing alone exactly as before this round.

### Known limitation

A camera underneath a liquid plane it cannot see through - a cave with a lake over it - reads as
submerged, because there is no per-pixel or per-frame signal that distinguishes the two. `UNDERWATER`
off turns the medium off and the camera-jump reset drops the state on a teleport.

### Verification

* `tools/UnderwaterRegression.cpp` (new, portable, runs in CI without a GPU) began at 121 assertions over
  the whole contract: the plane reading under every camera rotation and position and its five
  refusals; the tracker's hysteresis, its hold, the remembered plane, the surfacing and beach-walking
  cases that have no draw in frame, the jump reset and ordinary swimming; the medium's numbers
  (extinction, first e-fold near thirty yards, red first, the far cap, `DEPTH` off, zone clarity,
  garbage tuning cannot produce a NaN); `ShadePixel` against the water stage's own bottom path term for
  term; the caustic normalization re-measured from four million samples to four in a thousand, the
  plateau count that once printed a lattice onto the foam, peak over mean about five, the modulation's
  mean exactly one, and the net shown to be the two octaves of the surface's own chop via
  `renderer::water::NoiseGradient`; the beam statistics (a peak above .9, a duty cycle under 35%,
  sub-horizon safety) and the wobble's peak, wrap continuity and NaN clock; the surface crossing modelled
  rather than asserted - ramp 0 an exact identity term by term, ramp 1/2 exactly half the extinction
  and half the net and a quarter of the in-scatter, a ramp outside 0..1 clamped, and the whole crossing
  walked in twenty steps to prove it is monotone; the column's meeting with the plane, from eight yards
  and from sixty and on a level ray and on a downward one and on a sail above the waterline; the frame
  that drew no liquid still publishing a moving net, this frame's up axis and this frame's light; and the literal pins
  that hold the ps_3_0 stage, the water pixel stage, the compositor, the atmosphere, the cloud
  deck, `FrameContext`, the ini and the F7 layout against each other, ordering included.
* `tools/WaterRegression.cpp` gained the pins for the new classification lane and for the CPU mirror's
  coverage and column (5179 assertions, unchanged elsewhere).
* `tools/WaterPreview.cpp`, the CPU mirror, follows the coverage change (`depthAlpha = absorption`) and
  states which side of the column's underwater branch a preview camera takes: the preview renders from
  above the plane, where `underwater` is exactly zero, so the captured-depth side of the lerp is the
  whole column - a stated identity rather than a silent omission.
* `tools/ValidateShaders.ps1` compiles `kUnderwaterSource` with `fxc` on Windows and budgets it at the
  ps_3_0 device floor, 512 slots, the same ceiling `kAmbientOcclusionSource` (501) sits under. Measured
  on the runner: **481** - 456 before the column met the liquid plane, so the clamp and the two caustic
  gates cost 25 slots and leave 31 under the floor. Round 7's silt and patch field take it to **505**,
  7 under the floor, and round 8's Snell window to **509**, 3 under - bought by centring the beam samples
  once over their mean instead of per sample: the next term this stage gains has to earn its slots, not
  just want them. The estimate while it was being written was
  250-350, the usual direction of error in this tree, where vectorised work costs more than it reads.
  The runner's `Shader instruction counts` annotation carries the figure for whatever commit is being
  read; a slot count is a property of a commit and not of a document.
* The water pixel stage pays for the classification fix, and the cost is stated rather than left to be
  found: **958** slots against main's **954**, four slots for a `smoothstep` and a `lerp` in a budget of
  2048. Both figures are the runner's own annotation for the respective commits, so the delta is
  measured and not estimated.
* No `[branch]` in the new stage wraps a body that samples with an implicit LOD, because fxc rejects
  that outright on ps_3_0 (`X3528`, "can't force branch with gradients on non-inputs") rather than
  merely compiling it slowly. The first CI run said so; the two `[branch]`es that remain sit on shader
  constants around pure arithmetic, and every test that reaches a `tex2D` is a plain `if`.
  `tools/UnderwaterRegression.cpp` pins the rule, and `VolumeEffects.h` carries the same one for its
  anti-aliasing taps.
* The Windows runner builds the DLL, runs the water GPU regressions and the F7 overlay device
  regressions against these changes; the live look - a coast at a low camera, a diver looking straight
  down, surfacing, walking up a beach - remains an in-client check, and the sandbox this round was
  written in has neither `fxc` nor `d3d9`.

## Full rework round 5: the quality ladder, mid waves, shore and back flow (2026-10-17)

Round 4 left a sea that reads as a body of water, and a problem no look-setting could answer: the
only way to buy frames back from it was to switch features off one at a time, and nothing in the
tree said how much the water cost. This round adds the missing third kind of control - one level
that trades the whole set of expensive terms at once - and spends the room it creates on three
things the reference has and this renderer did not: waves in the band between the swell and the
chop, a shoreline that knows a beach from a rock wall, and water that goes back out again.

### Why a ladder and not eleven switches

A slider with eleven positions is a promise that each position is worth something, and the only way
to keep that promise cheaply is to make the level the single input. `src/Water/WaterQuality.h` is
the one place the ladder is written down. `WaterEffect.h` resolves a level into a `QualityPlan`
once per tuning reload, uploads the plan as constants, and both shader stages compare the level they
were handed against that header's own gate midpoints (`GateFor(level) = level - .5`), so a level can
never sit on a boundary and the CPU and the GPU cannot disagree about what a level means. Eleven
compiled shader variants would have been the alternative; one shader with a uniform gate is what
ships, and the two ray marches take a *trip count* from a register rather than a branch, so a level
can hand them any count between 0 and the 12 their loops are written against without recompiling.

| level | name | what it evaluates |
|---|---|---|
| 0 | FLAT | no procedural sea at all: no wave gate, no chop, no marches, no emitter reflections |
| 1 | MINIMAL | the analytic swell shaded on a flat sheet, sky reflection only |
| 2 | LOW | the mesh lifts, the base chop octave, a 3-step reflection march |
| 3 | FAIR | 4-step marches; sun-road occlusion returns |
| 4 | MEDIUM | the second chop octave, bed-slope shore understanding, 6-step marches |
| 5 | GOOD | 7-step marches |
| 6 | HIGH | **the sea that shipped before this key existed** - 8-step marches, 8 emitters |
| 7 | ENHANCED | mid-wave octave, foam lace, shore back flow (**shipped default**) |
| 8 | VERY HIGH | the mesh also lifts the 20-yard mid wave, 10-step marches |
| 9 | ULTRA | 12-step marches |
| 10 | MAX | backlit foam veil, soft refracted contact line |

Two rules the ladder keeps, both pinned in `tools/WaterRegression.cpp`:

* **A level never changes the authored look of the water it keeps.** WAVE HEIGHT is the same peak
  lift at 0 and at 10, and CLARITY, FOAM AMOUNT and GLINT STRENGTH are the same numbers. The one
  place that could have broken it is the displaced mid wave - a third Gerstner train would have made
  the same authored height taller - so the vertex stage renormalizes by
  `(kWeightA+kWeightB)/(kWeightA+kWeightB+midGate*kWeightC)`, applied to the lift *and* the slide
  together. The mesh keeps its peak height and its slide-to-lift ratio at every level; the fold
  budget gets smaller rather than larger (`kMeshPinchMax = .825274`, still inside the eight-train
  `kMaxPinch` the shading clamp assumes).
* **A level is monotone.** Raising it can only add work and add detail, so the slider has no
  surprises in it and a player looking for frames only moves down.

Level 0 is cheap in a way that matters beyond the shader: with no reflection march and no sun-road
march there is nothing left that reads the pre-water copy for those two features, so
`NeedsScene()` stops asking for the capture they would not read. Refraction, absorption and foam
still ask for it, because the shore, the shoal and the back flow are all read out of that depth
buffer at every level that has them.

### Mid waves

The shortest analytic train is 4.9 yards and the base chop is one 2.5-yard cell; between them the
sea carried nothing but smooth rounded forms at a fixed scale, which is exactly what the eye groups
back into lines - the round-4 comb, one band higher. A third noise octave at just under half the
base frequency (5.4-yard lumps), counter-phased against the base octave the way the fine one is and
riding the swell between the two, breaks the regularity at the scale that was missing. It fades
with `filter` rather than with its square: at half the base frequency it stays resolvable about
twice as far as the fine octave, and fading it as early would remove it from the middle distance,
which is the only place it was added for.

Above level 8 the *mesh* also lifts the 20-yard train C, which is the shortest wave the client's
roughly four-yard liquid grid can carry without faceting (five facets to a crest). It is a ceiling,
not a step on the way to something shorter.

### Shore understanding

The shoal foam needs the bed's slope, and the only geometry this pass can read is the captured
depth, so the slope is recovered from reconstructed bed points: this pixel's own point on its ray
and four more on neighbouring rays, a fixed world distance out along the pixel's screen axes. The
cross product of the two tangents those points define is the bed's plane normal, and its
inclination to world-up is the dimensionless slope - the same number at any resolution, distance
or camera pitch, and the same number `tools/WaterPreview.cpp` measures in world space with
`FieldSlope`, which is the reading the published frames were tuned against.

The span is in yards, not pixels - `kBedSlopeSpanYards`, +-1.5 either side, `FieldSlope`'s own
baseline - and the span is the part that matters. A one-pixel stencil is exact for a plane in
exact arithmetic, but the captured depth is stored with a step, and where the INTZ fetch is a
short-mantissa floating-point depth that step is a sizeable fraction of the bed's rise between
neighbouring pixels: divided by a footprint inches across it comes back as slope, jittering a
2x2 quad at a time, and because that number drives the shoal foam, the beach/wall split and the
swash band width, a shallow bed near the threshold printed it as tick rows along its depth
contours (round 19, above). Over a three-yard baseline the same step is a rounding error at any
precision the capture can produce. The taps' texel distance comes from the lattice footprint the
pixel stage already takes for the chop fade, read back through the lattice's own cell size as
yards and grown by the bed's extra distance along the ray, so any coarser pixel keeps its own
span and distant water is untouched. Up close the shader now reports a calmer slope than a
per-pixel difference would, which is the trade: less shoal foam in the last yard than a number
that was mostly noise was producing.

One number then separates the two shores that look the same to a depth buffer: about twelve degrees
of bed is a beach, about forty is a wall. A beach gets a wide swash zone and sheets running up it; a
wall gets a narrow bright contact line and nothing that can run up it. At level 10 the same slope
buys a soft contact - the last inches of a waterline are a meniscus, not a step, so the refracted
sample's offset fades in over the first yard instead of pulling the bottom by the full wave slope
right at the seam.

### Back flow

Nothing in the renderer moved water back out of a beach, so a shoreline could only ever arrive. The
driver is the longest swell train the pixel is standing in - the same analytic train A the vertex
stage lifted the mesh with, arriving through the same interpolated sine/cosine pair - so the run-up
comes in with a crest the player can already see coming, and the drain follows it back out at the
speed the sea is really travelling. No second clock and no new phase: `sin01.x` is that crest's
height here and `cos01.x` its rate of change, positive while the water is rising and negative while
it is leaving.

Depth is the shoreline's own coordinate - "up the beach" is "less water over the bottom" - so the
whole effect is a shift of the depth the shore masks are built from, and it needs no shore normal
this pass has no way to read. It moves the shore and foam edges **only**: the absorption column, the
refraction and the in-scattering still measure the water that is really there, because a sheet
running up a beach is thin water, and thin water is exactly what those terms describe. The run-up in
yards is `SwashYards(plan, geometry.amplitude)` - the arriving crest's own height, scaled once and
capped at 2.5 yards - so it inherits WAVE HEIGHT, WAVE SIZE, the zone's sea state and the WAVES
switch without a second authoring path, and it is zero on water that was authored flat.

The foam that goes back out with it is a band in depth moving with the swell, torn into flecks by
the chop value so it can never read as rings painted on the water, kept inside the swash zone by
`shallowEdge`, and gated on `draining` - the streaks are only there while the sheet is leaving,
which is what makes them a backwash rather than a second run-up. It also carries
`foamDepthValid`, as the shore and shoal terms always did: without it a pixel with sky in the
captured depth behind it reads its clamped foam depth as swash and streaks deep water (round 19).

### Foam

Coverage was one scalar, and one scalar can only ever be a grey sheet - which is what "the foam is
nice but flat" meant. Real foam is a lace: bright filaments with dark, water-filled gaps between
them, at multiple scales. An anchored fine sample is the fleck; an exact per-pixel sample of the broad warp
is the tongue both are torn out of. Those extra noise evaluations are intentional: using a
vertex-interpolated value in high-contrast foam exposed triangle seams that shifted with the camera.
The level-7 mid octave stays out of coverage, so its projected lattice cannot return as a grid. The
lace multiplies crest terms whole, because a whitecap *is* a broken streak, and only `.45` of the
shore term, because a sheet running up a beach is continuous in a way a cap on a crest is not.

The lace reads sampled **values**, not slopes. The first implementation clipped a threshold ramp
inside the noise range (21.9% of fine cells and 20.2% of mid cells hit the low plateau; the completed
lace was flat at 1.0 for 43.1% of sampled cells). Replacing that with a smooth complement product
removed the artificial plateau, but a second in-game report showed the grid still present. That
proved the plateau was not the whole cause, and the earlier claim that the camera-moving grid was
fixed was premature.

The remaining quality-7 input was the per-pixel **mid octave** itself: level 6 removes that octave
and the artifact, level 7 adds it. Its world lattice is a useful wave-slope input, but it does not
need to modulate foam coverage. The lace combines the fine per-pixel fleck with an exact per-pixel
broad-warp sample, rather than interpolating that high-contrast foam input across liquid triangles:

```
lace = 1 - .70 * (1 - fine) * (1 - broad)
```

The mid octave continues to shape the wave surface; only its value is removed from the foam lace.
The broad field is evaluated in the vertex stage and shared across adjacent pixels/tiles, avoiding a
mid-frequency per-pixel lattice in the bright foam mask. The CPU preview mirrors the same formula.
Regression pins require the mid-value input to stay out of the lace and the clipped threshold forms
to stay gone. This is the first change aimed at the quality-7 input itself; the game capture remains
the acceptance check for the reported camera-moving grid.

Round 16 found the remaining camera-dependent input still inside that lace. The fleck read the
**fine chop octave's value**, and that octave is sampled counter-phased on purpose: `plane*2.17`
against the ripple phase with its lanes swapped and scaled by `1.31`. At a fixed world point the
relative plane and the camera-carrying phase cancel for the base octave, but not for that spelling,
so the fine coordinate moves when the camera moves even at a frozen instant. As a wave slope the
drift is invisible - it is the sea's own grain - but the lace is a bright mask multiplier laid over
a stable bed, so its flecks crawled across shoal foam and showed on the terrain under it as a
hash-like shimmer (the report's screenshot: short dark ticks on an otherwise smooth shallow bed,
exactly where the thin green side of the foam colour ramp multiplies through). Measured through
`BuildWaveField` over the regression's camera set, the old fleck's value at one world point changes
by **0.22 on average** (worst 0.58) between cameras at the same instant; every other foam input -
broad warp, base detail, swell, pinch, bed depth - is anchored, so the fleck was the one term that
could move against the bed. The fleck is now its own sample (`FoamLaceFineValue` in
`src/Water/WaterNoise.h`, mirrored in the shader): the absolute cell `plane + wavePhase.xy` under
the integer matrix `[[2,1],[-1,2]]`, a scaled rotation `sqrt(5)` times finer than the base lattice
and exact under the 64-cell wrap, riding the swell by the fine octave's own offset, so the flecks
keep their scale and their travel with the sea but not their crawl with the camera (same measurement:
worst change **0.037**, phase precision). It also gets a fade of its own (`FoamLaceFilter`): the
fleck returns to neutral between 0.18 and 0.45 fleck cells per pixel - five pixels per cell down to
two - because below that a point-sampled fleck is aliasing that breaks into ticks as its phase
drifts, while the broad tongue keeps the slower `filter` fade and outlives it. The fine chop octave
itself is untouched: its slopes still counter-drift as surface grain. Only the mask stopped reading
it.

At the same level the colour becomes a ramp on the coverage the masks already produced instead of
one flat white: a thick cap scatters everything back and reads as the bright chalk of a breaking
crest, a thin veil lets the water's own tint through and reads as the grey-green lace underneath it.
That ramp is what lets the two coexist on one wave. At level 10 the veil also glows when the source
is behind it, on the same `vDotL` the in-scatter lobe already computed, weighted by what the ramp
left over so the thick caps that already scatter everything back are not double-lit.

### Looking up through the water

The surface is a two-sided interface, but a two-sided normal alone is not a two-sided optical model.
When the camera is below the surface, the view ray travels from water toward air. At angles beyond
water's critical angle, no transmitted ray exists: the interface is totally internally reflecting.
The shader now retains the camera-side bit before orienting the normal, adds a narrow smooth TIR ramp
around `cos(theta_c)=sqrt(1-(1/1.333)^2) ~= .661`, and uses the same interface Fresnel for the
reflection mix. It also attenuates the camera-to-surface ray by the authored per-channel
Beer-Lambert coefficients and converges it toward the body's deep tint. This uses ray length, not
view-Z, so an oblique underwater view does not artificially shorten the water column. Above water,
the camera-side blend is exactly identity and the new reflection term is gated off.

> **Corrected in round 6.** The blend above was gated on the *triangle's* facing, which is a per-fragment
> test standing in for a per-camera fact, and the identity it claims held only for faces the camera was
> squarely above. See "Full rework round 6: the camera's side of the water": the gate is now the
> camera's own distance from this draw's liquid plane, read on the CPU from the draw's matrices, and
> above water it is exactly zero for every pixel of every tile.

The output alpha is at least the reflection weight already mixed into the water colour. Without that
floor the fixed-function blend applies the original scene a second time over the computed reflection,
letting the opposite liquid face show through at the exact angles where the reflection should dominate.

A separate camera-angle dropout was visible in the user's report. Their `WaterEffect.log` showed all
sampled liquid candidates replaced (128/128, 105/105, 109/109, 157/157), no shader creation failure,
and no ignored liquid layouts; `WaterReflection.log` showed successful copies. So the effect was not
being disabled in those frames. The replacement vertex shader nevertheless consumed native VS c4 as a
fog curve without checking its contents. An alternate camera-selected VS can bind a different c4; the
fog interpolation can then collapse the whole computed water colour to fogColor, visually identical to
losing the shader. Before each replacement draw c4 is now checked against the shared finite fog-curve
shape. If it is invalid, this draw uses the identity fog curve (unfogged water), and the native c4 is
restored on every exit. The affected VS/PS hashes and bad c4 are recorded once in `WaterEffect.log`.

The user's latest `WaterEffect.log` confirms successful replacement in the sampled frames, but the
built-in F8 draw capture was disabled (`Enabled=0`), so it does not contain the per-draw shader/c4
identities at the exact bad angle. The new fallback logs an invalid c4 automatically in `WaterEffect.log`
without requiring F8. A true unmatched draw family is still separately reported by hash.

### Registers, slots and the interpolant ceiling

No register, no interpolant and no texture was added, which is the only reason this round could
happen at all: the Output struct is still eleven fields and the pixel stage still takes ten inputs,
against the eleven-`o#`/ten-`v#` ceiling round 4 documented. The ladder rides lanes that were
already travelling:

| lane | contents, as of round 5 |
|---|---|
| `c202.z` (VS) | the mid wave's phase, one of the three trains the mesh can now lift |
| `c204.z` (VS) | the mid-wave gate, a multiplier rather than a branch so it can scale the renormalization |
| `c204.y` / `c204.z` (PS) | reflection march steps, sun-road march steps (`controls[17]`, `controls[18]`) |
| `c211.z` (PS) | the quality level itself (`controls[46]`), which the stage compares against the ladder's midpoints |
| `c211.w` (PS) | the wave gate, now also zero at level 0 - the same thing asked from the other direction |
| `c218.z` (PS) | the shore back-flow run-up in yards, 0 with the feature off (`controls[74]`) |

`controls` is 75 floats of the 156 the saved `c180..c218` span holds, so the save/restore window,
the single `SetPixelShaderConstantF(180, …, 39)` upload, the untouched `c197/c198` gap and the
WATER OPACITY floor on `c199` are all
unchanged; `c204.w` and `c218.w` are still spare.

The slot count is the one estimate in this round that was wrong, and it is worth recording exactly
how wrong. This document estimated "roughly eighty"; Windows CI measured the pixel stage at **890**
slots against **713** on the commit it branched from - **+177**, and the vertex stage 169 to **188**
(+19) for the third train. The two foam-checkerboard fixes that followed - fading the lace with the
octaves it reads, and flooring the bed slope's measurement span - took the pixel stage to **895** and
left the vertex stage alone. That is round 3 and round 4's lesson a third time: the price per term is
invisible to a reader counting terms. What actually cost it is that a noise octave is a four-corner
hash plus a bicubic, so the mid octave alone is worth dozens of slots before the shore shape, the
swash, the lace, the backwash, the colour ramp and the veil are counted, and the two marches giving
back steps gave back *time*, which is not the same currency - with `[loop]` the instruction count is
static, so cutting steps buys time per pixel and nothing at all in slots.

Which is also the correction this round owes about the ladder itself: **a level buys time per pixel,
not slots.** `[branch]` gates still compile, so every level pays the same slot count and a level cannot
rescue a card whose `MaxPixelShader30InstructionSlots` is under it - such a card fails
`CreatePixelShader` and keeps native water at level 0 exactly as at level 10, which is what it did
at 713 too. The band that changed is only a hypothetical card reporting between 713 and 889; the
512-slot floor cards were already outside the shader before this round. The 2048 cap still holds at
about 2.3x headroom rather than the 3x it was written against, and the count is still the number a
change like this one has to answer for.

### Verification

* `tools/WaterRegression.cpp` walks every level: each plan is in range, every resolved gate *is* its
  level, both ladders are monotone in cost and in features, absurd levels clamp instead of indexing
  past the tables, level 0 is flat, level 6 is the shipped sea, level 7 adds the mid octave and the
  shore work at the shipped march budget, and level 10 is everything. The shader's gate literals are
  derived from `GateFor()` rather than typed again, so the two halves cannot drift by one. The
  three-train fold is pinned against `kMeshPinchMax` and the renormalization against
  `MeshNormalization()`, and train C's coefficients moved from the "shading-only" literal set to the
  "both stages assemble the displaced trains from the same coefficients" one.
* Both stages were re-parsed with a portable HLSL parser (the sandbox has no `fxc`); the modified
  pixel stage parses clean against the same harness the pre-change source does, and the vertex
  stage's only diagnostic is the pre-existing `Vertex shader must output a position` semantic note
  about `gl_Position`. Final instruction counts remain a Windows CI and live-client check.
* `tools/WaterPreview.cpp` resolves the *same* header rather than inventing its own gates, and
  gained `--quality`, `--shore-foam-width` and probe lines for every intermediate the round adds
  (`bedSlope`, `beach`, `runUp`, `draining`, `swashDepth`, `lace`, `backwash`, `foamColour`,
  `veil`), so a level can be audited one pixel at a time:

  ```
  build/waterpreview --scene coast --quality 7 --shore-foam 220 --wind 190 \
      --shore-foam-width 320 --probe 274 242 --time 22
      shore bedSlope 0.0357304  beach 1  runUp 1.31625  draining 1  swashDepth 3.31377
      foam lace 0.53838  backwash 0.265436  colour 0.436471 0.529412 0.477059  veil 0
  ```

  (Printed in full because an earlier revision of this block quoted numbers two different probe
  pixels had produced under a command line that reproduced neither.)

  The same pixel at `--quality 6` prints `runUp 0  draining 0  lace 1  backwash 0`, and the run-up
  travels the waterline between 2.7 and 5.0 yards across a swell period, which is the back flow.

## Water-heavy-area performance pass (2026-10-15)

Booty Bay exposed both halves of the water cost: a large number of liquid tiles
made the render-thread setup overhead visible, while those tiles covered enough
screen area for the `ps_3_0` wave, reflection and light work to dominate the GPU.
This pass keeps the authored eight-train sea and removes work whose contribution
was already available or exactly zero:

- The vertex stage already evaluated long trains A/B to displace every liquid
  mesh. Their two sine/cosine pairs now travel in the unused `.zw` lanes of the
  existing UV interpolators. The pixel stage renormalizes those interpolated
  pairs (bounded against the analytic field on the client's roughly four-yard
  grid), reconstructs height/slope/pinch, then adds the same six shading-only
  trains and applies the same warp, grouping derivative, normal, foam and crest
  formulas. It no longer repeats the long pair's projections and `sincos`
  evaluations for every covered pixel. No interpolator, texture, sampler or
  authored coefficient was added or removed; `.xy` remains the native UV ABI.
- SSR no longer reads scene colour on a depth miss and stops once hit coverage is
  complete. Celestial occlusion stops only when visibility is exactly zero.
  Local lights reject pixels at or beyond their squared radius before vector
  normalization and the exponent-64 specular lobe. Fractional edge visibility,
  partial SSR hits and every in-radius light follow the previous arithmetic.
- `watereffect::Scope` now saves native pixel constants `c180..c218`, preserves
  the untouched `c197/c198` gap, splices in local lights/water controls plus the
opacity floor on `c199`, uploads
  once and restores once. Native vertex inputs `c5..c35` are likewise read in one
  enclosing block. This reduces constant-state driver crossings from twelve to
  seven per matched tile while retaining complete restoration. The selected local
  lights are also transformed to the immutable frame view once per frame/selection
  signature rather than repeating the same eight matrix transforms for every tile.
- The three location-colour override decisions are resolved during tuning/location
  reload instead of asking the profile API on every tile. The resolved zone colour
  still yields to an explicit location override, key by key.
- Water can still request scene/depth capture for reflection, refraction and
  absorption when the volume renderer is off, but that path no longer allocates,
  clears or binds the volume-only full-resolution water mask and R32F surface-depth
  targets. Disabling volume also releases an existing pair (about 64 MiB at 4K).

A packed procedural-noise lookup was prototyped but deliberately not added: its
approximation error was small, yet another per-tile texture/sampler transaction
would work against the harbour draw-call bottleneck. The no-new-state changes
above are the safer first optimization. `tools/WaterRegression.cpp` pins the
packed long-wave terms, early exits, cached profile queries, constant batching
and volume-target gate. `tools/WaterDeviceRegression.cpp` verifies all 39 pixel
registers, all five vertex-control registers and the untouched gap restore on a
real D3D9 device. Portable tests can validate source and policy locally; final
shader compilation/instruction measurement and device rendering remain Windows
CI and live-client checks.

### Shared-wind re-audit

The performance change does not create a second wave clock or alter wind input.
`ModernWoWRenderer.cpp` still publishes one immutable `FrameContext::wind`
snapshot after scene/cloud rendering for the next frame. Clouds consume a new
sequence once in `VolumetricClouds::Render` through `ApplyCloudWind`, advancing
base, detail, weather and shear offsets. Water consumes that same snapshot in
`watereffect::Present`: `TargetSeaState` maps strength/weather to energy, motion,
shear and foam wind, while `WaveMotion` maps direction and motion to all wrapped
wave phases. `AffectClouds` and `AffectWaves` independently select linked versus
legacy policy; with both enabled, both effects are therefore driven by the same
revision, direction and strength. `tools/WindRegression.cpp` rechecks all eight
travel signs, strength/shear coupling, sequence gating, wave phase continuity,
reloads and production source wiring.

## Full rework round 1: square patterns and tiles of native water (2026-10-14)

Two reported symptoms, both reproducible from the code:

**1. Square patterns in the replacement water.** The procedural wave field was
built from the liquid material's own uv (`flowingUv`, then `dot(uv,·)*float2(14,31)`)
and hashed with `frac(dot(i,float2(127.1,311.7))*0.1031)`. Both are wrong at
ordinary world scale:

- uv coordinates that grow with the world position, multiplied by ~300 and then
  reduced with `frac`, leave only a few float32 fraction bits. The noise
  quantises onto its own lattice, i.e. flat axis-aligned squares, and the more
  distant the water the larger the squares.
- `fwidth()` of that same coordinate fed the level-of-detail fade, so the fade
  became garbage exactly where the pattern was already degenerate.
- Only the noise *slope* was faded with distance, not the sampled value, so the
  unresolved octave aliased into a fixed screen-aligned grid inside the sun
  glitter and crest foam.
- Anything derived from a tile's uv restarts at the tile edge, so detail was
  framed by hard squares even before precision failed.

The field is now evaluated from the position relative to the camera, recovered in
the vertex shader from **this draw's** view matrix (so it also works for the
client's reflection camera and never depends on frame-lagged capture), with the
camera's own world position added as a phase computed in double on the CPU
(`src/Water/WaveField.h`). The noise wraps its lattice indices modulo 64, which
makes reducing that phase into the period a free and invisible operation: every
coordinate the pixel shader floors stays small at any world coordinate, the
pattern is nailed to the terrain, and geometry waves now ride the same field so
a displaced crest and its highlight stay on one wave. The vertex stage cannot use
the wrapped phase (it drives two sines with different periods, and folding one of
them would slide it by a third of a wave every 64 cells), so it receives the
camera's lattice coordinate as its own unwrapped constant and adds it before the
sines. Without that constant the displacement is a function of the distance from
the camera, and the bumps visibly swim over the terrain as you move. The hash arguments are
bounded by construction (largest index 4095, before that: unbounded), the swell
octave gets its own phase and a sheared travel direction instead of sharing the
ripple's, and the sampled value fades with the pixel footprint together with the
slope. The wave normal no longer comes from screen-space derivatives of the uv
and a winding-dependent sign flip: the vertex shader hands the pixel shader the
world axis directions rotated into the lattice, so the tilt follows the real
gradient. Two shader lookups and four derivative instructions were removed to pay
for it, and the viewport `MaxZ` is folded into the depth unpack so each of the
three ray loops costs one subtraction and one divide instead of an extra divide.

**2. Square patches of fully native water.** Replacement, scene capture,
classification and ground capture all consult `water::IsMaterial`. For the
captured canonical layout (8x64 lookup + 512x512 surface) that is shader
agnostic, but every other surface size first demanded a **documented vertex
shader and a documented pixel shader**. The client picks liquid vertex variants
by camera distance and shadow quality - `src/Core/ShaderCache.cpp` records
exactly that failure for `0x48a82796bd612aeb`, "water tiles very close to the
camera ... fell through unclassified and rendered with none of our effects" - so
any undocumented vertex variant leaves a whole tile native while its neighbours
are replaced. Three changes:

- The pixel shader is the material. A documented liquid pixel shader plus the
  liquid two-texture signature is now enough, with the vertex shader left free;
  the previous both-roles requirement is what made tiles depend on a hash list
  that can never be complete.
- `0x70faf83955e2b668`, the fourth vertex variant confirmed from live logs and
  recorded in `TODO.md` but never added to `WaterPolicy.h`, is now listed, so it
  also counts as water for the atmosphere/shadow classifier at any surface size.
- Both fog paths (`DistanceFog.h`, `src/Effects/EnvironmentFogCapture.h`) keyed
  their fog-curve register on **one** of the four liquid vertex shaders, so tiles
  drawn with a sibling variant kept the native distance fade while their
  neighbours were rescaled - a hard square seam across one continuous surface.
  They now share `water::FogConstantRegister()`, and the existing shape guard
  still rejects any variant that does not actually hold a fog curve there.

Draws carrying the liquid texture signature that is *not* approved are now
reported once per distinct signature in `WaterEffect.log` as
`liquid material not covered by the captured water shaders: ... vs=0x... ps=0x...`
with the frame counter `liquidLayoutsIgnored`. Detection stays fail-closed, but
the next unmatched water family becomes identifiable from a player's log instead
of from a screenshot.

`tools/WaterRegression.cpp` covers the widened and still-rejected identities
(including the precipitation and UI shaders that must stay out), the shared fog
register, and the wave field: same terrain point from five very different camera
positions must land on the same lattice coordinate, phases must stay inside the
wrap, and the constants must remain finite after 100000 s of animation.


## Full rework round 2: the reference look (2026-10-14)

Round 1 made the waves physically anchored; this round is about what the surface is made
of. The reported target was the new renderer's water, which the panel describes as three
authored controls - movement, look and reflection - with waves that scale with the size of
the body, a river that foams where it meets a rock, and depth as an optical property rather
than a fade. Three things about the old shading kept us from reading as that, and all three
were structural rather than a matter of tuning numbers:

- **A wave only moved the highlight.** The body of the water was `surface + colour * base`
  with a small normal-driven bump, so a swell had no lit back and no shaded face. Rounded
  forms are the single loudest thing in a reference frame, so the shading normal now modulates
  the body directly, against the geometric normal, in both the light direction and the
  hemisphere it sees. Flat water and a zero-strength sea are untouched by construction: the
  terms are differences.

  That multiplier is **floored**, and the floor is not a tuning preference. `controls.z` is
  `NormalPercent * clamp(sqrt(agitation), 0, 2.5)`, so in an agitated sea the expression
  `1 + controls.z*(1.9*(NoL-dot(n,l)) + .75*(...))` goes **negative** on a face turning away from
  the source - and nothing saturates `rgb` between there and the output, so the render target
  clamped it to pure black. Water is alpha-blended and writes no depth, so the far face of a wave
  shows through the near one, and a far face clamped to black read as a silhouette cut out of the
  sea: reported as "the blacked shape of a wave visible through the water" from surface height.
  The floor is `lerp(.22, .78, saturate(-vDotL*1.7-.45))` - it keeps the whole brightening range
  and most of the shading, and rises with the light coming through the sheet toward the viewer,
  the same `dot(-v,l)` cue the in-scatter lobe and the foam veil glow use, so all three agree
  about which side of a wave the sun is on. A thin sheet with the light behind it is among the
  brightest things in a sea and never the darkest. `vDotL` moved up to the body shading because
  that is now the first of the three places that needs it; it is still computed once.
- **The "environment" was fog.** Reflections at grazing angles mixed toward `fogColor`, which
  is why open water read as a dark sheet with a bright smudge on it. The surface now reflects
  a horizon-to-zenith sky built from the scene's own fog and light colour, with the sun/moon
  disc mirrored in it, and the screen-space hit is mixed toward that sky by the wave's own
  roughness - the cheap stand-in for a blurred mip chain this pass cannot sample.
- **Depth was one scalar.** `absorption` was a single 0..1 fade, so "murky swamp" and "clear
  mountain lake" were the same water at a different alpha. Absorption is now per channel
  (Beer-Lambert over the measured column), the deep colour is what the column converges to,
  and light scattered back out of the column is what makes a crest glow when the source is
  behind it. Refraction strength still scales the *blend* and never the colour - with
  refraction off the water must fall back to the client's pixels, not to a black bottom - and
  the depth transition stays independent of it, which is the contract an earlier regression
  broke.

Two more things moved because the references showed them: the refracted sample is pulled
further the deeper the column is (shallow water and a bar stay crisp while open water
shimmers), and foam is authored rather than automatic - wind strength gates crest and shoal
foam, and a shoal term reads the depth gradient, which is the only geometry this pass can
see, so a river running over a bar whips up and a calm canal does not.

**The swell is no longer noise.** The long waves are two analytic sine trains on the same
world-anchored lattice, at 16 and 8 cells, which gives exact slopes for free and - because
both periods divide the shader's 64-cell wrap (`kSwellCells`, `kSwellShortCells`,
`kLatticeWrap`) - lets the vertex
stage displace *the same* waves the pixel shader lights, from one shared constant phase. A
displaced crest and its highlight cannot drift apart, distant water keeps its large forms
(only the chop fades with the pixel footprint), and dropping the second `noiseGradient` call
paid for most of the new shading. `src/Water/WaveField.h` documents the wrap arithmetic;
`tools/WaterRegression.cpp` asserts it with a `static_assert` so changing a period without
changing the wrap fails the build rather than sliding the waves apart by eye.

### What the reference does that this pass cannot

The comparison implementation runs a Tessendorf FFT wave simulation and an advected foam state
texture on compute shaders, samples a mip-blurred reflection chain, and shades with full
GGX/Smith and an environment BRDF curve. D3D9 `ps_3_0` has no compute, no UAVs, no ping-pong
state without spending a render pass, and no ceiling on how many instruction slots the water pixel
stage may spend: `tools/ValidateShaders.ps1` compiles it, reads the count and annotates it on every
run, and lets it be (fxc /O3 measured the shader at 642 slots on Windows CI on 2026-10-14, at 713
on 2026-10-17 after the CSE pass, at **890** after round 5's quality ladder added the mid
octave and the shore work, and at **895** after the foam-checkerboard fixes - the estimate for
round 5 was "roughly eighty" and the measured
answer was +177, under the same explicit 2048 cap; it had
been held to 621 - 597 measured plus 24 of compiler margin - until that ceiling started deciding
which waves were allowed to shade, which is the wrong job for a budget, so round 4 retired it rather
than re-negotiating it, and the cap came back in 2026-10-17 at three times headroom, now about 2.3x).
The count is still the number to argue about, and the reason to keep measuring
at all is Round 3: it estimated 608, the shader measured 633, and instead of a zero-margin budget or
cut visible shading the broad warp moved to the vertex stage and is interpolated to pixels - the slot
count came down by changing *where* the work happens, not by removing work. Earlier rounds, the device
fallback, and why the volume and cloud raymarches *are* still capped, are recorded under
"Shader budget raised for the volume rework" below). So each of those
has a substitute here: an analytic multi-train swell plus bounded value noise for the simulation,
a roughness mix toward the sky for the blurred chain, and a shaped-exponent lobe whose exponent
carries both the roughness and the sun's height for GGX with Smith visibility - a low sun smears the road
wide, a high sun keeps it tight, which is the behaviour the disc-widened lobe produced. The
SSR march takes the first convincing hit instead of maximising confidence per step, and the
sun-visibility march uses a single sided band. Both were measured against the frame budget,
not the slot budget: with `[loop]` the instruction count is static, so cutting *steps* buys
time per pixel and nothing at all in slots - which is worth remembering before trading steps
away again.

### Authoring

Every value above is a `[Water]` key. Explicit `[Global.Water]` overrides take priority;
otherwise values are read per area first (`LocationGraphics.ini`, the same
scope the enable switch uses), so zones differ without a code change: `ClarityPercent`,
`WindStrengthPercent`, `SparklePercent`, `WaveRoughnessPercent`, `SheenPercent`,
`ShoalFoamPercent`, `FoamBrightnessPercent`, `SunDiscPercent`, and the three colours
(`WaterColour` multiplies the zone's own painted water with `0x808080` neutral, `DeepColour`
is the asymptote, `AbsorptionColour` is the per-channel rate). Colours are the only values
that need text rather than an integer, so `ReadTuningColour` parses `0xRRGGBB` or decimal in
this header rather than growing a colour type in the shared tuning reader. `WATER CLARITY`,
`WATER WIND / FOAM BIAS` and `SUN SPARKLE` are on the F7 overlay; the rest are file-only for now.

### Sea state: zone, shared wind and weather

`renderer::water::TargetSeaState` in `src/Water/WaterSeaState.h` is portable
policy shared by the DLL and reference preview. The live path now consumes
[shared dynamic wind](wind-system.md); the existing preview scenes remain
fixed, unlinked references unless a shared-wind input is supplied explicitly.

- `WaterAgitationMultiplier` in **EnvironmentProfiles.ini** describes exposure.
  All 414 verified roots carry a value (see the zone audit): open seas and coasts
  are rougher than sheltered ponds. `ZoneWaves=0` substitutes 1.0 without
  disabling wind. The accepted authored range remains 0–4.
- Shared wind owns the direction and its weather/time-dependent strength.
  Native type and authored intensity, scaled once by the zone's
  `WeatherIntensityMultiplier`, feed wind and cloud coverage at the same frame
  boundary. There is no verified server-side severity signal.
- With shared wind disabled or `AffectWaves=0`, water uses its legacy fixed
  direction and direct native-weather sea response.

`[Water] WeatherResponsePercent` (0–200, default 100) controls departures from
reference wind energy when linked, and the native-weather push when unlinked.
`WeatherEaseSeconds` (1–120, default 14) is the response time to about 95%, with
clearing twice as slow. Zone exposure and the per-body `WindStrengthPercent`
foam bias remain independent authoring layers.

For linked water, let `air = clamp(1 + (sharedStrength - 1) * response, 0, 5)`
and `forcing = max(air - 1, 0)`. Strength 1 is neutral. With response 100% and
wind at 0, a little residual swell survives; with response 0%, authored swell
remains, but **neither case creates wind-driven foam without actual wind**.
The native-weather push is not added again on top of the already
weather-modulated wind.

| Output | Linked formula / destination |
| --- | --- |
| `agitation` | `clamp(zone*(.2+.8*air), .12, 2.6)`; wave/swell/geometry amplitude and foam energy |
| normal response | `NormalPercent * clamp(sqrt(agitation), 0, 2.5)` |
| `shear` | `clamp(.25*forcing + (agitation-1)*.12, 0, 1)`; integrated cross-current |
| `wind` | `clamp(authoredWind*sharedStrength*(.45+.55*agitation), 0, 2.5)`; shader foam/roughness gate independent of the sea-energy response |
| `motion` | `clamp(1+(agitation-1)*.55+.8*forcing, .08, 3)`; future phase velocity |

The unlinked model retains `agitation = clamp(zone*(1+.95*push), .12, 2.6)` with
`push = response * localWeatherAmount`, `forcing = push` and `air = 1` in the
other formulas (including its legacy wind/foam gate). A response of zero keeps
the zone's authored energy in either mode; direction is separately controlled
by the wind link. An authored foam bias of zero still suppresses wind-derived
foam.

`WaveMotion` now integrates **each** swell/chop/shear/UV phase in double
precision, wrapping each in its own period. Speed or shear edits never multiply
a new value into the whole session's elapsed time. Turns use a local world
pivot and compensate the swell, warp and grouping phases together, rather
than spinning the sea around the world origin. Every draw/reflection camera
continues to use its own rigid model/view anchor. The geometry and pixel stages
receive the same effective basis, wave size and phases. The shared-wind change
itself did not require a shader ABI or instruction-count change.

Wind, sea state and phase history survive device reset and tuning reload.
A frame delta is bounded to 0.1 seconds; invalid deltas cannot poison shader
constants. `WaterEffect.log` still reports eased sea state every 600 frames;
**F7 → WIND → WIND DEBUG VIEW** reports the shared input and linkage.

On the F7 Water tab the relevant controls are `WAVE SPEED`, `WIND / WEATHER
RESPONSE`, `PER-ZONE SEA STATE`, and `WATER WIND / FOAM BIAS`. Wind direction,
strength, gusts, timing and link switches live in the separate **WIND** tab.
Both use the existing location save/reload mechanism.

`tools/WindRegression.cpp` covers all eight direction signs, shared weather,
energy response, phase continuity, far-origin turns, reflected/different camera
anchors, frame rates, controls, stalls and long sessions. Water and cloud
regressions retain their geometry, shader/CPU, climate and exposure checks.
Final visual acceptance still requires the actual client.

### Constant ABI

The pixel shader's water block grew from twelve registers to seventeen (`c212` world-up and
wind, `c213` body style, `c214` water tint, `c215` deep colour and foam brightness, `c216`
absorption colour and sparkle gain) and the vertex stage's from two to three (`c202` carries
the swell phase). `watereffect::Scope` saves and restores all of it. The client's own water
constants keep their old float indices, which is what lets `tools/WaterDeviceRegression.cpp`
read `c[13]`, `c[16]`, `c[19]`, `c[20]`, `c[23]` and `c[28..30]` as before; those assertions
are the
cheapest available proof that the ABI did not shift.

Round 3 kept the register count and rearranged what the wave registers mean, because the field
itself changed from "a phase and a rotation" to "a phase per train":

| register | contents |
|---|---|
| `c200` (VS) | amplitude in yards, lateral slide in yards, sigma, one cell in yards |
| `c201` (VS) / `c209` (PS) | the world-to-lattice rows, identical in both stages |
| `c202` (VS) | one phase per swell train, in cells along that train, wrapped into its own wavelength |
| `c200` (PS) `.x` | sigma: the Gerstner pinch the pixel stage divides its slope by |
| `c210` | chop phase (xy), broad foam-warp phase (zw) |
| `c211` | texture creep (xy), quality level (z), waves gate (w) |
| `c212` | this draw's world-up in view space (xyz) and its authored wind (w) |

The vertex stage's `c5..c8` model-view matrix is read back on the CPU for the per-draw coordinate;
the phase uses the current global VIEW camera when its basis matches, so an object/tile translation
is not mistaken for the camera origin. The draw's up axis still comes from its own matrix. The
shader constant save/restore is unchanged in size - no additional vertex or pixel registers were
needed for world anchoring.

Round 4 grew both stages by the minimum: the vertex stage gained `c204` (the grouping field's
camera anchor) and the pixel stage `c217` (the four shading-only trains' phases, in one
contiguous `float4`). The scope now saves and restores eighteen pixel and five vertex registers,
`controls` is 72 floats and `vertexControls` is 20. **No interpolant was added, because there was
no room for one** - see the register note below.

| register | contents, as of round 4 |
|---|---|
| `c200` (VS) | amplitude, slide, sigma, one cell in yards |
| `c201` (VS) / `c209` (PS) | the world-to-lattice rows, identical in both stages |
| `c202` (VS) | xy = the phases of the two displaced trains; mirrored in PS `c210.zw` for ABI stability |
| `c203` (VS) | xy = the domain warp's camera anchor, wrapped in the warp's own period |
| `c204` (VS) | xy = the grouping field's camera anchor; never reaches the pixel stage |
| `c210` (PS) | chop phase (xy); displaced-pair phases remain mirrored in zw but are no longer re-evaluated by the PS |
| `c211` (PS) | texture creep (xy), unused (z), the wave gate (w) |
| `c217` (PS) | phases of shading-only trains C–F |
| `c218` (PS) | phases of shading-only trains G–H |

The warp value and its gradient arrive as `TEXCOORD7` (a `float4`: xyz the sample, w the
grouping gain) and `TEXCOORD4` (a `float4`: xy the lattice coordinate, zw the grouping gradient).
The pixel shader samples no low-frequency noise at all, which is what keeps a set nailed to the
terrain for one multiply and one `dot`. The performance pass widened the two existing native UV
values from `float2` to `float4`: `TEXCOORD0.zw` carries A/B sine and `TEXCOORD1.zw`
carries A/B cosine, allowing both pairs to normalize in vector form. It did not add an Output
field, so the eleven-VS-output /
ten-PS-input ceiling is unchanged. The current water block remains `c200..c218` (19 registers),
and the five vertex controls remain `c200..c204`; only the state transaction now saves the full
`c180..c218` pixel span so local lights and controls cross the driver together.

That packing is not tidiness, it is the only option. Without a register declared as `psize0`,
`vs_3_0` gives the vertex stage eleven `o#` registers and `ps_3_0` accepts ten `v#` ones - this
Output struct was already at eleven fields, so a twelfth interpolant is not "a bit more bandwidth",
it is `error X5622: Too many output registers declared (12)` and `error X5300: Invalid register
number: 10` from fxc, which is exactly how round 4's first version failed CI. Both stages were
already carrying a `float3` and a `float2` whose spare lanes could hold the envelope, so the
envelope costs nothing. `tools/WaterRegression.cpp` now counts the Output fields and the pixel
stage's parameters and refuses any other number, because that limit is invisible in the source
and absolute at compile time.

### Looking at changes without a GPU

`tools/WaterPreview.cpp` is a CPU mirror of the water pixel shader - the same wave field from `src/Water/WaveField.h`,
the same hash from `src/Water/WaterNoise.h`, the same refraction, absorption, in-scatter, reflection, foam and alpha
terms in the same order - that renders a PNG with `g++ -O2 -std=c++20 -pthread`. Every tuning decision in this round was
made by looking at its output first, because the shader itself can only be compiled in CI.

```sh
g++ -O2 -std=c++20 -pthread tools/WaterPreview.cpp -o build/waterpreview
build/waterpreview --out Screenshots/water-forever-look.png --grid 2 --supersample 2
build/waterpreview --out build/sea.png --scene sea --wind 140 --sparkle 150 --stats
build/waterpreview --scene coast --debug 2
build/waterpreview --scene lake --probe 240 260
build/waterpreview --out build/weather.png --matrix --scene sea --grid 2 --supersample 2
```

`--grid 2` renders the four built-in bodies - a slow river, an open sea at a low sun, a windward coast with a headland,
and a moonlit lake - into one contact sheet, each panel labelled with the wind, clarity, swell and sun numbers it was
drawn with, so a frame cannot be mistaken for another one later. `--debug 0|1|2` is the same channel numbering as
`[Water] WaterDebugMode`, so a frame from here can be read against a client log. `--no-reflect`, `--no-refract`,
`--no-depth` and `--no-foam` zero the corresponding strengths the way the ini keys do, which is how a term gets proved
to be the one responsible. `--probe x y` prints every intermediate for one pixel: view depth, column, transmittance,
absorption, Fresnel, roughness, reflection hit, foam, alpha, the wave normal and whether the result is NaN.
`--supersample 2` renders each panel at double size and resolves it linearly, which is the only honest way to judge the
octave the anti-alias gain exists to resolve, and it is what a 2x backbuffer does in the game.

`--matrix` draws one sea four times - a sheltered zone and an exposed one, each in calm air and in a
rainstorm - which is how the sea state gets looked at without a client; `--agitation 1.45 --weather
rain --weather-strength 95 --response 60` set the same four numbers by hand. The panels share a
clock and a camera, so anything that differs between them is the model and not the moment. The
preview calls `TargetSeaState` itself rather than copying its arithmetic, and the labels print the
zone's authored value and the sky as inputs rather than as results. The four-body sheet above is
byte-identical before and after this feature, which is the same identity the game relies on.

The order of the two passes is the point of the file. Pass one produces a pre-water scene copy with a view-depth
buffer; pass two shades water against that buffer. A previewer that shaded water in isolation would not see a raw sky
painted as refraction, a refraction pulled across a foreground silhouette, or foam on the wrong side of a shoreline -
and those are the defects this feature has historically had.

It is a mirror, not the game, and three differences are structural. There is no driver filtering, mip selection or
sRGB round trip: the DLL samples captured bytes, here the copy is computed and encoded once at the end. There are no
client detail maps (the 512 ripple and surface textures): the analytic chop stands in for both of the shader's octaves
and the painted surface tone is a flat colour, so a near field reads smoother and brighter than the shader's and
absolute levels are not comparable - structure is. There is no vertex stage: geometry waves displace the surface by
evaluating the same sine pair per pixel instead of interpolating it across a tessellated grid.

What it found in its first three renderings were two defects that no portable test could have caught:

- In-scatter used `(1-g^2)/den^2` with a `.5` gain - a Henyey-Greenstein phase function with the wrong exponent and no
  normalization, so a face turned toward the sun multiplied the water body by about nine. An open sea rendered as one
  flat pale wash. It is now `(1-g^2)*.17/(den*sqrt(den))`, the lobe's own 3/2 exponent with `1.2/4pi` folded into the
  gain: a forward peak near 1.3 and a side lobe near 0.08.
- The sun glint had no compensation for its own width. `pow(NoH,n)` integrates to `2*pi/(n+2)`, so as `n` rose the
  road dimmed, and with these slopes it was simply absent: no sparkle anywhere in any frame. The term is now
  multiplied by `(n+2)` and the output gain `.62` became `.0992`, which is the same fold.

Both were found by looking at a picture, and neither changed a line of the CPU-side code that the regressions
exercise.

`tools/PreviewSheet.h` holds the PNG writer and the 5x7 label font that both previewers use; `tools/CloudPreview.cpp`
writes byte-identical output through it. The committed sheet is the tool's own PNG re-encoded losslessly - same pixels,
a tenth of the bytes - and the renderer is deterministic, so the command above reproduces it exactly.

### Verification

Portable regressions cover the wave field, the material gate, the shared fog register and the two
look normalizations as shader text, and the device test renders the complete shader on the software
HAL. The numbers behind the look - how much a crest lights up, how fast red dies per yard, how
bright the disc is - were set against `tools/WaterPreview.cpp` above, which says more about them
than a reference screenshot can and less than an in-game pass does: the frames carry no client
detail maps, no mip filtering and no vertex stage. The final check stays the F11 capture plus an
in-game pass. The
slot count, the MSVC build and the device test all run on the Windows CI job; this environment
cannot run fxc, so every **slot measurement** in this document is a CI result, not a local one. The
water pixel stage carries no ceiling any more, which puts the constraint on the measured number
itself: the count is annotated on every run, so a change that moves it by tens of slots is something
a reviewer reads and weighs, in the pull request that made it, rather than something a stale number
decides. In-game waves, transitions and foam still require a live-client pass.

## Full rework round 3: motion, mass and foam (2026-10-14)

Round 2 fixed what the water looked like. Round 3 fixed what it did. The report from play was
three things, in order of how bad they were: the waves moved when the camera turned and slid when
the player moved; they were far too fast, "insanely fast" at the shipped numbers; and they were
flat - wave curves drawn on a plane, with no body to them. Foam on breaking crests was the ask
that came with the fix, and the round-1 requirements had to survive it: per-zone sea states and
weather-driven roughness stay, because a calm zone and a storm are authoring, not defects.

### Why the sea followed the camera

Two transpositions, not one. The vertex stage built its world axes by reading the draw's
model-to-view matrix ACROSS its rows - `float3(modelView[0].x, modelView[1].x, modelView[2].x)` -
which is the view's own axes expressed in world coordinates. Dotting those with a view-space
position applies the camera's rotation twice, so the lattice coordinate turned when the camera
turned: the sea behaved like a decal on the screen. The array is the client's row-vector matrix,
so world axis *i* in view space is row *i*, exactly the convention the stage's normal transform
had always used a few lines below the bug.

The second half was the anchor. The camera position that the wave phase carries used to come from
`FrameContext`, which `CameraCapture` fills on the first draw of a frame that carries perspective
constants - so a water tile drawn before that capture was anchored to last frame's camera, and the
sea slid backwards by exactly the distance the player moved in between. The client's own water
reflection pass is worse: it renders water under a mirrored camera the capture never sees at all.
Both halves now come from the current draw without confusing a tile origin for world origin:
`watereffect::Scope` reads `c5..c8` and the live `D3DTS_VIEW` transform. `AnchorForWaterDraw` uses
the global VIEW camera when its basis matches modelView (including translated water tiles), while
retaining the conservative local anchor for a genuinely transformed basis. This avoids a stale
`FrameContext` camera and avoids cancelling each tile's world translation. Regression tests construct
yawed, pitched and translated-tile matrices, then require the shader's relative coordinate plus the
selected phase anchor to recover the same global world point.

### Why the sea was fast

`kLatticeRotation` rotated the lattice basis by `time * 0.035` radians per second. Rotating a
basis about the world origin translates every point that is not at the origin: at a camera six
hundred yards out that is 175-700 yards per second of sea sweeping past, while a camera at the
origin saw nothing wrong - which is why it survived two rounds of looking. The orientation is a
constant now (`kLatticeYaw`, 0.62 rad), and the pace a crest travels at is a property of its
wavelength instead: `c = 0.44 * sqrt(10.73 * wavelength / 2pi)` yards per second, deep-water
dispersion scaled to something a player reads as water. The forty yard swell crosses at about
3.6 yards a second (an eleven second period), the ten yard chop at 1.8. `FlowSpeedPercent` still
scales all of it, through the integrated sea clock, and weather still quickens it - both
preserved, neither re-multiplied into the field.

### Why the sea was flat

The displacement went along `float3(modelView[0].z, modelView[1].z, modelView[2].z)` - the view
direction, not an up axis - so geometry waves pushed water toward and away from the camera and the
silhouette could never change. Water now lifts along world up and slides toward the coming crest,
which is a Gerstner wave: the slide is the term that makes crests pinch and steepen and troughs
open out, and it is why the same amplitude reads as a body of water instead of a sine seen from the
side. The mesh carries the two longest trains (the client's liquid grid is about four yards apart
and a ten yard wave on it is facets); the pixel stage shades all three and divides its slope by the
Gerstner Jacobian, so a crest the geometry steepened is a crest the light sees steepened. The
steepness is authored once (`sigma = 0.45`, clamped fold-safe on the CPU), so no combination of WAVE
HEIGHT, WAVE SIZE and sea state can turn a wave inside out - `J` never drops below 0.46, and the
shading floors it at 0.3 besides.

That also caught a sign error that was invisible while the surface was flat: the shading normal was
tilted by the POSITIVE gradient of the height field. A height field's normal is `(-dh/dx, -dh/dy, 1)`,
so every wave face was lit as though it leaned the other way and highlights sat in the troughs. Both
stages and the CPU mirror now use the negative gradient, and the regression pins the sign in all
three.

### Foam on the edges of waves

Foam used to be a shore term plus a crest mask. It is now three sources with one gate: the crest
itself (`crestMask`), the pinch where the geometry is folding water onto water (`breaking`, which is
zero on water that was authored flat - so a calm zone shows no white line however high its wind),
and the shoal where the bottom comes up fast. All three are multiplied by the body's authored WIND
and by its sea state, so per-zone and weather behaviour is unchanged in kind and stronger in effect:
a squall on an exposed coast puts a cap on every third crest, a sheltered pond in the same rain
stays dark water with a little chop. Foam is a surface, not a decal: it raises the roughness, takes
the reflection (`*(1 - foam*0.85)`) and the sun road (`*(1 - foam)`) with it, and only then adds its
own colour, lifted toward the light but softened at `NoL = -0.25` so a shaded face still carries its
cap.

The top-down screenshot showed both polygonal seams and camera-relative foam movement. The depth-based
shore estimate was one source: the old shader derived bed slope from `ddx/ddy` of view-Z depth and a
projected pixel footprint, so camera pitch and depth precision changed the foam boundary. The shader
now reconstructs the seabed point on the sampled ray, uses its vertical separation from the water
surface for shore/backwash depth, and derives slope from the reconstructed 3D normal relative to this
draw's world-up vector. Optical column depth and refraction remain in view space.

A second source was the coordinate anchor. A native c5..c8 model-view matrix can contain a liquid
tile's world translation; deriving the camera phase from that matrix cancels the tile origin and
restarts noise at each tile. The draw now combines its camera-relative vertex coordinates with the
live D3D view matrix's global camera position when the bases match, so translated world-aligned tiles
share one phase without borrowing a stale frame camera. Finally, the broad foam warp is sampled at
pixel positions rather than linearly interpolated across mesh triangles, preventing visible facets in
large bright patches. The footprint fade is retained only as anti-aliasing LOD.

### Settings, and what a player sees

`GeometryWavesEnabled` ships on now, with `[Water] GeometryWavePercent` re-authored as a height in
percent of the shipped sea: 0 is a flat sheet, 100 the reference look, 300 a gale. The F7 menu
gained WAVE GEOMETRY and WAVE HEIGHT (0-300, step 5) next to WAVES and WAVE SIZE. Turning geometry
off does not flatten the shading - the crest sharpening and the whitecaps stay, because those are
properties of the sea and not of the mesh - and turning WAVES off still gives a flat, unsharpened,
foamless sheet, exactly as before.

The published frames in `Screenshots/water-forever-look.png` and `Screenshots/water-zone-weather.png`
were re-rendered with this round's `tools/WaterPreview.cpp`, which now displaces the shaded point by
the same Gerstner lift and slide (forward-displaced rather than inverted, the one named difference
it adds to its list).

### Verification

`tools/WaterRegression.cpp` gained the statements a screenshot cannot hold: the lattice keeps one
orientation under any time and wave size; the long swell crosses between 2 and 6 yards a second;
every train lands the same wave under every camera from its own wrapped phase; a draw's matrix plus
the coordinate the vertex stage computes from it is the world point, at every yaw, pitch and mirror;
the up axis converts back to world +Z; unfilled, scaled, sheared and NaN matrices are refused; and
across every height/size/sea-state combination the steepness stays under the fold, the lift stays
under 1.2 cells, and waves-off is exactly flat. Both shader stages and the CPU header are pinned to
the same train table, the same transposition-free axis reading, the same negative-gradient tilt and
the same foam gate, so a fix cannot be half-reverted. `tools/WaterDeviceRegression.cpp` pins
`GeometryWavesEnabled=0` in its isolated settings and asserts it after the reload, because its quad
sits exactly on the far clip plane and a displaced vertex would be a clipped pixel; its sentinel
`c200` moved from `{71,0,73,74}` to `{0,0,73,74}` because `c200.x` is the amplitude now.

The pixel stage was estimated to grow by roughly forty instruction slots (a third train, the pinch
and Jacobian, the whitecap mask, the back-lit in-scatter, the foam coupling, minus the flow gate
and the crest slope dot it replaced), so the initial budget moved 560 to 608 without fxc. The next
Windows run measured 633, not 608. Rather than accept a zero-margin increase or remove visible
shading, the broad domain warp and its gradient moved to the vertex stage and are interpolated
through `TEXCOORD7`; the liquid mesh resolves this 35-yard-scale field smoothly, while the fine
chop remains per pixel. That kept the same warped crest and chain-rule normal and measured **597**
slots. `tools/ValidateShaders.ps1` budgeted 621 for it at that point - the 597 measurement plus
24 slots of compiler margin - a ceiling round 4 replaced, below.

## Full rework round 4: the comb, and the sets (2026-10-14)

The report after round 3 was not "the water is flat" any more. It was: *the waves still go in
lines - I can clearly see line by line of waves going and going*, with a repeating pattern a
player can count. Round 3 had fixed the sea following the camera, its pace, and its mass. It had
not fixed the shape of the surface, and the reason is worth writing down because it is not
obvious from a screenshot.

### What was actually being seen

A probe along a screen row in the mid-distance measured crest after crest at a spacing of exactly
forty yards - train B's wavelength, in the 40/20/10 table - in perfectly straight lines, and the
blob that the domain warp produced travelled with the camera. Three things produced that, in order
of how much of the complaint each one owns:

1. **Three sinusoids are a rhythm.** With weights 1/.5/.26 the forty-yard train carries the
   silhouette, and the pixel stage is the only place the ten yard train exists at all: the client's
   liquid grid is about four yards apart, so the vertex stage displaces the two long trains and the
   short one contributes nothing to a crest. Two displaced trains a factor of two apart realign
   every other crest, which IS a comb. The far field was worse than a comb: the chop's
   `filter=rsqrt(1+footprint)` term fades the six-octave noise to almost nothing at distance, so
   distant water was ONE sine at 40 yards with nothing on top of it. Straight, evenly spaced,
   repeating: exactly the report.
2. **The warp was too weak to bend a comb.** 3.2 cells of displacement at a 1/14-cell (35 yard)
   scale bends a forty-yard crest by a few yards over its own length - visible as a wobbling line,
   still a line. A wave that keeps its wavelength is a wave that keeps its spacing, however much it
   curves.
3. **Nothing decided where the waves were.** The amplitude was the same in every square yard of
   the body, so the pattern had no reason not to repeat: a real sea arrives in SETS, five or six
   tall crests and then flat water, because a band of wavenumbers carries its energy in a
   modulated envelope.

### What changed

* **Eight trains, in a table, with a start of their own.** Wavelengths 16, 11.5, 8, 5.8, 4.4,
  3.4, 2.55 and 1.95 cells (40 down to about 5 yards at the shipped wave size) - a ratio of
  about 1.35 between neighbours and never an integer one - in eight directions whose crest
  lines are never closer than 18 degrees, with weights 1/.62/.34/.19/.12/.075/.052/.036 falling
  off so that the summed SLOPE stays at the value the shading's Jacobian was authored against,
  and a fixed phase offset per train (`.37`, `.71`, `.19`, `.55`, `.83`, `.63`, `.29`, `.63` of
  that train's own wavelength) so no two crests cross lattice zero together. Two of the eight
  are displaced, all eight are shaded, exactly as before: the mesh budget did not change. The
  two shortest trains are the newest addition (October 2026): with only six, the surviving
  crest families regrouped into lines at range, because nothing crossed the coarse bands at
  the scale the eye resolves far away; the pair at 65 and 128 degrees crosses them there.
* **One stronger warp instead of two weak ones.** `.05` frequency (one sample per 50 yards) and
  `6.5` cells of amplitude (5.0 until October 2026), displacing the crest ALONG the field. Six
  and a half cells of bend on an eleven-cell wavelength moves a crest twenty yards sideways and
  back, which stretches the spacing here and doubles crests up there: a bent train has no
  constant wavelength left to read as a rhythm. The second decorrelated warp and the vortices
  from the previous round were cut - they could not break a comb and they cost slots to prove it.
  A second, finer chop octave rides the same wind from October 2026 - sampled half an octave
  above the main chop and advanced against the swell's own phase - because at distance the
  coarse chop was the last regular pattern left standing.
* **A grouping envelope.** A third world-anchored noise field at one sample per 115 yards produces
  a gain in `.52 .. 1.14`, and it multiplies the lift, the Gerstner slide, the shading slope, the
  pinch, the chop riding on the swell and therefore the whitecaps. It is sampled ONCE, in the
  vertex stage, and handed to the pixels in the spare lanes of `TEXCOORD7`/`TEXCOORD4` with its
  gradient (there is no twelfth interpolant register to spare). The slope carries
  it by the product rule - `dh = gain*dswell + swell*dgain` - so the flank of a set tilts the
  water as well as dimming it, which is the difference between waves that fade out and waves that
  run out. A ceiling just over 1 is deliberate: every fold-safe clamp, foam threshold and crest
  mask is authored against the table's own maximum, and a gain that only ever goes down cannot
  invent a second sea state in a sheltered bay.
* **`detail.x` (the foam pattern) is NOT enveloped,** and the authored swell response
  (`foreverStyle.x`) is applied BEFORE the gain: it is a slider on the wave, and differentiating an
  envelope that already carried the slider would scale the slider by `dgain` too.

What did not move is the core the task asked to keep: one world-anchored lattice, phases computed
in double on the CPU from the draw's own matrix, per-train deep-water dispersion, the same analytic
trains displaced and shaded, the client's detail map creeping underneath, and the Gerstner slide
that makes a crest read as a body.

### Registers, the slot count, and the interpolant ceiling

The pixel stage grew one register (`c217`, the four shading-only trains' phases in one contiguous
`float4` rather than gathered across three) and the vertex stage one (`c204`, the grouping field's
anchor), so `watereffect::Scope` now saves and restores 18 pixel registers and 5 vertex ones. The
`controls` array is 72 floats and `vertexControls` is 20. October 2026 added `c218` for the two
shortest trains' phases - a third `sincos` batch costs less than regathering their projections
from the two registers beside them - taking the save/restore window to 19 registers and
`controls` to 76 floats.

Before this round the pixel shader measured 597 slots of a 621-slot budget, and the estimate in
this document was "about ten slots higher". The measured answer was **642**, and getting there
taught two things that are worth keeping because neither is visible in the source.

* The first Windows run did not fail on slots. It failed on interpolants: with the envelope added
  the vertex stage had twelve outputs and the pixel stage eleven inputs, and with no register
  declared `psize0` vs_3_0 offers eleven `o#` registers and ps_3_0 accepts ten `v#` ones -
  `error X5622: Too many output registers declared (12)`, `error X5300: Invalid register
  number: 10`. Nothing was dropped to fix it: the gain rides `waveWarp.w` and the set's gradient
  rides `worldRel.zw`, spare lanes of registers already travelling to the pixel stage for other
  reasons. `tools/WaterRegression.cpp` now counts the `Output` fields and the pixel stage's
  parameters and refuses any other number, because a limit that only fxc knows about is a limit
  that gets discovered an hour later in a Windows job log.
* Six trains cost 45 slots, not ten (eight, with the third batch, cost more again - see the
  fxc count in CI). Two vectorised `sincos` batches, a chain rule and a
  differentiated envelope all fold into one instruction per lane, so the price per train is
  invisible to a reader counting terms in the source. The estimate was not paranoid enough. The
  four short trains' slope terms remain the first thing to cut if this shader ever needs to
  shrink - the heights matter more to the silhouette than their exact slopes do - and the
  envelope is not on that list, because it is the one addition that no train can substitute for.

The ceiling then went away, which is a decision rather than a measurement: the sea is not asked
to shed visible features to keep the shape the shader had in October. `source` moved out of
`$budgets` in `tools/ValidateShaders.ps1` and into `$measureOnly`, where fxc's count is still read,
printed and annotated on every run - and still fails the job if it cannot be read at all. The other
entries keep their budgets, because a raymarch loop that unrolls past what a driver accepts is a
different failure from an expensive line of shading.

What replaced the ceiling is the review, and it is worth being blunt that this is now a weaker
guard than the number it replaced: the protection against a 900-slot water shader is a human
reading an annotated count, so the number above is the thing a change has to answer for. The
hardware limit was never the budget anyway - it is `MaxPixelShader30InstructionSlots`, and a card
under it keeps native water because `CreatePixelShader` fails.

### Verification

* `tools/WaterRegression.cpp` holds the table to the properties that make it a sea: wavelengths
  strictly decreasing and never an octave or a unison apart, no two trains parallel IN LINE SPACE
  (a crest line has no direction, so the test takes the cross product and checks it modulo a half
  turn), weights falling, each start inside its own train's wavelength, and every literal the
  shaders spell equal to the header's - including the `0.01347826` in both stages being exactly
  `kGroupFrequency * kGroupDepth`, and the fold clamp being written against `kGeometryPinch *
  kGroupMax` rather than the unhistoried `kMaxPinch`. The first version of the direction table put
  the two shortest trains at -71 and 104 degrees, which is five degrees apart in line space - one
  comb with two teeth - and that test is what caught it.
* The CPU mirror (`tools/WaterPreview.cpp`) carries the same warp, gain, product rule, pinch and
  chop scaling, so the published frames are of the water the client draws. Before/after frames at
  the same camera (`build/look-before.png`, `build/look-after.png`, not committed) show the
  mid-distance changing from evenly spaced parallel bands into crossing crests at several angles
  with visibly grouped heights.
* "Waves in lines" is now a measured regression, not an eyeball claim: `WaterRegression` walks
  the crest line of the dominant train (where a lone sine is exactly constant) and requires the
  field to vary along it by a real fraction of how much it varies everywhere (0.71 measured,
  0.30 required, 0.00 for a lone train). Crests that march cannot pass it; crests that grow,
  double and die do. Per-zone colours are resolved beside the agitation profile they already
  had - `WaterColour`/`DeepColour`/`AbsorptionColour` in `EnvironmentProfiles.ini`, with
  `[Area.35]` Booty Bay authored azure over its teal Stranglethorn zone - and
  `tools/CloudClimateRegression.cpp` holds the layering (area over zone, unauthored zones keep
  the global `[Water]` colours, borders blend instead of stepping).
* Still open, in `TODO.md`: the `fxc` slot count, and a look in-world - a CPU mirror can prove the
  numbers agree and cannot prove the water looks right at 40 feet above a harbour.

## Shader budget raised for the volume rework (2026-10-14)

The first Windows run of the volume shader failed to compile (`mix`, a GLSL spelling - now
guarded portably by `tools/ShaderSourceRegression.cpp`), and the second failed the budget:
**543 instruction slots against 496** (542 now, since the doubled tint below came out of the
shader). 543 is above the 512 instruction slots that a `ps_3_0` *device* is required to support,
which is what the 496 budget existed to stay under, so this was a decision, not a tweak.

That is worth stating precisely, because the number appears in three places and the reason for it
is easy to over-read. 512 is not a limit of the `ps_3_0` bytecode format and it is not a limit the
client imposes. The pixel shader model 3.0 hardware requirement is that a device support *at least*
512 slots, and a device may report more, up to 32768, in `D3DCAPS9.MaxPixelShader30InstructionSlots`.
It is that per-device number which `CreatePixelShader` is accepted or rejected against. fxc here
assembles the shader without a word of complaint, which is the proof that the format is not the
bound. Nor is the game's own shader budget involved: `WaterEffect` compiles and creates this shader
itself, on the client's device.

The decision was made from measurements rather than from the plan's guess. `fxc` priced one copy
of the shader per candidate cut in a single run (the probe scaffolding is gone; these are the
numbers it reported):

| candidate cut | slots saved | what it costs |
| --- | --- | --- |
| screen-space reflection march | 69 | no scene mirrored in the water; sky reflection only |
| sun-road occlusion march | 51 | the sun/moon road stops being hidden by terrain and buildings |
| analytic swell train | 37 | the wave the vertex stage displaces and the light on it |
| Henyey-Greenstein scatter lobe | 17 | flat scatter; a crest stops glowing when backlit |
| reflected sky gradient and disc | 15 | flat fog colour reflected, and `SunDiscPercent` stops meaning anything |
| wave-lit body term | 13 | back to one lit term, so wave forms read flat |
| shoal-foam depth derivative | 11 | no foam where a river shallows over a bar |
| SSR steps 12 -> 8 -> 2 | 0 | nothing; those loops are `[loop]`, so the count is static |
| shared subexpressions | 6 | nothing; invisible tidying only |

Two of the plan's three named fallbacks turned out to be free: the SSR step reduction it put
first saves **zero** slots (a `[loop]` body is counted once, which is the same fact this file
already records about the frame budget), and the ISP isotropic scatter term it put second was
one of the larger visible savings. Nothing reached 496 without giving up a term the rework
exists to add, so the budget was raised to **560** and the trade is recorded at the number in
`tools/ValidateShaders.ps1`.

The consequence is explicit rather than hidden: on a device reporting
`MaxPixelShader30InstructionSlots = 512`, `CreatePixelShader` fails, `WaterEffect` logs
`water shader creation failed hr=... PS30 slots=512; keeping native water until device reset`
and the client's own water is drawn for that session. The fallback is clean in the sense that
matters: `Scope` has only *read* device state by the point it attempts creation - textures,
samplers, MRT bindings and constants are all touched after - so a device that refuses the shader
is left exactly as it was found, and `shaderCreationFailed` keeps it from trying per tile.
Water effects therefore require a GPU that reports more than 512 slots, which in practice means
anything later than the GeForce 6/7 and Radeon X1xxx generation the 512 minimum was written for -
the cap is a per-device number, and this runner's fxc is not the thing that gets to answer it.
That is a player-visible limitation and is in
`changelog.d`, not only here.

The alternative was considered and rejected: a `WATER_COMPACT` compile of the same shader text,
chosen when the device reports the bare minimum, keeping the body, tint, scatter, swell and foam
and dropping the two ray marches (69 + 51 slots) so that device class keeps new water instead of
native. It is a real option - the text is compiled at runtime by `D3DCompile`, so a second pass
with a `#define` is cheap, and `ValidateShaders.ps1` could enforce a second budget of 496 on it -
but it puts a second shading path into the file that every future water change then has to be
thought about twice, to buy back a look on hardware the 512-slot minimum was written for. Nothing
here forecloses it: the guards would go around the two marches and nothing else.

The budget still does its original job for this shader - growth must be argued in
a diff - and the other enforced budgets are untouched.


## The 2048 cap and the CSE pass (2026-10-17)

On explicit request the water pixel stage went back into `$budgets` in
`tools/ValidateShaders.ps1`, at **2048**: about three times the count it
actually uses, so the ceiling guards against a pathological edit (an
unrolled march, a forgotten constant array) instead of acting as a design
input on wave-train quality. `$measureOnly` is empty again; the device
fallback is unchanged - a card that reports fewer slots than the stage
needs fails `CreatePixelShader` and keeps native water.

The same request asked for the effect to be optimised as far as possible
without any visual sacrifice. The two raymarch loops (the eight-step SSR
and the eight-step light march) hold the bulk of the 642 measured slots,
and their step count *is* the quality: fewer steps is a visible change, so
they are untouched. What is removable without changing a single output bit:

* The wave offsets `wave*.15` (SSR) and `wave*.18` (light march) were
  recomputed on every step although the wave normal does not move inside
  either loop. Hoisting them out removes four multiplies per step of each
  march - 48 across both - and the hoisted product is the same
  instruction, so the values are bit-identical.
* `dot(wave,l)` was evaluated twice (the body modulation and the glint
  term). One `NoL` serves both.
* `max(lightColor.rgb,0)` was evaluated three times (in-scatter, the sky
  disc, the sheen colour). One `lightPos` serves all three.
* `dot(-v,l)` and `-dot(v,l)` were the same dot from two sign conventions
  (the HG phase denominator and the backlit gate). `dot(-v,l)` is
  `-dot(v,l)` bit-exactly - sign flips and the `ps_3_0` dot are exact - so
  one `vDotL` serves both, with the denominator written
  `1+g*g+2*g*vDotL`, which is the original expression to the last rounding
  step.

The estimate from that arithmetic is roughly 580 slots; the first Windows
run after this pass is authoritative, and any movement larger than the
estimate is visible in the Checks tab for the same reason every other
budget movement is. The look pins in `tools/WaterRegression.cpp`
(the normalized HG lobe, the glint's width compensation, the neutral-at-
0x808080 tint multiply) and the portable HLSL-spelling guard all pass
against the edited source, and none of the pinned texts changed.

## Additional safety sweep (2026-10-14)

Four further defects were found in code:

- All four draw hooks skipped water whenever the legacy highlighter was
  enabled/visible, even if it did not match or install a shader on the draw.
  They now skip water/fog only when that draw's highlight scope actually
  changed the shader. A matching highlight still has priority.
- Scene color was always sampled with sRGB decoding **off**. When the native
  color target enables sRGB writes, its copied pixels are already encoded;
  sampling them as linear and encoding again brightens refraction/reflections.
  The scene sampler now follows native sRGB output. The depth sampler always
  stays linear. Neither native output state nor caller sampler state is lost.
- `ShaderCache` keyed hashes by unowned COM addresses. A released shader's
  address could be recycled for a different shader but retain the old hash,
  affecting water recognition. Entries now own identities, with a 1024-entry
  LRU bound (important for short-lived debug shaders). Reset releases them;
  shutdown avoids COM releases under loader lock. Transient bytecode-query
  failures are not cached. Pipeline keys also include bytecode hashes so
  pointers recycled after eviction cannot resurrect old classifications.

- If camera capture was unavailable for the first world water draw, the scene
  copy could retry later in the same frame, after water was already in the
  scene. That feeds water back into its own reflections/refraction and gives
  different tiles different inputs. The first world water draw is now the
  capture deadline even if its camera is unavailable; the next frame retries.
  Offscreen draws still do not consume the world's capture opportunity.

- The per-body `WaterColour` multiply carried a `*2.0` gain. The tint is uploaded as the
  colour bytes over 128, so the documented neutral `0x808080` already arrives as 1.0, and
  the doubling put every authored colour a stop high and clipped the bright ones - the
  complete-shader sweep read a captured 128 gray back as 255. `tools/WaterPreview.cpp`
  carried the same gain, so the preview sheets the look was tuned against were tuned bright
  with it; the fix lands on both copies, and the two sheets under `Screenshots/` were
  regenerated from the corrected mirror. A portable pin in `tools/WaterRegression.cpp` now
  holds the shader and its CPU mirror to the one neutral multiply.

The device regression tests COM lifetime/eviction/retry deterministically
before HAL setup, actual highlighted/unhighlighted water pixels, and the
**complete production shader's** refraction with sRGB both off and on. For
128-gray scene color, both paths must produce 128-gray (±2), not approximately
188-gray from double gamma correction. State restoration is checked afterward. Capture scheduling tests also verify
that a late camera cannot copy an already-water-filled scene and that the
next clean frame can retry.
These are targeted regression tests, not a claim of an Ascension client replay.

## Follow-up: texture resolution was used as material identity (2026-10-14)

The shared material gate required slot 0 to be exactly 8×64 and slot 1 to be
exactly 512×512. It rejected a 256×256 or 128×128 surface before consulting
any shader identity, incrementing `frameCandidates`, creating the replacement
shaders, or capturing the scene. `GroundSurfaceCapture` duplicated that gate.
This is reproducible with the production policy and explains why successful
shader compilation cannot establish that a water draw reaches replacement.

Keep the canonical layout fallback for previously supported unknown tile
variants. For different resolutions, require **both** a documented native
water VS and a documented water PS, the lookup's base size or a valid reduced
top mip, and a square power-of-two surface. The shader roles are not
interchangeable. A ripple lookup alone (also used by weather) is insufficient.
Verify the actually bound shader pointers before using tracked hashes; when
state-block application bypasses hooks, query the bound shaders' cached hashes.
Classification, scene capture, replacement and ground capture now use one gate.

Portable tests cover all three documented VS and both PS (stage roles verified
from the pre-refactor gate at `11fd29e`), all lookup mips, surface sizes
16–1024, missing inputs, wrong shader roles, weather/UI and invalid shapes.
Device tests use synthetic ABI-compatible shaders **with supplied native
identity metadata** to exercise the real classifier, scene copy, replacement,
framebuffer readback and state restoration at six noncanonical layouts. They
also reject stale tracked identities. These tests are not native shader dumps
or an Ascension client replay. The reporting client's logs have no texture
sizes; this code defect is verified, but its relevance to that particular
client remains unconfirmed without an observed draw.

## Follow-up: live toggle blocked by startup configuration (2026-10-14)

The shader-budget fix did not address a separate enable path. `Configure` read
`[Water] Enabled` from `ModernWoWRenderer.ini` with a default of **0**, then
returned before loading tuning or compiling either shader. The F7 menu reads
and writes `GraphicsEffects.ini`/location overrides (default **1**), and calls
`ReloadTuning`, which only changed `effectEnabled`. Thus a stale startup OFF
or a missing startup key left `enabled=false` and empty bytecode even when
the menu showed ON. Water-only draw/depth hooks could also be omitted.

Water now prepares its backend at startup independently of the live toggle.
`enabled` means backend readiness; `effectEnabled` follows the same live key
and default as the menu. The obsolete startup water `Enabled` key is ignored.
Use `[Water] Enabled=0` in `GraphicsEffects.ini` (or F7) to disable water; F11
still controls the complete graphics package. OFF does not capture or replace
water draws, but leaves the backend ready for a later ON or location change.

The device regression now uses isolated settings with legacy startup OFF and
live OFF, rather than relying on the shipped all-ON files. It checks backend
preparation, menu Save/reload, missing-key defaults, location transitions and
master OFF before GPU setup (also on headless runners), then verifies native
OFF and magenta replacement ON pixels on a D3D9 device. This reproduces the
configuration failure path; it is not a capture from the reporting client.

## Previous shader-budget correction (2026-10-13)

## Why an enabled setting could still show native water

`WaterEffect.h` compiles HLSL at startup, but creates the D3D9 shaders only on
an identified liquid draw. A successful `D3DCompile` does **not** mean that the
player's device accepts the shader. The old water shader was documented at
1,552 instruction slots, versus the 512 slots guaranteed for PS 3.0. Failed
`CreatePixelShader` calls simply returned, leaving native water installed and
retrying on every water tile without logging the cause.

The replacement now uses real loops for SSR and light occlusion, a vectorized
noise hash, two wave octaves, CPU-side uniform rotation, and a single celestial
specular lobe. These keep waves, SSR, sky reflections, refraction, absorption,
foam, celestial glint, and local-light reflections in the same pass. Windows
shader validation measured it against a **621-slot budget** (597 slots with fxc /O3 plus 24 of
margin; the broad domain warp is vertex-sampled and interpolated to keep the pixel shader inside that
number without removing foam or its chain-rule normal). That budget no longer exists for water - the
round that retired it and the measurement that prompted it are described under "Full rework round 4"
above. Earlier shader-budget history, including why the original 496-slot ceiling moved, is recorded
above. Device-creation
failures log the HRESULT and device instruction
limit once, and retry after reset rather than on every tile.

This is a verified failure path in the code, not a diagnosis from a capture of
the reporting player's machine. In-game confirmation still matters.

## Other corrections

- **Independent switches:** scene/depth capture is requested by any of
  reflections, refraction, depth tint, shore foam, or sun-glint occlusion.
  Turning reflections off no longer removes the others. Water can borrow depth
  and camera capture when `[Volume] Enabled=0`, without enabling atmosphere.
- **No capture:** valid fallback projection constants prevent `0/0` and NaNs
  when INTZ is unavailable (notably with MSAA). Waves, environment reflection,
  glint and crest foam still render; screen-space features require capture.
  Above 1x the *colour* source is still captured - a multisampled colour target
  resolves through the `StretchRect` this was already making - and only depth is
  impossible. Anti-aliasing comes from the image pass instead:
  `docs/anti-aliasing.md`.
- **Refraction off really means off:** zero distortion and zero scene-color
  mixing. Absorption remains independent. Foreground depth is no longer treated
  as a deep seabed, and distorted samples crossing a foreground silhouette are
  rejected. Missing seafloor depth uses deep tint rather than refracted sky.
- **Material identity:** replacement, capture and classification share the
  supported two-texture signature (slot 0: 8x64; slot 1: 512x512). Classification
  tests this before its shader-pipeline cache so another material cannot poison
  the result. A ripple lookup by itself is not sufficient; it also occurs on
  weather. Unknown liquid layouts are intentionally not guessed at.
- **Transactional state:** query the actual bound shaders; restore constants,
  extra textures and samplers even after partially failed setup. Optional water
  MRTs are bound as a pair, validated for size/capabilities, and restore write
  masks and viewport. An unsupported R32F blend target must not break the main
  water draw.
- **Lifecycle:** recreate both water targets together on resize; expose only a
  freshly cleared pair; clear borrowed pointers at frame end/reset; reject
  offscreen scene copies. A failed first copy retries next frame, never after
  water has already entered the scene. Capture diagnostics no longer repeat every frame
  when the counters stop changing.
- **Atmosphere boundary:** water already has linear surface depth. Its projected
  boundary is normalized just like terrain depth, not scaled a second time by
  WoW's viewport MaxZ (commonly .94).
- **Animation:** elapsed time is relative to first use, without a discontinuous
  ten-minute wrap. Disabling waves also removes wave-driven refraction offsets.
  Crest foam strength no longer depends on the shore-foam strength slider.

## Tests

- `tools/ValidateShaders.ps1`: compiles the production VS/PS and fails if either stage does not
  compile, or if any *budgeted* shader - the volume and cloud raymarches, weather, and the water
  pixel stage (2048, see "The 2048 cap and the CSE pass" above) - exceeds its ceiling. The water
  pixel stage measured 642 slots on Windows CI on 2026-10-14, 713 after the CSE pass, **890**
  after round 5's quality ladder and **895** after the foam-checkerboard fixes; the vertex stage
  measures 188. Both counts are annotated on every
  run (the former 621 cap is described under "Full rework round 4").
- `tools/ShaderSourceRegression.cpp`: reads every raw HLSL literal in the tree and rejects the
  GLSL spellings fxc reports as undeclared (`mix`, `fract`, `mod`, `inversesqrt`, the GLSL
  texture lookups and type names), naming the line and the HLSL spelling meant. It runs in the
  portable job, because shader text was previously only ever compiled by fxc on the Windows
  runner - which is how `mix` reached the volume shader's sun glint and cost a CI run.
- `tools/WaterRegression.cpp`: portable material, independent-capture toggle
  combinations, and target-size policy tests (included in Linux CI). It also pins the packed
  long-wave dataflow, zero-contribution branches, profile-query cache, batched constant windows,
  volume-only MRT gate, and the in-scatter/sun-glint normalizations as production shader text -
  plus the look formulas in the CPU mirror - so an optimization cannot quietly cut the look. Since
  round 5 it also holds the WATER QUALITY ladder to itself: every level resolves in range and to its
  own gates, both ladders are monotone in cost and in features, level 0 is flat and level 6 is the
  previously shipped sea, the shader's gate literals are derived from `GateFor()` rather than typed
  twice, the marches take their trip counts from registers, a level with no marches stops asking for
  the scene copy, the three-train fold is bounded by `kMeshPinchMax`, and the F7 slider and the ini
  key agree with the header's own bounds.
- `tools/WaterPreview.cpp`: not a test, but the thing the look was tuned against (see "Looking at
  changes without a GPU"). It needs no device and builds with the same compiler invocation.
  `--quality N` renders a level, and `--probe X Y` prints the shore and foam intermediates that
  level adds, so a ladder step can be audited one pixel at a time without a GPU.
- `tools/Test-Water.cmd`: builds a D3D9 regression executable using the actual
  water Scope and capture code. It checks shader installation, rendered magenta
  debug pixels, no-capture color, the complete batched constant span (including
  untouched gap registers), texture/sampler restoration, independent reflection
  controls, capture lifetime, offscreen rejection,
  optional MRTs (including fractional coverage and unattenuated surface depth),
  size mismatch, classifier cache isolation, partial-bind failure rollback,
  and reset.
  Windows CI runs it; no HAL explicitly reports a GPU-test skip (exit 77), not
  a successful rendering test.
- `tools/Test-Atmosphere.cmd`: includes equal-depth water/terrain boundary
  comparison at MaxZ=.94, in addition to the existing water/fog regressions.

## Round 5: WATER OPACITY, a floor under coverage (2026-10-19)

### What was being seen

At Stormwind Harbor the modern surface was a sheet of glass: the bed, the rocks and the
shoreline under it read through the sea almost undimmed, while the client's own water next
to it hid them. F7 showed every water effect enabled, so this was not a missing pass.

Coverage in the pixel stage was built from exactly two terms: the client's painted liquid
alpha (`baseAlpha=color.a*base.a`) and the column's own extinction (`depthAlpha=absorption`,
the Beer-Lambert term the body's `ClarityPercent` feeds). Both are small in clear, shallow
water, and they multiply: `surfaceAlpha=saturate(baseAlpha+(1-baseAlpha)*depthAlpha)`. There
was no term anywhere - not in `[Water]`, not in F7, not in the register map - that said how
much of the scene the sheet is allowed to leave showing. The renderer had a knob for how far
light reaches *through* the body and none for how much the *surface* covers.

### Why clarity was not the answer

`WATER CLARITY` drives `bodyStyle.x = .088*(100/clarity*zoneClarity)`, which is extinction
per yard. Raising it darkens the bed (through the depth-fog and refracted-bed paths) and only
weakly raises the surface's own coverage, because `absorption` is `1-exp(-sigma*depth)` and at
a shallow shore the depth is a yard or two. Tuning clarity to make the sheet opaque therefore
also makes every zone soup, which is the wrong trade and the wrong knob. The surface needed
its own lane.

### What changed

- **One lane, c199.** `float4 opacityControl:register(c199)` - the first free register in the
  block the water draw already writes. `pixelConstants[19][0]` carries the floor, the other
  three lanes are cleared, and it is built *before* the cached-light splice because
  `localLightConstantCache.values` is 68 floats and stops at c196. No new register was added,
  no upload grew: `c180..c218` is still one `SetPixelShaderConstantF`, and the span was
  already saved and restored by `Scope`, so c199 restores with everything else.
- **A floor, not a second formula.** `surfaceAlpha=max(surfaceAlpha,opacityControl.x)`, placed
  after the reflection and from-below optical maxima and before the wrong-side fade. After the
  optics, so it can only add cover where the optics already agreed and deep water still
  converges on the deep tint. Before the fade, so a far sheet still dissolves at its
  silhouette instead of being pinned opaque by the floor - the floor says how much sea a pixel
  is, the fade says that this fragment is not one.
- **Its own key.** `[Water] OpacityPercent=85`, read exactly like every other water number
  (`ReadTuning`), so `LocationGraphics.ini` can override it per map/zone/area. Written into the
  mirror `tools/WaterPreview.cpp` at the same position in the same order, with the shipped
  default, so `--probe` reports the alpha the DLL draws.
- **F7.** `WATER OPACITY`, 0..100 step 5, default 85, beside `WATER CLARITY` and bounded by the
  same clamp the renderer applies.

0 restores the previous look exactly; that is the escape hatch if a body wants the old
see-through sheet. 85 is roughly the opacity of the client's own water in the report.

### Verification

`tools/WaterRegression.cpp` pins the whole chain: the register declaration, the floor's
position between the underwater maximum and the wrong-side fade, the fact that depth
absorption is untouched (so clarity keeps its meaning and the two knobs cannot drift into each
other), the c199 upload ahead of the cached-light splice, the ini default, and the exact F7
line and bounds.

## Round 6: the sea stops racing after a teleport (2026-10-19)

### What was being seen

Sometimes, right after a teleport, the water moved at several times its normal speed for a
few seconds and then settled. Nothing about the wind or the sea state had changed more than
a teleport changes it.

### The cause: a pivot from the wrong place

`WaveMotion::Advance` integrates every phase and compensates each *heading* and *wave size*
edit about a pivot, so that a sea whose direction is easing does not sweep the crests past a
player who is standing still: a point `d` away from the pivot shifts by `dot(d, Δlattice)`.
That is exact at the pivot and nowhere else, which is the point - the pivot is supposed to
be the camera the player is looking through.

The pivot was `frame.cameraValid ? frame.cameraPosition : lastWaveAnchor.camera`. The
fallback exists for waves-only and no-depth frames, but it is not "the last camera": it is
the last *draw* anchor, and a teleport is exactly the event that leaves one behind. A loading
screen draws nothing, and the first frames of the new zone capture no camera yet, so the
anchor still described the previous zone - hundreds or thousands of yards away. Every
heading/size edit in those frames was therefore compensated about a point in another zone,
and the whole field shifted by the distance between them. The heading eases over ~12 s, so
the result was a fast sweep that decayed as the ease finished: the reported "extreme speeds,
then back to normal".

### The fix

The anchor may stand in for the camera only while it is **fresh** (`LastWaveAnchorFresh`,
250 ms, on the same clock the sea integrates on). With no fresh pivot the lattice simply
holds where it was for those frames: an unknown camera cannot re-anchor a sea. The travel
integration is untouched, so crests keep moving at the sea's own speed throughout. The same
rule now gates the anchor the full-screen medium builds its field from (`UnderwaterFieldAt`),
so the caustic net cannot slide across the bed by the teleport distance either.

### Verification

`tools/WaterRegression.cpp` pins the freshness constant, the `pivotValid` expression that
`Advance` is called with, the medium's use of the same rule, and the `Reset` that clears the
stamp. `tools/UnderwaterRegression.cpp`'s "one field, built at composite" pin now also
requires the fresh-anchor gate.

## Round 7: open water is not a window (2026-10-19)

### What was being seen

WATER OPACITY at its shipped 85% fixed the shallow bed reading straight up through the
sea, but two views still showed the surface as a sheet of glass: the open water past
Stormwind Harbor's southern barrier and the mouth of Booty Bay. Neither has any terrain
under it - the depth capture there holds nothing at all - and the sheet still let the
empty sky behind it through.

### Why the floor could not cover it

Every optical term in the stage is built from the captured column. Over open water there
is no captured column: `behindZ` is the cleared far value, `captureValid` is 0 because no
draw carried a bed, so `depthAlpha=captureValid*(1-transmittance)` is 0 and coverage came
out as `baseAlpha` - the client's painted liquid alpha, which is a small number. The
authored floor then raised that to exactly `opacityControl.x`, and a floor of 85% leaves
15% of whatever is behind the surface showing. Behind an open-ocean pixel there is only
the clear colour the scene copy was never asked to replace, so 15% of emptiness blended
into every one of those pixels. A floor is the wrong instrument where there is nothing to
be translucent *to*.

### The fix

The stage already computes `openOcean` (`step(99999,behindZ)`, the capture's own statement
that no geometry lies behind this pixel). Coverage is now complete there:

```hlsl
float openOceanCoverage=openOcean*(1-underwater);
surfaceAlpha=max(surfaceAlpha,lerp(opacityControl.x,1.0,openOceanCoverage));
```

- Above water the sea is the only thing in the column, so there is no refraction it owes:
  it covers fully and the body's own deep colour, in-scatter and sky reflection are what
  the pixel shows. The horizon still fades into the medium through the fog lerp that
  follows, so the plane meets the sky at the same colour instead of at a hard edge.
- Under the surface the same `behindZ` is the sky through Snell's window, and forcing
  coverage there would paint the window shut, so the term is multiplied by `(1-underwater)`.
  That branch already owns its own coverage rule (`totalInternal`/`transmittance`).
- The rule is above the wrong-side fade, like the floor, so a far sheet still dissolves at
  its silhouette rather than being pinned opaque at the edge of a wave.
- `tools/WaterPreview.cpp` mirrors it (`Mix(.85f,1.f,openOcean)`), because the preview has
  no underwater camera and its branch is documented as exactly zero.

The knob keeps its meaning: WATER OPACITY still says how much cover the surface may leave
out where there IS a bed to see, and raising it to 100 hides even that.

Two more things made the same view read as emptiness rather than as sea.

**The horizon took the sky's colour.** The sheet fades into `fogTarget`, and above water
that was the air's fog colour exactly - so the distant sea converged on the very colour of
the sky it met, which is the "nothingness in the distance". Light reaching the eye from a
distant water pixel has crossed the body, and the body's colour is what makes a sea read as
a surface. The target now takes the deep colour in proportion to the column's own
saturation (`seaShare = saturate(absorption) * .55` above water), so water that the body has
really swallowed keeps a sea colour at range, while a shallow shore - where absorption is
small - still fades exactly as it did. Under the surface the medium's own asymptote is
unchanged.

The absorption term alone was not enough in fog: at 320 yards the air's haze is close to
opaque, so a target that is mostly `fogColor` erased the water into the sky colour whatever
its absorption. `seaShare` therefore also rises with view distance
(`max(..., smoothstep(40, 320, |viewPos|) * .9)` above water): near water still meets the
sky at the horizon, and past a few hundred yards the sheet converges on the body, so
distance deepens the sea instead of deleting it.

A second case has no picture behind the sheet at all: a frame whose scene copy never
landed (`captureValid` 0). There `depthAlpha` is zero by construction and the "behind" a
partial alpha would blend is whatever the client left in the target - over water, the
clear colour. Nothing captured is not something to see through, so that case joins the
open-ocean branch (`max(openOcean*(1-underwater), 1-captureValid)`). It is a safety net,
not the normal path: with a capture present nothing about it changes.

### Verification

`tools/WaterRegression.cpp` pins the term, its `(1-underwater)` gate, its position between
the optical terms and the wrong-side fade, the captureless safety net, the tinted fog target
(and its preview mirror).

## Round 8: the medium does not depend on which way the camera points (2026-10-19)

### What was being seen

Underwater, the medium (fog, caustics, beams, grade) disappeared at some camera
positions - most reliably when the view turned up towards the surface - and came back
when the camera turned back. The water *surface* was still shaded; only the medium was
missing.

### The cause

`Underwater::Render` refused any frame whose `FrameContext::cameraValid` was false. That
flag is published by the frame's camera capture, which needs a draw carrying the eye:
a frame that draws only sky, only a wall, or only UI publishes no fresh pose, so
`cameraValid` stays false while the retained inverse view, projection unpack and camera
position are still the last accepted ones (a frame or two old). Looking up from under the
surface is exactly such a frame, so turning the head switched the whole medium off and a
terrain draw switched it back on. The state itself was never wrong - the tracker kept the
submersion the entire time, including ten-second stretches with no liquid draw at all
(`UnderwaterRegression`'s "diver looking at the seabed" case) - so the camera pose gate
was the only thing dropping the pass.

### The fix

The retained pose is used for a bounded hold, exactly like the local lights:

```cpp
const bool poseRetained = !frameContext.cameraValid && frameContext.viewRawValid &&
    frameContext.cameraEyeAgeMs <= FrameContext::kCameraEyeHoldMs;
```

- Inside `kCameraEyeHoldMs` (1 s) the pass runs on the rows the frame still carries. The
  reconstruction is off by however far the camera moved since the last eye-carrying draw,
  which for one hold is the swimmer's own movement - and the tracker refuses to advance
  the eye on those frames, so nothing drifts.
- Past the hold the pass refuses as before: a genuinely stale camera is a wrong world
  position, and the `NoteIdle(7)` line now says which of the two it was.

### The rest of the camera dependence

Holding the pose is the fix for the transient; the pass also reads four *bits* that are
left over from the camera capture and must be this frame's before the hold applies:
`projUnpack` (is the depth this frame's projection), `depthMaxZ`, `viewRawValid` (the raw
view is this frame's) and `cameraValid`. Those are checked before the hold, so a retained
pose is only ever used with a projection that goes with it - the pair is republished
together or not at all. The medium itself (clarity, tint, deep colour) is rebuilt from the
tuning and the environment every frame whether or not a liquid draw ran, so no part of the
*fog* is carried over from a draw or a previous camera.

### Verification

`tools/UnderwaterRegression.cpp` pins the hold (both the expression and the 1 s bound)
next to the existing NaN/projection refusals, so the pass cannot go back to dropping the
medium by camera angle.

## Round 9: WATER OPACITY owns the colour, not only the blend (2026-10-19)

### What was being seen

The slider moved, the sea did not. Coverage was already floor-ed at 85% and had made
the shallows better, but open water still read as a sheet of glass, and dragging the
slider between 85 and 100 changed almost nothing.

### Two channels, one of them unwired

There are two independent ways the scene under the water reaches the screen.

1. **The blend.** `surfaceAlpha` decides how much of the captured scene shows beside the
   water's own colour. This is what the floor from round 5 controls.
2. **The body's colour.** `bottom = refracted * transmittance + inScatter + deep * absorption`
   is the *captured seabed*, already inside the water's own output. That is then mixed in
   at `depthValid * RefractionPercent`, its own authored control, and the result converges
   on the deep colour by `absorption`.

The floor only ever touched (1). With refraction at its shipped 14% the bed contributed
little, but at a high REFRACTION setting - or wherever the capture is missing outright -
the body was the whole picture, and the slider could not touch it. Worse, without a
capture the stage fell straight through to the client's own liquid colour, which over open
ocean is a pale, nearly invisible sheet: the colour of the sky it sits under.

### The fix

- `body = lerp(body, seaBody, opacityControl.x * saturate(effectiveDepth * .5))`, **after**
  `body` has been composed from the client's colour and the captured bottom. This is the
  line that failed the first time round: the fade was applied *inside* the captured-bottom
  term, which REFRACTION scales - 14% at the shipped setting - so 85% of the surface stayed
  the client's own pale liquid colour at every slider position. "Opacity does nothing" was
  literally true: it moved a sixth of the picture. Now 100 is the sea body and 0 restores
  the physics exactly, and the depth gate keeps a shoreline honest (the first yard of a
  sheet cannot hide a bed it is not over).
- A frame with no scene copy at all now shows `deep + inScatter` rather than the client's
  liquid. Coverage is already forced to 1 there (round 7), so the surface is a body with
  nothing to blend - and the body is what it should look like.
- The CPU preview carries the same `hideBed` at the shipped 85%.

`tools/WaterDeviceRegression.cpp`'s sRGB/refraction block now sets `opacityPercent = 0`
while it asserts that a copied 128 gray stays 128: with the new term in place, asserting
the raw refraction round trip *at the shipped opacity* would be asserting that the slider
does nothing.

## Round 10: coverage needs a blend, and the bed reading cannot veto the slider (2026-10-19)

**The alpha was never reaching the screen.** The replacement pixel stage's whole
contract is "this fragment is this much sea": `result_.color.a` is the column's
own extinction and the fixed-function blend is what lets the world below show
through by exactly `1 - a`. The pass never touched the blend state, so the draw
composited exactly as the native liquid material was authored to - and an ocean
composited additively (or with `ONE` as the source factor) makes alpha a no-op.
On such a client every coverage change we ever shipped was invisible: the body
colour still replaced the client's own liquid texture (that is what "opacity
hides the old water"), while the seabed stayed at full strength through the
sheet at any alpha, which is "it does not regulate real underwater
transparency". No amount of tuning inside the shader could have reached it.

- The pass now sets `ALPHABLENDENABLE=TRUE`, `SRCALPHA`/`INVSRCALPHA`,
  `BLENDOP_ADD` and `SEPARATEALPHABLENDENABLE=FALSE` for the replaced draw only.
  The `D3DSBT_ALL` block the pass already captures restores all of it, and the
  no-block path saves and writes the four values back itself.
- A draw whose native blend is not already that is logged once per distinct
  combination (`liquid draw composited with blend enable=...`), and the periodic
  line carries `blendOverridden=N`. That sentence is the evidence, without a
  capture, that this was the client's compositing and not our coverage.
- `WaterRegression` pins the four writes, the restore, and the log line.

**A missing depth reading used to switch the slider off.** `hideBed` was gated
by `saturate(effectiveDepth * .5)`, and `effectiveDepth` is 0 whenever the
captured depth is missing, is not behind the fragment, or is the sheet's own
surface (`behindZ == viewPos.z`, which is what a copy that already contains the
water looks like). Treating that as "ankle deep" meant the fade weight was zero
over exactly the open water the slider exists for: the surface kept the
captured bed at full strength and the only visible effect left was the alpha.

- `bedTrusted = depthValid * step(viewPos.z + .25, behindZ)` and
  `bedFade = lerp(1.0, saturate(effectiveDepth * .5), bedTrusted)`: an unknown
  or implausible bed is **not** a shallow one, so opacity keeps full authority.
  A genuinely measured shallow shelf still keeps its sand, which was the point
  of the gate. The CPU preview mirrors it.

## Round 11: the far sea was a different sea (2026-10-19)

The water fogs into `fogTarget`, and `fogTarget` used to be the **deep tint**,
while the water's own body is the deep tint **plus in-scatter**. Two different
colours, then, for the same sea at two distances - and the fog is a curve that
saturates, so the change between them happened over a short span of screen and
read as a flat grey slab lying on the horizon with the live water in front of
it. That is the reported "chunk over the horizon": not a missing draw and not a
zone, but the distance at which the air takes over being a different colour from
the water it is taking over from. Raising WATER OPACITY could only darken it,
because the slab was already the water's asymptote - just the wrong one.

- `seaBody = deepTint.rgb + inScatter` is now named once, above the capture
  branch, and `fogTarget` lerps toward **it** (`lerp(fogColor, seaBody, seaShare)`)
  above water. A fully fogged sheet is exactly the body a near sheet converges
  on, so the fog distance cannot be an edge in the middle of one plane.
- The distance share rises over `smoothstep(40, 600, |viewPos|)` to `.85`, not
  `.9` at 320: the ramp is long enough not to be a visible band, and the last
  15% of air keeps the horizon from becoming a hard line against the sky.
- The uncaptured branch uses the same `seaBody`, and the CPU preview mirrors all
  three. `WaterRegression` pins that the fog target is the body and that the old
  `lerp(fogColor.rgb, deepTint.rgb, seaShare)` spelling is gone.

## Round 12: the foam mask was reading a figure as a shoal (2026-10-20)

Reported after a rollback to `79d2907`, so this predates every round after it:
**turning FOAM off removes the dark second copy of the figure.** That is the
first report that named a term instead of a symptom, and it names the mechanism.

`shoal` is the one foam source that is not about depth or waves:

```
shoal = foamDepthValid * saturate(bedSlope * bodyStyle.w * 2.0 - .3) * saturate(1 - foamDepth*.05)
```

`bedSlope` comes from four depth taps around the pixel (`bedE/W/S/N`), and those
taps can land on **anything standing in the water**. Reconstructed from a
figure's depth a tap is a point ABOVE the surface this fragment belongs to, the
slope between it and the centre reads as a cliff, and the shoal mask reads a
cliff as *"the bottom comes up fast"* and lays foam over it.

Foam is not a decal in this stage. `reflectionAmount` is scaled by
`1 - foamAmount*.85` and `pathEnergy` by `1 - foamAmount`, so that band of
surface loses **the mirror and the sun road** at once: a figure in the water was
surrounded by a character-shaped region with no reflection and no glint - a
dark copy of him lying on the water. It is downstream of opacity, refraction and
reflection, which is why all three sliders left it alone.

The centre point already had the test that answers this - `foamDepthSigned`, the
world-up dot of (surface - bed). Each tap now gets the same test before any
slope is derived from it:

```
bedE = lerp(bedE, bottomView, step(0, dot(bedE - viewPos, upWind.xyz)))
```

A tap above the surface is not bed and falls back to the centre point, exactly
as a tap on the sky already did; four equal points give a flat bed, no shoal and
no invented cliff. A real shoal is unaffected - its taps are below the water by
definition.

## Round 13: the sea re-anchors on the camera it was drawn from (2026-10-21)

### What was being seen

Round 6 stopped the sea from pivoting about the previous zone's *draw anchor*, and the report
came back: **the water still races after a teleport, and mostly where the surface is seen
edge-on rather than ahead.** That detail is what round 6 could not explain, because it fixed the
age of the pivot and not its identity.

### Why round 6 was still open

`frame.cameraValid` does not mean "this frame's camera". `CameraContinuityGate` accepts a new
camera position only after **three consecutive candidates agree**, so for the first frames after
a jump the frame publishes the pose it *holds* - the place the player left - and marks it
`PoseHeld`. Its eye-age clock reads fresh, because a draw really did carry that eye a few frames
ago. The round-6 rule (`frame.cameraValid || LastWaveAnchorFresh()`) therefore still handed
`WaveMotion::Advance` a pivot thousands of yards away from the water being drawn, and every
heading edit in those frames was compensated about the old zone.

Two further things kept the sea moving after the landing frame:

* the heading ease is **12 seconds long**. Whatever is left of it at the landing keeps sweeping,
  and it sweeps proportional to distance from the pivot - which is the other half of "edge-on
  rather than ahead": the far side of a bay is where the sweep has the most distance to cover.
* the two upper chop octaves were built from the *relative* cell plus a rotated copy of the
  camera's own phase (`2.17*plane + 1.31*R90(phase)`, `.46*plane - .87*R90(phase)`). Those two
  halves are not the same transform, so at a fixed world point that coordinate moved with the
  camera: about **2.5 lattice cells per yard travelled** for the fine octave. The tree had
  already found this fault and retired it from the foam lace's fleck, where it printed as
  crawling hash; the chop itself kept reading it.

### The fix

1. **Identity, not age.** `SeaPivotForFrame` takes the point from
   - this frame's own liquid draw (`lastWaveAnchor`, tagged with `frameIndex`), else
   - the frame's captured camera, and only when `cameraPoseSource == PoseCaptured`.

   A held pose is never a pivot, and an anchor from an earlier frame never is either. The
   medium's own anchor rule (`UnderwaterFieldAt`) is unchanged, because that pass reconstructs
   its offsets against the same `frameCtx.cameraPosition` it is handed - the two halves of *that*
   field agree even when the pose is held.
2. **A jump settles instead of easing.** A pivot that lands more than
   `Tracker::kCameraJumpYards` (50) from the last one is a teleport, a hearth, a portal or a
   zone change. `WaveMotion::Settle` finishes the pending turn and the WAVE SIZE change in that
   frame - no easing, no pivot compensation - so the lattice the player arrives to is the one
   the wind already asked for. Nothing from the old place is carried across, and there is no
   ease left to watch. Travel for the frame is still integrated normally.
3. **The octaves read the absolute cell.** `worldCell = plane + wavePhase.xy` once, and then
   every octave is a fixed matrix on it:
   - base: `worldCell` (unchanged),
   - fine: `[[2,1],[0,2]]` - sheared, ~2.2x the base frequency, rotated, incommensurate,
   - mid: `-.5*y, .5*x` - half frequency as half of a quarter turn, counter-phased.

   The matrices are **integer** (the mid one half of an integer, with a hash period of 32 to
   match) so the CPU's 64-cell phase reduction stays invisible: a 64-cell step is a whole number
   of each octave's periods, exactly as it already was for the base octave. A non-integer scale
   would have re-rolled the grain every 160 yards of walking.

### Verification

`tools/WaterRegression.cpp` measures the two octaves' coordinates at one fixed world point from
five cameras (and three WAVE SIZEs) and requires them to agree to phase precision, checks that a
64-cell reduction of the phase is a whole number of each octave's periods, and pins the new
pivot/settle expressions. `tools/WindRegression.cpp` proves `Settle` lands exactly on the sea the
wind and WAVE SIZE ask for, that it is not a freeze (travel and later turns continue), and that
calm air still keeps the residual swell's heading. `tools/WaterPreview.cpp` mirrors both octaves.
The status line now carries `pivot=` (1 this frame's draw, 2 a capture, 0 held), `jumps=`, `yaw=`
and `scale=`, so a player's log says whether a sweep had a pivot at all.

## In-game verification / troubleshooting

1. Install the rebuilt **32-bit** `d3d9.dll` next to the client executable and
   restart. Enable WATER in F7 or set `[Water] Enabled=1` in the effective
   `GraphicsEffects.ini`/location settings. No startup water gate is required.
   Check F11 has not disabled the graphics package.
2. Set `WaterDebugMode=1` in the effective water settings and press F12. Supported
   water should turn magenta. Return it to `0` afterward. Mode `2` isolates glint.
3. Inspect `WaterEffect.log`: `compile`, `vertex_compile`, and `replacement active`
   distinguish compilation and installation. Periodic candidate/replacement
   counters distinguish unmatched materials from failed installation. Shader
   failures include the HRESULT and `PS30 slots` capability.
4. For SSR/refraction/depth tint, use in-game **Multisampling 1x** and restart;
   INTZ capture cannot replace multisampled depth. `WaterReflection.log` records
   the first pre-water scene copy. Missing capture must not disable basic water.
   The edges come from the image pass's anti-aliasing instead - see
   `docs/anti-aliasing.md`, which lists everything above 1x costs.
5. Check a coast, a shallow canal, under a bridge, sun/moon glint, and a nearby
   lamp. Test reflections off with refraction/depth/foam on, atmosphere off,
   F11 off/on, window resize, and Alt+Tab. Native lava/slime and unsupported
   texture layouts must not become water.
6. If magenta never appears, edit the existing `[Diagnostics]` section in
   **ModernWoWRenderer.ini** (not GraphicsEffects.ini): `Enabled=1`,
   `AutoCapture=1`, `MaxDraws=8192`, `SkipDraws=0`. While looking at the broken
   water, press F12. This reloads diagnostics and arms **one frame**, without
   requiring F8 or a restart. For manual F8 captures use `AutoCapture=0`.
   Include `WaterEffect.log`, `WaterReflection.log`, `VolumeEffects.log`, and
   the newly reported `WaterDiagnostics/<timestamp>/` folder (draws.txt and
   shader binaries). Turn diagnostics off and press F12 after collecting it.
   Capture is observation-only and also works with F11 OFF.
7. If there is no capture folder, read `water diagnostics:` entries in
   **WaterEffect.log**. They identify the exact configuration and output paths,
   effective enable/auto values, disabled F8 presses, the eight-capture limit,
   directory/file failures, start and completion counts. If that log is also
   unwritable, messages go to the Windows debugger output. A zero-draw frame
   still creates a capture file: missing output is not evidence of missing
   water. Earlier DLLs silently ignored disabled F8 presses/file failures and
   did **not** reload diagnostics with F12; they require a restart after editing
   the section. These diagnostic changes do not broaden water detection or
   claim to resolve an unobserved liquid layout.
