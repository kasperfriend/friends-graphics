# Upward rain / beam artifact (2026-10-14)

## Cause

The before-UI weather continuity shader is independent of the cloud renderer.
It was sampling rain at `uv + time * velocity`: because screen V increases
**downward**, positive sampled-V travel moves the visible texture **upward**.
Snow had the same sign error. The rain UV's vertical scale was also multiplied
by `.22`; with the nearest layer scale of `1.6`, a single texture period spanned
`1 / (1.6 * .22) = 2.84` screen heights. Broad, upward-moving illuminated streaks
therefore remained visible when clouds were disabled.

## Corrections

- Subtract particle travel from sampling coordinates. Positive gravity now
  moves rain and snow down, and positive horizontal wind moves drops right.
- Sample rain in compact cells, with per-cell occupancy and horizontal jitter
  to break repeated columns. Each nearest-layer cell is at most 3.91% of the
  screen height, with transparent gaps and tapered ends; farther drops are
  smaller. Native world-space billboards remain the primary precipitation.
- Match procedural fallback rain to the authored low-alpha mask rather than
  introducing full-opacity streaks when the optional asset cannot be loaded.
- Use the resolved weather mode for texture selection: Auto snow previously
  ran the snow shader branch with the rain texture.
- Normalize captured depth using viewport MinZ/MaxZ before occlusion; treat
  cleared sky/no-depth input as distant. Reject depth without a valid camera
  or from another device. Missing depth disables occlusion, not precipitation.
- Set and restore fog, stencil, scissor, fill, color-space and all used sampler
  states explicitly. Preserve the tracked game pipeline. Mist intensity is
  applied once, not squared, and zero wind no longer introduces vertical wind.
- Compile runtime weather shaders at the same optimization level as validation
  and enforce a 496-slot precipitation shader budget.

No cloud scattering, sun-shaft direction, or native particle vertex motion is
changed. This fixes a concrete artifact in the added weather pass; an in-game
check is still needed to confirm the reported view has no additional issue.

## Regression tests

`tools/Test-Weather.cmd` runs the production HLSL and before-UI integration on
D3D9, without instantiating clouds or atmosphere. Tests compare rendered frames:

- At 512x512 and 1/60 second, the isolated near rain layer translates 8 pixels
  down (not up), and snow translates 4 pixels down.
- Positive wind produces the expected diagonal translation.
- Rain is visibly present but has bounded streak lengths.
- Near surfaces occlude rain; equivalent depth at MaxZ=1, MaxZ=.94 and nonzero
  MinZ produces the same image.
- Auto snow selects snow textures with no INTZ; hostile inherited UI state
  does not clip or color the pass, and constants/samplers/state are restored.
- No native weather means no added weather pass; fallback masks stay low-alpha.

The Windows Release CI job runs these tests. A runner without a D3D9 HAL reports
an explicit GPU-test skip rather than claiming the images were tested.

## In-game check

During native rain, disable clouds and lens droplets to isolate the added rain
layer. Check downward, short streaks at normal and high RainSpeedPercent, with
zero wind and positive/negative wind angles. Compare F10 on/off (weather only),
then restore lens droplets/clouds. Also check under cover, Auto snow, F7 and
Alt+Tab. Full depth occlusion requires in-game Multisampling 1x and camera capture.

# Long-session full-screen rain overflow (2026-10-17)

## Cause

The pixel stage builds each layer's grid as `(uv * cells - PHASE)`, where the
phases are integrated in double on the CPU, uploaded as float, and run
unbounded for the whole rain. The stage is float32, and subtracting a phase
of a few thousand cells from a uv term below a hundred cells is a
cancellation: the grid quantizes to the ulp of the phase. The distinct
vertical levels collapse (measured at 1080p for the nearest layer: 1016 at
start, 512 after 10 minutes, 128 after an hour, 16 after six), and the drops
merge into screen-wide horizontal bands of rain - the reported "the screen
overflows with rain particles". Snow (about 4.6 h) and the sandstorm (about
5.2 h) share the bug; 4K degrades faster, 720p slower.

## Fix

A phase split, computed in exact double in `PrecipitationMotion.h`:
`PHASE = N + f`, with `f` in [0, 1) and `N` wrapped into the layer's own
lattice period, uploaded as five float4s (c6-c10). The float conversion of
the fraction can round up to exactly 1.0 - either from a fraction above
`1 - 2^-25`, or from a phase a hair negative, which is what the wind
projections sitting on the camera axis produce; in that case the one moves
into `N` instead, so the stage always receives a genuine `[0,1)` fraction.
(The `tools/WindRegression.cpp` suite integrates the production split for a
six-hour session and pins exactly this.) The pixels subtract only
`f` - small minus small, no cancellation - so the pixel levels stay exactly
as fine as at t=0 for any session length, and `N` only shifts the cell
index, which the cell hash wraps by the same period. The wrap is seamless
(the hash is periodic in the period, and the split makes the pattern a pure
translation), and the layers' periods are deliberately non-commensurate
(rain: x 32768, y 32768/16384/8192; snow and sand have their own tables), so
each layer's pattern repeats on its own 8-50 minute cycle and the combined
pattern effectively never repeats. The snow/sand gust sines wrap their
arguments at 2pi, and the mist keeps only its [0,1) drift parts.

