# Volumetric clouds

A raymarched cloud deck for the sky, integrated into the scene colour before the atmosphere compositor rather than
painted over the finished frame. The field follows the Nubis/Horizon recipe - a Perlin-Worley base shape eroded by a
Worley detail volume, placed by a weather map, lit with a dual-lobe phase function and a multiple-scattering
approximation - inside a `ps_3_0` instruction budget.

## Why it is built this way

- **A spherical shell, not a slab.** The deck lies between `BaseAltitude` and `BaseAltitude + Thickness` on a shell
  of radius `PlanetRadius` (42000 units, about 2.5x the real Earth-to-WoW scale ratio) centred under the camera. A
  flat slab either ends in a visible straight edge or has to be pushed out of the way when the player flies; a shell
  meets the horizon with a curve and can be flown into and above. The shader handles three camera regions explicitly
  (below the deck, inside it, above it), so looking down at the tops from a flying mount works.
- **Real 3D noise, generated once.** A 64³ RGBA Perlin-Worley shape volume, a 32³ RGB Worley detail volume and a
  512² RGBA weather map are built on the CPU from fixed seeds the first time the renderer starts and cached next to
  the dll as `CloudFields.cache`. Fixed seeds mean every player sees the same sky; the cache means the cost is paid
  once rather than every launch. A cache written by an older build is rejected by its version field and rebuilt.
- **The weather map decides where clouds are at all.** Coverage, cloud type, a variation band and a storm mask come
  from independent integer-period Perlin bands (fronts and groups) multiplied by a masked Worley cell field. The
  periods are integers per axis because the field has to tile exactly over the 34 km weather tile - scaling a sample
  coordinate to stretch the fronts instead would seam the sky.
