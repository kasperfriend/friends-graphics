# Volumetric clouds

A raymarched cloud deck for the sky, integrated into the scene colour before the atmosphere compositor rather than
painted over the finished frame. The field follows the Nubis/Horizon recipe - a Perlin-Worley base shape eroded by a
Worley detail volume, placed by a weather map, lit with a dual-lobe phase function and a multiple-scattering
approximation - inside a `ps_3_0` instruction budget.

## Why it is built this way

- **A fixed world-height layer.** The visible deck lies between absolute world Z `BaseAltitude` and
  `BaseAltitude + Thickness`, never an offset above the camera or local terrain. You can fly toward, through, and
  above clouds without the layer moving out of the way. Looking around changes only the viewing ray, not the field.
  Horizontal rays inside the deck integrate normally; outside it they miss. Horizon/distance fades hide the far
  march limit. `PlanetRadius` is retained for legacy shadow parameters but no longer shapes the visible deck.
- **Real 3D noise, generated once.** A 64³ RGBA Perlin-Worley shape volume, a 32³ RGB Worley detail volume and a
  512² RGBA weather map are built on the CPU from fixed seeds the first time the renderer starts and cached next to
  the dll as `CloudFields.cache`. Fixed seeds give reproducible fields (animation time is local to the session); the cache means the cost is paid
  once rather than every launch. A cache written by an older build is rejected by its version field and rebuilt.
- **The weather map decides where clouds are at all.** Coverage (R), cloud type (G) and a variation band (B) come
  from independent integer-period Perlin bands (fronts and groups) multiplied by a masked Worley cell field, each
  histogram-equalised so the coverage slider has a predictable response. The storm mask (A) is built afterwards from
  the equalised channels - coverage and type high at once, the deep core of a tall mass - and is reserved: nothing
  samples it yet. The
  periods are integers per axis because the field has to tile exactly over the 34 km weather tile - scaling a sample
  coordinate to stretch the fronts instead would seam the sky.
- **Clipped by the scene, not skipped on it.** The march reconstructs the distance to whatever the client already
  drew and stops there. A sky-only pass - which is what this started as - deletes every cloud between the camera and
  a surface: fly above the deck and you get terrain with cloud shadows on it and no clouds. It also keeps the common
  case cheap, because a ground pixel thirty units away ends before it enters the layer and bails on the first
  test. The composite pays for this with a depth-aware upsample (below).
- **World-anchored ground shadows.** The inexpensive shadow field uses a local planar approximation to the deck,
  integrated from a fixed sea-level reference plane. Its XY origin is snapped to texels; each terrain/object
  receiver is projected along sunlight according to its own world height. Moving the camera vertically does not
  change the cloud optical path or darken/lighten the entire world. Shadows fade out above the deck and at the map
  border. The ground-shadow pass is unchanged by the fixed-height cloud update and still omits high-frequency erosion.
- **Cloud-filtered sun rays.** The atmosphere samples that same moving transmittance field at eight positions along
  each depth/water-clipped view ray. Sunlight passes through the gaps while dense clouds block it, including when
  the sun is off screen. The radial glare source separately samples the cloud buffer's alpha, never cloud brightness.
  Rays require the atmosphere's global fog and the SUN RAYS master toggle; no scattering is drawn through walls.
- **Before the atmosphere, not after.** The composite writes into the scene target and the scene copy is refreshed
  afterwards, so aerial perspective, the sun halo and the god-ray source mask all see the clouds. A deck in front of
  the sun dims its rays instead of letting them glow through it.

## Pipeline

| Pass | Resolution | Output |
| --- | --- | --- |
| Shadow | 256² top-down | transmittance of the deck above each world point |
| March | configurable 20–100% of width/height | `rgb` = in-scattered light, `a` = transmittance |
| Composite | full | `scene * transmittance + scattering`, depth-aware upsample of the march |

The march itself:

1. Reconstruct the world ray from `projUnpack` and the inverse view rows, and intersect the two fixed world-Z
   planes, clamping entry to zero when inside the deck.
2. Fade grazing rays near the horizon and fade the far end of the march, so neither the layer nor the march limit
   ends in a hard edge.
3. Jitter the first step with an interleaved-gradient offset derived from `VPOS` - fixed in screen space, never a
   frame counter, so the bilinear upsample averages neighbouring phases instead of flickering.
