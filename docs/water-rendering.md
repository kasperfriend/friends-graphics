# Water rendering audit

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
run, and lets it be (fxc /O3 put the current shader at 642 slots on Windows CI on 2026-10-14; it had
been held to 621 - 597 measured plus 24 of compiler margin - until that ceiling started deciding
which waves were allowed to shade, which is the wrong job for a budget, so round 4 retired it rather
than re-negotiating it. The count is still the number to argue about, and the reason to keep measuring
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

Every value above is a `[Water]` key, read per area first (`LocationGraphics.ini`, the same
scope the enable switch uses), so zones differ without a code change: `ClarityPercent`,
`WindStrengthPercent`, `SparklePercent`, `WaveRoughnessPercent`, `SheenPercent`,
`ShoalFoamPercent`, `FoamBrightnessPercent`, `SunDiscPercent`, and the three colours
(`WaterColour` multiplies the zone's own painted water with `0x808080` neutral, `DeepColour`
is the asymptote, `AbsorptionColour` is the per-channel rate). Colours are the only values
that need text rather than an integer, so `ReadTuningColour` parses `0xRRGGBB` or decimal in
this header rather than growing a colour type in the shared tuning reader. `WATER CLARITY`,
`WIND` and `SUN SPARKLE` are on the F7 overlay; the rest are file-only for now.

### Sea state: the zone and the weather

The keys above decide what a body of water looks like. This decides how much of it there is.
Water was already procedural and already moving - `BuildWaveField` is an analytic sum of sine
trains advected across the surface by `FlowSpeedPercent` and `FlowRotationPercent`, and the vertex
stage displaces the same field the pixel stage lights - but it moved the same amount in a mountain
pond as off the coast of Stranglethorn, and it did not care whether it was raining. Both are now
inputs.

The model is one pure, device-free function, `renderer::water::TargetSeaState(zone, weather)` in
`src/Water/WaterSeaState.h`, shared by the DLL and the previewer so a PNG here and a frame in the
game cannot disagree about what a setting does. Two things feed it:

- `WaterAgitationMultiplier` in **EnvironmentProfiles.ini**, which is the same per-zone layer the
  cloud climates ride (`[Zone.33] Profile=CloudLush WaterAgitationMultiplier=1.45` keeps the Lush
  climate and retunes one water number, because `ResolveEnvironment` inherits the chain and then
  applies the section's own keys). All 88 authored zones carry a value: 1.4-1.5 on the open seas,
  1.45 for Stranglethorn, 1.15-1.3 for the high coasts and fjords, 0.45 in Elwynn, 0.3 in
  Undercity. `[Default]` is 1.0, which is what makes the feature shippable switched on: a zone
  nobody authored is exactly the old water. A value outside 0-4 fails the whole F12 reload rather
  than being clamped, in company with every other key in that file.
- live weather, taken from the observation the clouds already make. `ModernWoWRenderer.cpp` calls
  `watereffect::FeedWeather(nativeWeatherMode, intensity)` on the line after
  `volume::cloudWeatherStrength`, so rain closes the sky and roughens the water in the same frame
  instead of only one of them agreeing that it is raining.

`[Water] WeatherResponsePercent` (0-200, default 100) is how far weather is allowed to push a
zone, `WeatherEaseSeconds` (1-120, default 14) how long it takes to get there, and `ZoneWaves`
(0/1) the switch that pins every zone to 1.0 if a value in the profile file is disputed. The easing
is the cloud weather easing's own shape, `Response = -expm1(-3*dt/seconds)` - 95% of a step in one
time constant - with the drain at twice the build, so open water settles rather than snapping flat.
`dt` is clamped to 0.1 s per frame, so a loading screen cannot teleport the sea state. A zone that
damps storms with `WeatherIntensityMultiplier` also damps what those storms do to its water:
`Present()` multiplies the response by `Environment[Weather]`. That coupling is deliberate, and it
is documented at `[Default]` in the profile file, where an author meets both numbers.

The five outputs are multiplied into floats the CPU already uploads. There is no new constant
register, no HLSL edit, and the slot budget is untouched:

| output | where it lands |
| --- | --- |
| `agitation` = `clamp(zone*(1+.95*push), .12, 2.6)` | `controls[1]` wave amplitude, `[32]` swell, `[35]` crest foam, `[30] *= .7+.3*agitation` shore foam |
| normal response | `controls[2] = NormalPercent * clamp(sqrt(agitation), 0, 2.5)` |
| `shear` = `clamp(.25*push + (agitation-1)*.12, 0, 1)` | `BuildWaveField(elapsed, 1, clamp(flowRotation*(1+shear), 0, 1))`, a cross-chop over the swell |
| `wind` = `clamp(authoredWind*(.45+.55*agitation), 0, 2.5)` | `controls[51]`, the wind the shader shades and foams against |
| `motion` = `clamp(1+(agitation-1)*.55+.8*push, .08, 3)` | `SeaMotionClock`, which supplies `elapsed` |

`push` is `response * amount`, where `amount` is the current weather's intensity and `response` is
`WeatherResponsePercent * WeatherIntensityMultiplier`. `motion` is the reason a clock exists at all:
wave phase is `elapsed * speed`, so multiplying an absolute clock by a *changing* speed moves every
crest the instant weather eases - one visible jump per frame while a squall arrives. `SeaMotionClock`
accumulates `dt * speed` instead, so a speed change alters future travel only. It never wraps and it
ignores a non-finite step.

`Reset()` forgets the eased state, so reloading settings or changing zones cannot interpolate across
the change. An authored `WindStrengthPercent` of 0 keeps a river foamless in any weather, because
every wind-derived term here is a multiplier on what the zone asked for. `WaterEffect.log` prints the
eased state every 600 frames (`sea=agitation,motion,shear weather=kind,strength`), which is how a
report of "nothing changed in Stormwind" becomes a number rather than an argument.

On the F7 Water tab: `WAVE SPEED`, `WEATHER RESPONSE` and `PER-ZONE SEA STATE`, next to the existing
`WIND`, `WATER CLARITY` and `SUN SPARKLE`, and saved per location like the rest.

`tools/WaterRegression.cpp` asserts the identity case (untuned zone, clear sky: every multiplier 1,
no shear), the ordering (an exposed coast in calm air is rougher than a sheltered pond in a
downpour), the clamps, that a NaN never reaches a constant, the easing rate and its framerate
independence, that the drain is slower than the build, that the clock does not jump when speed
changes, and that the shipped 88-zone file stays inside the accepted range.
`tools/CloudClimateRegression.cpp` parses the same file with the real resolver and asserts that a
water-only zone line keeps its climate, which is the cheapest proof that two features sharing one
authoring layer do not collide. In-game, this has not been looked at yet.

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
| `c210` | chop phase (xy), then the two displaced trains' phases (zw) |
| `c211` | texture creep (xy), the shaded-only train's phase (z), the waves gate (w) |
| `c212` | this draw's world-up in view space (xyz) and its authored wind (w) |

The vertex stage reads one more set of constants than it used to: `c5..c8`, the draw's own
model-to-view matrix, are read back on the CPU so the wave phase and the shading's up axis come
from the same matrix the displacement uses. `watereffect::Scope`'s save/restore is unchanged in
size - seventeen pixel registers and three vertex registers - because nothing new was added,
only rearranged.

Round 4 grew both stages by the minimum: the vertex stage gained `c204` (the grouping field's
camera anchor) and the pixel stage `c217` (the four shading-only trains' phases, in one
contiguous `float4`). The scope now saves and restores eighteen pixel and five vertex registers,
`controls` is 72 floats and `vertexControls` is 20. **No interpolant was added, because there was
no room for one** - see the register note below.

| register | contents, as of round 4 |
|---|---|
| `c200` (VS) | amplitude, slide, sigma, one cell in yards |
| `c201` (VS) / `c209` (PS) | the world-to-lattice rows, identical in both stages |
| `c202` (VS) | xy = the phases of the two displaced trains; the PS's `c210.zw`, verbatim |
| `c203` (VS) | xy = the domain warp's camera anchor, wrapped in the warp's own period |
| `c204` (VS) | xy = the grouping field's camera anchor; never reaches the pixel stage |
| `c210` (PS) | chop phase (xy), then the displaced pair's phases (zw) |
| `c211` (PS) | texture creep (xy), unused (z), the wave gate (w) |
| `c217` (PS) | the phases of the four trains only the shading carries |

The warp value and its gradient arrive as `TEXCOORD7` (a `float4`: xyz the sample, w the
grouping gain) and `TEXCOORD4` (a `float4`: xy the lattice coordinate, zw the grouping gradient).
The pixel shader samples no low-frequency noise at all, which is what keeps a set nailed to the
terrain for one multiply and one `dot`.

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
Both halves now come from the draw itself: `watereffect::Scope` reads `c5..c8` back with
`GetVertexShaderConstantF`, and `renderer::water::AnchorFromModelView` in `src/Water/WaveField.h`
turns that matrix into the camera position and the world-up axis the shading uses. It refuses a
basis that is not orthonormal - the pairing "view position dotted with row *i* is the world
offset's component *i*" is only true of a rigid basis - and the caller then falls back to the
captured camera, which is the right world even when it is the wrong frame. The regression test
builds real yawed, pitched and mirrored matrices, runs the shader's own dot products through them,
and requires `rel + anchor == world point` to within two centimetres at every angle.

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
  compile, if the water instruction count cannot be read, or if any *budgeted* shader - the volume
  and cloud raymarches and weather - exceeds its ceiling. The water pixel stage is measured and
  annotated but not capped (fxc /O3: 642 slots on Windows CI on 2026-10-14; the 621 it used to be
  held to is described under "Full rework round 4").
- `tools/ShaderSourceRegression.cpp`: reads every raw HLSL literal in the tree and rejects the
  GLSL spellings fxc reports as undeclared (`mix`, `fract`, `mod`, `inversesqrt`, the GLSL
  texture lookups and type names), naming the line and the HLSL spelling meant. It runs in the
  portable job, because shader text was previously only ever compiled by fxc on the Windows
  runner - which is how `mix` reached the volume shader's sun glint and cost a CI run.
- `tools/WaterRegression.cpp`: portable material, independent-capture toggle
  combinations, and target-size policy tests (included in Linux CI). It also pins the in-scatter
  and sun-glint normalizations as shader text - in the production literal and in the CPU mirror -
  so a cut made to fit instruction slots cannot quietly take the look with it.
- `tools/WaterPreview.cpp`: not a test, but the thing the look was tuned against (see "Looking at
  changes without a GPU"). It needs no device and builds with the same compiler invocation.
- `tools/Test-Water.cmd`: builds a D3D9 regression executable using the actual
  water Scope and capture code. It checks shader installation, rendered magenta
  debug pixels, no-capture color, constant/texture/sampler restoration,
  independent reflection controls, capture lifetime, offscreen rejection,
  optional MRTs (including fractional coverage and unattenuated surface depth),
  size mismatch, classifier cache isolation, partial-bind failure rollback,
  and reset.
  Windows CI runs it; no HAL explicitly reports a GPU-test skip (exit 77), not
  a successful rendering test.
- `tools/Test-Atmosphere.cmd`: includes equal-depth water/terrain boundary
  comparison at MaxZ=.94, in addition to the existing water/fog regressions.

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