- **Sky pixels only.** The march returns `transmittance = 1` for every pixel whose depth is below the sky threshold
  (`0.9999`, the same constant the atmosphere's boundary pass uses), so the quarter-resolution buffer cannot bleed
  across a silhouette.
- **Before the atmosphere, not after.** The composite writes into the scene target and the scene copy is refreshed
  afterwards, so aerial perspective, the sun halo and the god-ray source mask all see the clouds. A deck in front of
  the sun dims its rays instead of letting them glow through it.

## Pipeline

| Pass | Resolution | Output |
| --- | --- | --- |
| Shadow | 256² top-down | transmittance of the deck above each world point |
| March | quarter (half on ULTRA) | `rgb` = in-scattered light, `a` = transmittance |
| Composite | full | `lerp(scene, scene * transmittance + scattering, skyMask)` |

The march itself:

1. Reconstruct the world ray from `projUnpack` and the inverse view rows, and intersect the two shell radii for the
   camera's region.
2. Fade grazing rays near the horizon and fade the far end of the march, so neither the shell nor the march limit
   ends in a hard edge.
3. Jitter the first step with an interleaved-gradient offset derived from `VPOS` - fixed in screen space, never a
   frame counter, so the bilinear upsample averages neighbouring phases instead of flickering.
4. March with a **step tied to world length that grows with distance**, never the span divided by a step count: the
   shell is 2150 units thick but 25000 units long near the horizon, and dividing the span is what turns detail into
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

## Settings

`[VolumetricClouds]` in `GraphicsEffects.ini`, per location like the rest of the tuning (`LocationTuning`), and on
the F7 menu in the first column.

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Master switch for the pass. |
| `Quality` | 2 | 0 = LOW (48 steps), 1 = BALANCED (64), 2 = HIGH (96), 3 = ULTRA (128, half-res). Presets only set defaults; every value below stays overridable. |
| `SampleCount` / `LightSampleCount` / `BaseStep` / `ResolutionPercent` | preset | March budget overrides. |
| `CoveragePercent` | 50 | How much of the sky the deck covers. 0 skips the pass entirely. |
| `DensityPercent` | 100 | Optical thickness of the cloud material (base extinction 0.022/unit). |
| `CloudTypePercent` | 0 | -100 flattens everything into stratus, 0 is cumulus, +100 builds towering cumulonimbus. |
| `PlanetRadius` | 42000 | How fast the deck curves away to the horizon. |
| `BaseAltitude` | 850 | World Z of the cloud base. |
| `Thickness` | 2150 | Height of the layer. |
| `FeatureSize` | 3000 | World size of one tile of the base shape volume. |
| `ShapeAspectPercent` | 130 | Vertical repeats of the shape volume per horizontal tile. |
| `DetailSize` | 240 | World size of one tile of the erosion volume. |
| `WeatherSize` | 34000 | World size of one tile of the weather map. |
| `DetailPercent` | 88 | Strength of the erosion that shreds cloud edges. |
| `WindSpeed` / `WindAngle` / `WindShearPercent` | 26 / 25 / 22 | Drift speed, direction, and how far the tops lean downwind. |
| `AbsorptionPercent` | 38 | Self-shadowing strength along the light march. |
| `SunIntensityPercent` | 130 | Direct sun/moon contribution. |
| `AmbientPercent` | 105 | Sky ambient contribution, tinted by the zone fog colour. |
| `MoonStrengthPercent` | 30 | Direct contribution at night relative to daylight. |
| `MultiScatterAttenuationPercent` / `ContributionPercent` / `EccentricityPercent` | 60 / 92 / 55 | The three octave scalars. |
| `ForwardScatterPercent` / `BackwardScatterPercent` | 78 / 32 | The two phase lobes. |
| `SilverLiningPercent` | 55 | Bright rim on a backlit edge. |
| `PowderPercent` | 70 | Dark edge where a cloud faces the light. |
| `StepGrowthPermille` / `CoarseStepScale` / `JitterPercent` | 10 / 3 / 25 | March shape. |
| `HorizonFadePermille` | 18 | How quickly grazing rays fade out. |
| `MaxDistance` | 26000 | March limit; the deck fades out before it. |
| `AerialPerspectivePercent` | 60 | How much of the sky's haze a cloud picks up with distance. 0 makes near and far clouds equally contrasty. |
| `GroundShadows` / `GroundShadowPercent` / `GroundShadowExtent` | 1 / 55 / 14000 | Cloud shadows cast on the world below. |
| `DebugMode` | 0 | 1 = raw low-resolution buffer, 2 = coverage, 3 = direct lighting only, 4 = shadow map. |

Rain and snow add coverage while the weather layer is live (`volume::cloudWeatherCoverageBoost`). The boost is
applied to a copy of the settings, so the player's slider is never rewritten.

## Looking at changes without a GPU

`tools/CloudPreview.cpp` is a CPU mirror of the shader - the same noise generators from `src/Effects/CloudNoise.h`,
the same density function, the same light march and the same scattering - that renders a PNG with `g++ -O2
-std=c++20 -pthread`. Every tuning decision in this document was made by looking at its output first, because the
shader itself can only be compiled in CI.

```sh
g++ -O2 -std=c++20 -pthread tools/CloudPreview.cpp -o build/cloudpreview
build/cloudpreview out.png --coverage 45 --sun-elevation 30 --stats

CLOUDSHEET_BASE="--width 420 --height 236" python3 tools/cloudsheet.py sheet.png \
  "overcast=--coverage 92" "broken=--coverage 45" "scattered=--coverage 20" "sunset=--sun-elevation 6"
```

`tools/cloudsheet.py` renders the variants in parallel and stitches them into one labelled contact sheet, which is
the only practical way to compare four parameter choices at once.

## Cost

Three passes, all timed as `GpuPerfStage::VolumetricClouds`: a 256² shadow map, one quarter-resolution march (at
1920x1080 that is 480x270 rays) and one full-resolution composite.

`tools/ValidateShaders.ps1` compiles every shader literal with `fxc` in CI and fails the build if one exceeds its
budget. Measured on the CI runner against the 512-slot `ps_3_0` ceiling: **march 466**, shadow 116, composite 66
(for scale, the atmosphere's integrate pass is 462).

Two constraints shape the march code and are worth knowing before editing it:

- `ps_3_0` has 224 float constants but only 512 instruction slots and 32 temp registers. Constants are free;
  instructions are not. Anything new has to be paid for - the long reach light sample was folded into the light loop
  to pay for aerial perspective.
- `[unroll]` around anything that inlines the density sampler more than about twice fails with `X4505: maximum temp
  register index exceeded`. The light march is a real `[loop]`; `ps_3_0` allows four levels of nesting.

## Tests

`tools/AtmosphereRegression.cpp` (run with `tools/Test-Atmosphere.cmd`, needs a real D3D9 device) covers the deck on
the production classes with a synthetic depth buffer - sky in the top half, geometry in the bottom half:

- the overcast deck is visible, takes the environment ambient tint, and leaves geometry pixels byte-identical;
- `DensityPercent` drives the integrated opacity (measured through the coverage diagnostic);
- a noise field below the coverage threshold leaves the sky exactly as drawn;
- the weather boost raises coverage without mutating the configured value;
- the clearance clamp keeps the deck above a camera flown past the authored altitude;
- disabled clouds and zero coverage do not touch the target at all;
- `c0..c63`, the viewport and the device resources survive the pass and a device reset.