4. March with a **step tied to world length that grows with distance**, never the span divided by a step count: the
   layer is 2150 units thick but 25000 units long near the horizon, and dividing the span is what turns detail into
   putty. Empty space is crossed with strides three times longer; the march steps back and integrates properly as
   soon as it hits density, and returns to striding after seven empty samples.
5. At each sample with density: a six-step Beer march toward the sun or moon whose last step is a long reach sample
   (a tower far up the light path still has to cast), three scattering octaves with geometrically scaled extinction,
   contribution and phase, a powder term, and a vertical ambient gradient from the zone's own fog colour.
6. Accumulate a transmittance-weighted mean distance alongside the radiance, and apply aerial perspective once to
   the integrated result.

## Field shape

Density at a point is

```
base    = Remap(shape.r, (shape.g*.625 + shape.b*.25 + shape.a*.125) - 1, 1)   // Perlin-Worley
profile = analytic vertical gradient for the cloud type at this point
d       = Remap(base * profile, 1 - coverage, 1) * coverage
d       = Remap(d, detailFbm * strength * (1 - d*.7), 1)                       // erosion
```

Three details matter more than they look:

- **`ShapeAspectPercent` is how many times the base volume repeats vertically per horizontal tile.** It shipped at
  320 in the first build, which made vertical features three times smaller than horizontal ones and flattened every
  cloud into a lozenge. 130 gives billows that are about as tall as they are wide.
- **Erosion fades inside the core** (`* (1 - d*.7)`). Eroding uniformly either leaves the silhouette smooth or
  hollows the cloud out. Raising `DetailPercent` towards 1 does not produce wisps either - the `Remap` normalisation
  divides by `1 - strength`, so it just hardens edges into binary cut-outs.
- **The coverage window closes quadratically at the top.** The slider slides a window `[1 - c*1.5, 1 - c²*1.05]`
  along the weather map's equalised coverage channel. With a linear high edge the window still ends at 0.6 and half
  the sky keeps broken cloud no matter how far the slider is pushed, so overcast was unreachable.

## Lighting

| Term | Role |
| --- | --- |
| Dual-lobe Henyey-Greenstein | `lerp(HG(g=.78), HG(g=-.32), .35)` - a forward peak for the silver lining and a broad backward lobe so the anti-solar side is not flat |
| Silver lining | `SilverLiningPercent * pow(saturate(cosTheta), 24)` on top of the phase |
| Multiple scattering | Wrenninge's three octaves: extinction `a^n`, contribution `b^n`, phase relaxed toward isotropic by `c^n` |
| Powder | `lerp(1, 1-exp(-d*18), powder * sun-facing)` - the dark edge pure Beer absorption cannot reproduce |
| Ambient | `lerp(groundBounce, skyColour, h^0.7)`, both derived from the zone's authored fog colour, closing down by `1 - 0.45*coverage` because less sky reaches the base of an overcast lid |
| Aerial perspective | `1 - exp(-meanDistance / (MaxDistance/2))` blends the radiance toward the sky's haze colour and lifts transmittance by `AerialPerspectivePercent` |

Dark interiors are an optical-depth problem on the **light** path, not a multiple-scattering problem: at
`AbsorptionPercent 75` the cone optical depth reaches 13-22 and every multiple-scattering octave underflows anyway.
Absorption is 38 instead, which decouples self-shadowing strength from view opacity.

Direct lighting deliberately does **not** come from whether the sun/moon disc was drawn this frame. That is a
screen-space event - WoW culls the billboard outside the frustum and the tracker can miss frames at the screen
edge - so gating the deck's light on it lit and unlit the whole sky as the camera panned, and the ground shadow
toggled with it. The compositor builds a stabilized direct-light signal (`CelestialLightStabilizer`) from
**scene lighting only**. Daylight/moonlight factors and directional luminance select the dominant body and
strength, with hysteresis at twilight and a short fade for real lighting transitions. A visible disc cannot
switch the world light to the other body or force it on in a dark scene. Only the radial god-ray/glare origin
uses the visible-disc selection; world-space rays do not require an on-screen disc.

World directions are tracked independently for both bodies, including when the other body supplies the glare.
Each confirmed direction is retained when its billboard is culled; actual direction updates are gently
smoothed. Before a body has been observed, lighting uses a fixed overhead approximation, replaced by its
first valid tracked direction. This is deliberately approximate until acquisition: the old camera-derived
light constant is not a safe world-space source. Even rate-limiting it to 2.5 degrees per second let a stationary
player's camera turn steer the light all the way to the horizon and remove ground shadows within 40 seconds.
Cloud-shadow strength additionally fades in with sun elevation instead of snapping at the horizon gate.