## Verification

- A float32-faithful CPU emulation of the stage (scratch, not shipped)
  proves: the per-layer distinct pixel-level counts are identical for
  N = 0, N = P/2 and N = P-1 over a full f sweep at every resolution
  720p-4K, so sharpness is independent of session age (previously
  collapsing to single digits after a few hours); the split cell index
  matches the original unsplit cell at 0/114,288 sampled (phase, row)
  pairs over six-plus hours including the wrap crossing; and the very
  first frame rendered (t = 0) is pixel-identical to the old shader at
  0/1,555,200 mismatches. After the first frame the drops come from the
  periodic [0, P) window of the same hash - same distribution, so the
  rain statistics are unchanged.
- `tools/WeatherDeviceRegression.cpp` drives the production stage with
  phases from `ComputeWrappedPhases` (the same call `BeforeDraw` makes) and
  pins the 8 px fall / 4 px snow / 8 px wind translations, so the CPU
  formulas and the shader grids cannot drift apart.

The precipitation shader budget moved from 496 to 512 slots in
`tools/ValidateShaders.ps1` - 512 is the ps_3_0 floor every baseline card
must support, so the pass still fits them all; the split's per-layer
split+wrap slots needed the difference.

## In-game check

Sit through a long heavy-rain session (an hour or more at moderate-to-high
RainSpeedPercent, with and without wind) and confirm the rain stays fine and
staggered - no screen-wide horizontal bands of rain forming as the session
ages. Check Auto snow and sandstorm the same way. The near layer's pattern
repeats every ~19 minutes; because the other two layers never realign with
it, the combined rain should look unchanging rather than repeating.

# Full-screen rain that was not the phase collapse (2026-10-17, second pass)

Reported again after the split above shipped: *"rain shader is still overflowing full screen most
of the times"*. The word that matters is **still**. The phase split is sound - every grid is bounded
by its period, the wrapped cell index never exceeds it, and the decimal part the stage subtracts is
always below one - so the same symptom coming back meant a different mechanism. There were two, and
neither had anything to do with session age, which is why the report said "most of the times" rather
than "after an hour".

## Cause 1: the indoor factor was never set

The precipitation stage is a screen-sized quad composited over everything the client drew, so
nothing inside it knows where the player is standing. The one thing that was meant to keep rain out
of a cave, an inn or a city arcade is `smoothedIndoor`, uploaded as `c2.w` and read by the stage as
`intensity * (1 - indoor * 0.96)`.

It was declared in `WeatherVisuals.h`, read into two constant buffers (precipitation and the lens
droplets), and never once written by anything in the tree. It was permanently `0.0`, so the
suppression never happened: rain drew at full strength over the whole screen under any roof, and
kept doing so for the whole of `nativeWeatherHoldMs` - five seconds - after the client itself had
stopped emitting particles.

## Cause 2: a missing depth reads as "infinitely far"

When no INTZ depth was captured, the neutral fallback texture is a 1x1 `0xFFFFFFFF`, so
`GetLinearDepth` answers 100000 for every pixel. That was already handled for roof occlusion
(`lightColor.w` carries `hasCapturedDepth`), but the mist block was gated only on distance:

```hlsl
if (isSand < 0.5 && sceneZ > 6.0)   // and a 6..40 yard fade inside
```

With `sceneZ` at 100000 both gates answer "as far away as it is possible to be", so the mist ran at
full strength over every pixel of the screen. A distance-gated term reading a missing distance is
not a distant term; it is an ungated one. This is the state the client is in whenever multisampling
is above 1x, which is also why the same session reported effects disappearing there.

## Fix

`smoothedIndoor` is now computed in `Present()` from the client's own weather emission, which the
draw hook already counts. WoW stops emitting weather particles under cover, so a frame that still
recognises the weather mode but saw no native weather draw is a frame with a roof in it. The evidence
has to be sustained for `coverEvidenceMs` (350 ms) before it counts, so a client emitting on
alternate frames, or one throttled frame, never trips it - either resets the clock. It is deliberately
silent while the game is unfocused or the mode detector is suspended, because absence of a draw there
means exactly what the five second hold already assumes it means: nothing. The factor is eased, not
switched: up in about half a second, down in about a second and a half, so a roof edge cannot strobe
a fullscreen pass.

The mist is gated on `weatherParams.w`, which now carries `hasCapturedDepth`. No depth means no
depth-gated term, which is the rule the occlusion factor already followed.

`GetStatusText()` appends `(COVER)` while the factor is above half, so the rain dimming under a roof
reads as the renderer working rather than as the effect failing.

## Verification

`tools/WindRegression.cpp` pins all five pieces of the wiring: the mist gate on `weatherParams.w`, the
constant upload, the eased update, the silence-while-unfocused condition, and the 350 ms evidence
window. It also still pins the phase split from the section above, so the two mechanisms cannot be
confused with each other again.

## In-game check

