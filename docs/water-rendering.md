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
state without spending a render pass, and **560 instruction slots** enforced by
`tools/ValidateShaders.ps1` (the shader measures 542 - 543 before the doubled tint came out;
the budget was raised from 496 for this rework rather than cut a visible term - see "Shader
budget raised for the volume rework" below, which also records what a 512-slot device does). So each of those has a substitute
here: an analytic two-train swell plus bounded value noise for the simulation, a roughness mix
toward the sky for the blurred chain, and a shaped-exponent lobe whose exponent carries both
the roughness and the sun's height for GGX with Smith visibility - a low sun smears the road
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
slot budget, the MSVC build and the device test all run on the Windows CI job; this environment
cannot run fxc, so "it fits in 560 slots" is a CI result there, not a local one.

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
anything newer than the GeForce 6/7 and Radeon X1xxx generation that the 512 minimum was written
for; recent drivers report the maximum. That is a player-visible limitation and is in
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
shader validation measures **542 slots** and enforces a **560-slot budget** (the 496-slot
budget it used to enforce stood below the 512-slot hardware minimum until the volume rework of
2026-10-14 above outgrew both). Device-creation failures log the HRESULT and device instruction
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

- `tools/ValidateShaders.ps1`: compiles the production VS/PS and fails if water
  exceeds 560 static instruction slots (542 measured; the ceiling was raised from 496 - see
  "Shader budget raised for the volume rework") or its instruction count cannot be read.
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