`tools/DeviceHealthRegression.cpp` covers every sun/moon visibility combination in daytime and at night,
long off-screen intervals, initial acquisition, invalid observations, and real day/night transitions. It runs
in the portable cloud CI job. The D3D9 atmosphere regression additionally checks that camera yaw/pitch and
legacy disc signals leave the world shadow texture unchanged with wind and evolution frozen.

## Settings

`[VolumetricClouds]` in `GraphicsEffects.ini`, per location like the rest of the tuning (`LocationTuning`), and on
the F7 VOLUMETRIC CLOUDS tab.

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Master switch for the pass. |
| `Quality` | 3 | 0 = LOW (48 steps), 1 = BALANCED (64), 2 = HIGH (96), 3 = ULTRA (128). Owns the sample count, light samples and base step together. Old `SampleCount` / `LightSampleCount` / `BaseStep` INI keys are ignored; they made F7 quality inert on existing installs. |
| `ResolutionPercent` | 75 | Independent F7 slider, 20–100% of screen width and height in steps of 5. Resizes live without changing march samples; Quality never changes this resolution. |
| `SunRaysEnabled` / `SunRayPercent` | 0 / 80 | Cloud-filtered world-space shafts, toggle and 0–200% strength in F7. SUN RAYS is also a master gate. |
| `CoveragePercent` | 50 | How much of the sky the deck covers. 0 skips the pass entirely. |
| `ZoneClouds` / `WeatherClouds` | 1 / 1 | Apply zone climate / native weather independently; also in F7. |
| `EvolutionPercent` | 100 | 0–400% independent shape evolution; 0 freezes evolution, not wind. Also in F7. |
| `TransitionSeconds` | 12 | Approximate 95% settling time for zone/manual shape edits, 0–60 seconds; 0 is immediate. |
| `DensityPercent` | 100 | Optical thickness of the cloud material (base extinction 0.022/unit). |
| `CloudTypePercent` | 0 | -100 flattens everything into stratus, 0 is cumulus, +100 builds towering cumulonimbus. |
| `PlanetRadius` | 42000 | Legacy shadow parameter; no effect on visible cloud height. |
| `BaseAltitude` | 850 | World Z of the cloud base. |
| `Thickness` | 2150 | Height of the layer. |
| `FeatureSize` | 3000 | World size of one tile of the base shape volume. |
| `ShapeAspectPercent` | 130 | Vertical repeats of the shape volume per horizontal tile. |
| `DetailSize` | 240 | World size of one tile of the erosion volume. |
| `WeatherSize` | 34000 | World size of one tile of the weather map. |
| `DetailPercent` | 88 | Strength of the erosion that shreds cloud edges. |
| `WindSpeed` / `WindShearPercent` | 26 / 22 | Cloud-local drift speed at shared wind strength 100% and shear gain. The optional, unshipped `WindAngle` (legacy default 25) is a sampling angle used only when unlinked from `[Wind]`. See [shared wind](wind-system.md). |
| `AbsorptionPercent` | 38 | Self-shadowing strength along the light march. |
| `SunIntensityPercent` | 130 | Direct sun/moon contribution. |
| `AmbientPercent` | 105 | Sky ambient contribution, tinted by the zone fog colour. |
| `MoonStrengthPercent` | 30 | Direct contribution at night relative to daylight. |
| `MultiScatterAttenuationPercent` / `MultiScatterContributionPercent` / `MultiScatterEccentricityPercent` | 60 / 92 / 55 | The three octave scalars. |
| `ForwardScatterPercent` / `BackwardScatterPercent` | 78 / 32 | The two phase lobes. |
| `SilverLiningPercent` | 55 | Bright rim on a backlit edge. |
| `PowderPercent` | 70 | Dark edge where a cloud faces the light. |
| `StepGrowthPermille` / `CoarseStepScale` / `JitterPercent` | 10 / 3 / 25 | March shape. |
| `HorizonFadePermille` | 18 | How quickly grazing rays fade out. |
| `MaxDistance` | 26000 | March limit; the deck fades out before it. |
| `AerialPerspectivePercent` | 60 | How much of the sky's haze a cloud picks up with distance. 0 makes near and far clouds equally contrasty. |
| `GroundShadows` / `GroundShadowPercent` / `GroundShadowExtent` | 1 / 55 / 24000 | Cloud shadows cast on the world below. |
| `DebugMode` | 0 | 1 = raw low-resolution buffer, 2 = coverage, 3 = direct lighting only, 4 = shadow map. |