Stand in rain, then walk under a roof or into a cave. The added layer should fade out in about half a
second and F10's status should read `RAIN ON (COVER)`; walking back out should bring it back over
about a second and a half. It should never fade while you are standing in the open in rain, and never
fade because you alt-tabbed. Separately, run with Multisampling above 1x (no depth) and confirm the
low grey mist no longer sits over the whole screen - the drops themselves still draw, because they are
screen-space and never needed depth.

# Layered rainfall look (2026-10-17)

The current replacement rain stage is an analytical particle field, not the
older rain-atlas UV shader described above. Its three depth layers have long,
sparse near streaks, medium drops and many fine distant drops. The visual
rhythm takes inspiration from rain in Red Dead Redemption 2, but is restrained
for WoW's brighter, stylized art and D3D9 shader budget: no opaque rain sheet
or artificial ground splashes. Drops remain sharp at different resolutions,
with subtle lighting/refraction against dark and bright scenes.

The shared wind still owns the downwind direction. Camera translation now
shifts the three layers by different amounts (near > far) to avoid a flat
screen overlay when walking or riding. This bounded, double-precision phase
is rebased after camera loss, reset or teleports; camera rotation alone cannot
jump the rain. `RainParallaxPercent=0` disables this additional motion.

# Camera-motion stepping (2026-10-17)

The rain pass used `GetTickCount` for its fall clock and applied each captured
camera translation straight to the particle lattice. The coarse system tick
and intermittent camera captures could move the drops in visible jumps while
turning or riding. Rain now integrates with the same high-resolution
performance-counter clock used by the other animated effects. Camera samples
feed a short, frame-rate-independent eased target; positions and streak angles
continue updating between identical captures. Invalid captures, teleports and
device resets rebase without replaying pending camera displacement. The wind
and gravity phases are not reset.

# Rain underwater, and rain climbing under a diving camera (2026-10-19)

## Cause 1: the pass does not know where the camera is

The analytic rain field, the snow/sand continuity layer and the lens droplets are all composited
into the backbuffer during the client's draw stream, at the first UI draw, as a screen-sized quad.
Nothing in that pass knew the camera was under water, and the underwater medium composited later
then tinted the streaks along with the rest of the frame - rain drew at full strength over a
diver's view. The pass cannot read `FrameContext::underwaterRamp` for this: `ResetForNewFrame`
clears it at the top of every frame, and the value is only settled by the composite that runs
*after* the draw stream.

## Cause 2: the camera parallax had no floor

The camera translation added to each layer's phase is world-anchoring, not decoration: a drop four
yards away moves a long way up the screen when the camera drops, and the formula is right about
that. What it lacked was the other half of the picture. The drops are also falling, and the camera
term was unbounded, so a descent (swimming down, a mount losing height, an ordinary fall) added an
upward screen displacement several times larger than gravity's. The whole field climbed, and
`Motion::Direction` normalises the same sum, so the near streaks pointed up as well - the reported
"when I move the camera downward, rain drops go up".

## Fix

`FrameContext::compositedUnderwaterRamp` is the medium's ramp as the last composite settled it.
`VolumeIntegration::BeforeClear` publishes it, with the same gating as the composite's own publish
(`!shaftDebug && enabled && active && Underwater::IsActive()`), so with the underwater pass off it
is always 0 and nothing changes above or below the surface. It is one frame behind, which the
ramp's own third-of-a-second smoothing makes invisible. `BeforeDraw` turns it into
`aboveWater = 1 - ramp` and fades the rain field, the snow/sand continuity layer and the lens
droplets by it; the status line appends `(UNDERWATER)` for the same reason it appends `(COVER)`.

In `rainfield::Motion`, `Advance` now remembers the fall speed and projected wind it integrated, and
`TrackCamera` floors the *vertical* camera contribution per layer at
`-kMinimumCameraFallShare * layerSpeed * gravity * dt`. A descending camera can therefore slow the
pattern to 35% of its fall or stop adding to it, but it can never push the pattern - or the streak
axis, which is the same sum normalised - upward. Ascending and horizontal camera motion are
untouched, and the eased phase keeps working exactly as before.

## Verification

`tools/WindRegression.cpp` drives a descending camera for 120 frames at 30 yards a second and
requires every layer's phase to move down and every streak axis to point down, on top of all the
existing camera-parallax, teleport, rotation and long-session assertions (which the clamp leaves
unchanged, because they all move the camera up or sideways). Source checks pin the
`aboveWater` factor in all three composites, the ramp publication in `BeforeClear`, and the field in
`FrameContext`; `tools/UnderwaterRegression.cpp` pins the same wiring from the medium's side. A
scratch sweep (not shipped) covered fall speeds 0.1-4, projected wind of ±840, descent rates up to
144 yards a second and 30/60 Hz plus a 0.1 s clamped step, with no upward phase step in any layer.

## In-game check

Swim down, ride a mount down and jump off a ledge, in rain: the drops must keep falling downward
(they may slow), and once the camera is under the surface the rain, the snow/sand layer and the
lens droplets must fade out, with F10 reading `RAIN ON (UNDERWATER)`. Surfacing restores them; a
lake viewed from above still rains normally.