### Zone character, weather and evolution

`EnvironmentProfiles.ini` now ships **88 zone assignments** and eight cloud-only
climates. They leave existing fog, lighting and precipitation multipliers alone:

| Climate | Character | Examples |
| --- | --- | --- |
| CloudFair | Smaller, separated fair-weather cumulus | Elwynn, Westfall, Mulgore, Nagrand |
| CloudArid | Sparse, small, shallow clouds | Tanaris, Durotar, Barrens, Hellfire |
| CloudDark | Broad, heavy, mostly flat decks | Duskwood, Plaguelands, Shadowmoon |
| CloudCoastal | Sweeping stratus sheets | Darkshore, Wetlands, Azuremyst, Zangarmarsh |
| CloudCold | Broken, subdued low decks | Dun Morogh, Winterspring, Dragonblight |
| CloudMountain | Deeper, taller billows | Storm Peaks, Alterac, Howling Fjord |
| CloudLush | Rounded, vertically developed masses | Feralas, Un'Goro, Sholazar |
| CloudOpen | No fair-weather deck; native sky remains exposed | Netherstorm, Twisting Nether |

Northrend has a cold map fallback; other unknown/custom zones inherit neutral
settings. Zone assignments use IDs from the shipped authoritative database, not
name guesses at runtime. The existing verified-client/database gate, location
debounce, inheritance and environment transition remain in force. Unverified
clients retain manual clouds, native weather detection where supported, and
procedural evolution; they do **not** apply guessed zone IDs.

Resolution order:

1. Existing `GraphicsEffects.ini` → map/zone/area `LocationGraphics.ini` cloud settings.
2. `EnvironmentProfiles.ini`: Default → Map → Zone → Area, including named inheritance.
3. Smooth the physical shape (quality, debug and master-enable flags remain immediate).
4. Blend native weather over that zone's fair-weather baseline.
5. Integrate wind and evolution; upload the same field constants to sky and shadows.

New environment keys: `CloudCoverageMultiplier` (0–2), `CloudTypeOffset` (-1–1),
`CloudFeatureSizeMultiplier` and `CloudThicknessMultiplier` (.25–4),
`CloudDetailMultiplier` (0–2), `CloudEvolutionMultiplier` (0–4).
Water shares this layer: `WaterAgitationMultiplier` (0-4, default 1) is a zone's own sea state,
documented in `docs/water-rendering.md`; a zone line that names only it keeps its cloud climate.

Multipliers default to 1, type offset to 0. Missing keys in old configs preserve
manual tuning; existing installations must merge the shipped climate sections
into their environment file to get these curated assignments. Reload with F12.
An area can override individual cloud keys without replacing its zone's profile.
Cloud type now interpolates toward **pure stratus at -100** and **pure towers at
+100**, retaining weather-map type variation between the endpoints.

Weather consumes the existing exact native rain/snow/sand draw signatures and
five-second detection hold. Detection no longer depends on enabled precipitation
replacement or successful effect-texture loading. The old WeatherVisuals Mode
hint has been removed: it could change continuity-layer snow into rain while
clouds and native particles still saw snow. The verified signal is the
**type/presence**, not server
severity: no primitive count is used as an invented intensity measurement.
`WeatherVisuals.IntensityPercent × WeatherIntensityMultiplier`, clamped to 0–1,
scales the cloud response; the default reaches full overcast. Rain builds deeper
billows, snow a flatter deck, sand a dusty lid. The procedural weather map still
provides local coverage, type and variation within that deck.

Weather approaches its target over ~18 seconds (95%) and clears over ~35 seconds,
after the detector hold. A normally empty sky can still develop a storm. To keep a
strictly manual sky use `ZoneClouds=0` and `WeatherClouds=0`; `Enabled=0` is the
immediate master off switch. `CoveragePercent=0` alone clears the **fair-weather
baseline**, not native storms. Zone/manual edits settle over `TransitionSeconds`
in addition to the environment manager's existing transition.

Animation uses independent, slowly moving shape/detail Z slices and a slower
weather-map drift. Its B channel gently opens/closes local patches; the variation
fades out at full overcast so storms never acquire animated holes. These are
noise-coordinate changes, **not changes to the world-height profile**. Wind and
evolution integrate bounded frame deltas into wrapped double-precision phases;
changing wind no longer recomputes displacement from the entire session age.
INI reloads and device resets preserve phases. Shared wind now supplies the
travel direction and strength; sampling offsets move opposite travel so the
clouds and their shadows move downwind, like the waves. F7 → WIND controls the
shared state, while the cloud wind-speed setting remains a local drift gain.
For a frozen field, set evolution to zero and shared base wind strength to zero,
disable weather-driven coverage, and allow transitions to settle. When unlinked,
set cloud `WindSpeed=0` instead. See [shared wind](wind-system.md).

No extra noise textures, texture fetches per density sample, ray steps or render
passes are added. One shared constant register and a few arithmetic instructions
extend the existing density functions. Shadow integration starts at the actual cloud base rather than sampling a
sea-level copy of the noise field. It still omits detail erosion; it follows the same evolving bodies, coverage and type, and the ray pass
continues to use that shadow map. `Clouds.log` reports effective coverage/type,
feature size, weather blend and evolution phase for live diagnosis.

## Looking at changes without a GPU

`tools/CloudPreview.cpp` is a CPU mirror of the shader - the same noise generators from `src/Effects/CloudNoise.h`,
the same density function, the same light march and the same scattering - that renders a PNG with `g++ -O2
-std=c++20 -pthread`. Every tuning decision in this document was made by looking at its output first, because the
shader itself can only be compiled in CI.

```sh
g++ -O2 -std=c++20 -pthread tools/CloudPreview.cpp -o build/cloudpreview
build/cloudpreview --out build/cloud-fair.png --coverage 45 --type 15 --feature-size 2250 --top 2570
build/cloudpreview --stats --coverage 100 --type 80 --time 300

```

For stationary-camera evolution comparisons use `--wind-speed 0 --time 0` and
`--wind-speed 0 --time 300`; `--evolution 0` freezes shape evolution. The density
reference now matches the production erosion denominator and endpoint type
mapping. These are offline reference renders, not in-game screenshots; the
preview mirrors the shadow-column integral, not the GPU map upsample/composite.

## Cost

Three passes, all timed as `GpuPerfStage::VolumetricClouds`: a 256² shadow map, one quarter-resolution march (at
1920x1080 that is 480x270 rays) and one full-resolution composite.

`tools/ValidateShaders.ps1` compiles every shader literal with `fxc` in CI and fails the build if one exceeds its
budget. CI holds the march to 496 slots against the 512-slot `ps_3_0` ceiling: these shaders are compiled at
runtime by whatever `d3dcompiler_47.dll` the player's Windows ships, which does not have to agree with the CI
runner's `fxc` about instruction selection.

Two constraints shape the march code and are worth knowing before editing it:

- `ps_3_0` has 224 float constants but only 512 instruction slots and 32 temp registers. Constants are free;
  instructions are not. Anything new has to be paid for - the long reach light sample was folded into the light loop
  to pay for aerial perspective.
- `[unroll]` around anything that inlines the density sampler more than about twice fails with `X4505: maximum temp
  register index exceeded`. The light march is a real `[loop]`; `ps_3_0` allows four levels of nesting.

## What it looks like

![Volumetric clouds: scattered, broken and backlit, overcast, and the deck seen from above](../Screenshots/volumetric-clouds.png)

Four frames from `tools/CloudPreview.cpp` at the shipped defaults: scattered cumulus at coverage 30, a backlit
broken deck at 50, an overcast lid at 92, and the same sky seen from 3600 units up. All four are the same field and
the same lighting - only the slider and the camera move.

## Zone changes and the weather deck (2026-10-19)

### What was being seen

After a burst of teleports the deck sat at full coverage in every location and never
changed - no transition, no clear sky, just an overcast lid wherever the player went.

### Where the coverage came from

Two things feed coverage. The first is the climate: the authored `CoveragePercent`
slider times `CloudCoverageMultiplier` from the resolved environment profile, eased by
`CloudDynamics` toward the new target over `TransitionSeconds` - a normal transition.
The second is precipitation: `result.coverage += (1-result.coverage)*wet`, where `wet`
is the decaying weight of the client's detected rain/snow/sand. At `wet=1` the deck is
***full*** overcast regardless of the zone's climate.

That second state is deliberately slow: when the native particles stop, the detection
holds for `nativeWeatherHoldMs` (30 s, so a doorway or a particle gap cannot blink the
pass) and the wet weight then eases out over its own 35 s response. Neither of those was
ever cleared on travel, because nothing told the weather state that the player had left
the place the rain belonged to. A teleport therefore carried the old zone's rain
evidence across, and a clear destination inherited a full deck for the hold plus the
clearing ramp - and a second teleport re-armed nothing, so it stayed that way.

### The fix

A confirmed **zone or map** change is a discontinuity, not a trend:

- `weathervisuals::ForgetLocation()` drops the detected weather (`nativeWeatherMode`,
  the per-frame observation, the hold tick and `weatherSuspended`) and the cover
  estimate. The particle evidence that made it true belonged to the old zone; the new
  zone's own draws set it again within a frame or two.
- `volume::ForgetWeather()` clears the frame's published weather and, through
  `VolumetricClouds::ForgetWeather()`, the `CloudDynamics` wet vector
  (`m_wet = {0,0,0}`) that had pinned coverage to 1.
- The climate filter is untouched: `m_current` still eases toward the new zone's
  `CloudCoverageMultiplier`, which is the transition the report expected to see.
- Only changes between two **valid** samples count. A loading screen's invalid sample
  is travel, not a new sky, so it neither fires the reset nor masks the real change on
  landing. Sub-zone (area) changes are ignored on purpose: walking between two districts
  of one city is the same sky.

The call site is in `Present`, immediately after
`EnvironmentProfileManager::Update` has confirmed the location, and it logs
`Weather re-armed: the zone changed...` once per move.

### Verification

`tools/CloudClimateRegression.cpp` drives a deck to `wet=1`/coverage 1, calls
`ForgetWeather()`, and requires the next frame to be at the zone's own coverage (`0.5`
with neutral multipliers) and to rebuild when rain continues; source pins hold the two
reset entry points and the zone-only trigger in `ModernWoWRenderer.cpp`.

## Tests

`tools/AtmosphereRegression.cpp` (run with `tools/Test-Atmosphere.cmd`, needs a real D3D9 device) covers the deck on
the production classes with a synthetic depth buffer - sky in the top half, geometry in the bottom half:

- the overcast deck is visible, takes the environment ambient tint, and leaves geometry pixels byte-identical;
- `DensityPercent` drives the integrated opacity (measured through the coverage diagnostic);
- a noise field below the coverage threshold leaves the sky exactly as drawn;
- the weather boost raises coverage without mutating the configured value;
- a camera inside the deck sees clouds in front of distant geometry; above the deck, the overhead sky is clear;
- disabled clouds and settled zero coverage without weather do not touch the target at all;
- `c0..c63`, the viewport and the device resources survive the pass and a device reset.

`tools/CloudLayerRegression.cpp` is a portable CPU-reference check for fixed world altitude, approach distance,
entry/exit from below/inside/above, horizontal rays, view direction changes, and scene-depth clipping:

```sh
mkdir -p build
g++ -O2 -std=c++20 -pthread tools/CloudLayerRegression.cpp -o build/cloud-layer-test
./build/cloud-layer-test
```

This checks reference math and the production height/bounds expressions; it does not replace HLSL compilation
or an in-game flight test. Ground-shadow rendering is deliberately unchanged by this fix.


### Climate regression

The portable test exercises the production climate parser/resolver/controller
and samples the actual generated noise fields. It validates shipped zone IDs,
inheritance, neutral fallback, signed/zero parameters, transactional reloads,
rapid border reversals, 30/144 Hz weather transitions, zero-coverage storms,
clearing, zero-wind evolution, phase bounds, and frame-to-frame density continuity.
All eight climates are checked against rain, snow and sand for >98% covered
vertical columns. This is a density/coverage test, not a substitute for in-client
lighting/performance review.

```sh
mkdir -p build
g++ -O2 -std=c++20 -pthread tools/CloudClimateRegression.cpp \
  src/Environment/EnvironmentProfile.cpp src/Environment/EnvironmentDatabase.cpp \
  -o build/cloud-climate-test
./build/cloud-climate-test
```

CI runs this test, the fixed-layer checks and frame-clock regression on Linux;
Windows CI builds Debug/Release and validates embedded shaders against ps_3_0
budgets. `tools/Test-Atmosphere.cmd` remains the Windows GPU/state regression.
Live acceptance: cross Elwynn/Duskwood and Barrens/Tanaris borders; observe native
rain/snow/sand onset/clearing; check a zero-wind sky several minutes apart; fly
below/inside/above; pan off the sun; toggle F7/F10, reload F12 and Alt+Tab. Confirm
no field restart, boundary pop or cloud/shadow disagreement. Native weather
recognition still requires a supported client draw signature.

### Camera pose freshness: cities, interiors, and looking up

The deck is anchored at absolute world heights, so "the clouds are dragged along with the camera" can only
mean the *frame's camera* was wrong while the client's depth buffer was the real one. That happens where no
draw ever carries a full-frame rigid camera - most of Orgrimmar seen from street level, with the terrain over
the player's head frustum-culled - and it clears up on higher ground because a real capture latches there.

`CameraCapture` now treats every sky/draw transform it sees as a *candidate* for the frame, not as the camera:

- The transform must be a rigid basis: unit-length, mutually orthogonal rows. A scaled billboard's rows are
  not the camera's axes.
- Its view translation must either be zero (the sun's skybox trick, where the rows ARE the camera's rotation)
  or imply an eye within `kSkyEyeLeashYards` (40 yd) of the last accepted camera. The moon's billboard sits at
  a fixed 4-16 yd render distance along the light and its rotation is not the camera's, so it is dropped rather
  than merely ignored; that is what used to make the deck swing with the mouse.
- Candidates are collected for the whole frame (`SkyPoseSelector`) and resolved once at the UI draw, when the
  frame's own sun/moon disc has been painted. A candidate that reproduces the painted disc (within 0.18 of
  normalized screen units) wins, using `ProjectWorldDirection`, which projects through the view matrix's
  COLUMNS - the same convention as `CelestialTracker` and `MathTypes`' row-vector transform.
- Without a disc, continuity with the accepted camera decides (`BasisAgreement >= .75`). A discontinuous
  transform is held for two frames and only adopted if the frame repeats it; one that changes every frame is
  never adopted.
- The eye is always `CameraContinuityGate::Position()` - the last position a real draw carried. A sky
  transform's translation never moves it.

Frames that still cannot be corroborated publish `FrameContext::cameraPoseStale` and keep the previous
orientation for screen-space passes. Every pass that places something in *world* space must refuse
(or hold) that frame:
`VolumetricClouds::Render` refuses a stale pose outright; `LocalLightingRenderer::Render` and
`LocalLightManager::ObserveTorchDraw` refuse it while `cameraEyeAgeMs` also exceeds `kCameraEyeHoldMs`, so a
pose held for a frame keeps the lamps steady (a frame-late lamp is invisible, a lamp placed through a
second-old eye is in the wrong street) while a genuinely orphaned pose is never drawn from. `capturedViewValid` and
`cameraCaptureShaderHash` are now cleared every frame and published only by a capture that actually succeeded,
so `DrawCallClassifier` cannot inherit a failed attempt's shader as "terrain".

`tools/DeviceHealthRegression.cpp` pins the projection convention, the disc contradiction, jump confirmation,
the empty-frame case, and the source-level guards on the deck, the lamp pool and the torch emitters.

### The celestial disc, verified against geometry (2026-10-19)

The client draws the moon as a small camera-facing billboard at a fixed 4-16
yards along the light, nearer than almost all world geometry. The cloud pass has
to treat those pixels as clear sky or the deck is cut by a dark disc, and it
identified them by "geometry whose depth matches the sprite's". That test is
ambiguous: a floor, wall or roof inside the same screen circle at the same
distance matches too - seen from below in a city, the moon's disc then appears
punched through the ground and the deck marches through it.

`src/Effects/CelestialDiscGate.h` supplies the discriminator, mirrored by the
ps_3_0 helpers in `VolumetricClouds`: a camera-facing billboard has ONE view
depth across its face, so its raw depth is constant from centre to rim, while
an angled surface is not. The centre and a rim tap on each axis must agree
within the sprite's own depth tolerance before any pass treats geometry as the
moon (`moonSprite`, c26 in the march and c12 in the composite).

Two related gates, both from the camera-freshness work above:

- `FrameContext::moonDiscValid` is false while `cameraPoseStale` - the tracked
  disc position was measured against a camera this frame is not using.
- The atmosphere's halo and its disc protection are disabled for such a frame
  (`sourceScreen.w`), so a stale screen position cannot paint a sun onto
  whatever now occupies that pixel.
