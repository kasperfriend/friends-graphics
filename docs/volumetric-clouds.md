# Volumetric clouds

A raymarched cloud deck for the sky, integrated into the scene colour before the atmosphere compositor rather than
painted over the finished frame.

## Why it is built this way

- **One world-space slab, not a dome.** The deck is an absolute band in world space (WoW world Z is up) between
  `Altitude` and `Altitude + Thickness`. A camera-relative dome slides with the player and makes flight feel like the
  sky is running away; an absolute slab stays where it was authored. The band is still clamped to at least
  `MinimumClearance` above the camera, so a mountain top or a flying mount can never end up inside or above the layer,
  where a sky-only pass has nothing sensible to show.
- **No 3D volume texture.** Density comes from the same tiling 128x128 value-noise atlas the atmosphere already
  creates (`EnsureNoise` in `VolumeIntegration.h`). Two bilinear slices offset by `(.173, .317)` and lerped on a
  smoothstepped fraction give a cheap 3D field. DX9-class hardware is not guaranteed to have the bandwidth for a real
  volume texture, and this costs no extra VRAM at all.
- **Sky pixels only.** The march returns `transmittance = 1` for every pixel whose depth is below the sky threshold
  (`0.9999`, the same constant the atmosphere's boundary pass uses). Everything the client already drew keeps the
  scene exactly, so the quarter-resolution buffer cannot bleed across a silhouette.
- **Before the atmosphere, not after.** The composite writes into the scene target and the scene copy is refreshed
  afterwards, so aerial perspective, the sun halo and the god-ray source mask all see the clouds. A deck in front of
  the sun dims its rays instead of letting them glow through it.

## Pipeline

| Pass | Resolution | Output |
| --- | --- | --- |
| March | quarter (half on ULTRA) | `rgb` = in-scattered light, `a` = transmittance |
| Composite | full | `lerp(scene, scene * transmittance + scattering, skyMask)` |

The march itself:

1. Reconstruct the world ray from `projUnpack` and the inverse view rows.
2. Reject pixels below the horizon (`smoothstep(0, HorizonFadePermille/1000, ray.z)`) and fade out rays whose slab
   entry approaches `MaxDistance`, so the deck never ends in a straight edge.
3. Intersect the slab, jitter the first step with an interleaved-gradient offset derived from `VPOS` (fixed in
   screen space, never a frame counter - the bilinear upsample then averages neighbouring phases instead of
   flickering).
4. Step through the slab (14/20/28 samples, hard-clamped to 32). At each sample with density, take four fixed steps
   towards the sun or moon on the cheap base shape for self-shadowing, apply Henyey-Greenstein forward scattering plus
   an isotropic floor, a powder term for the bright rim, and a height-dependent ambient from the zone's own fog
   colour. In-scattering is integrated analytically over the step, so changing the step length (a grazing ray crosses
   far more slab than a vertical one) does not change the brightness.
5. Break out as soon as transmittance drops below `0.012`.

## Coverage mapping

The coverage slider is mapped across the **measured** distribution of the noise field, not across `0..1`. For the
shipped atlas the two-octave mix has p2 = .27, p25 = .40, p50 = .46, p75 = .52, p98 = .64, so a naive
`field > 1 - coverage` test hides the entire usable range between slider 0.55 and 0.70. The density threshold is
`lerp(.70, .20, coverage)` with a `.14` soft edge instead.

Measured response of the shipped build (fraction of above-horizon pixels with transmittance < 0.9, offline
simulation of the exact shader math over a 160x90 ray grid):

| CoveragePercent | Sky covered | Mean transmittance | Mean optical depth |
| --- | --- | --- | --- |
| 0 | 0.0 % | 1.00 | 0.00 |
| 15 | 1.5 % | 0.99 | 0.01 |
| 30 | 17.8 % | 0.91 | 0.21 |
| 42 (default) | 46.6 % | 0.66 | 0.98 |
| 60 | 82.4 % | 0.32 | 2.66 |
| 80 | 95.1 % | 0.08 | 4.11 |
| 100 | 95.2 % | 0.06 | 4.43 |

`DensityPercent` scales extinction per world unit around a base of `0.022` (`kBaseExtinction`), which is an optical
depth of about 4 across the default 190-unit deck - thick enough to be opaque at the centre of a cloud, thin enough
that the edges stay translucent.

## Settings

`[VolumetricClouds]` in `GraphicsEffects.ini`, per location like the rest of the tuning (`LocationTuning`), and on the
F7 menu in the first column.

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Master switch for the pass. |
| `Quality` | 1 | 0 = LOW (quarter res, 14 steps), 1 = BALANCED (quarter, 20), 2 = HIGH (half, 28). |
| `CoveragePercent` | 42 | How much of the sky the deck covers. 0 skips the pass entirely. |
| `DensityPercent` | 100 | Optical thickness of the cloud material. |
| `Altitude` | 480 | World Z of the bottom of the slab. |
| `Thickness` | 190 | Height of the slab. |
| `MinimumClearance` | 140 | Minimum distance the slab is kept above the camera. |
| `FeatureSize` | 2600 | World size of one noise tile: larger means bigger, slower cloud shapes. |
| `DetailPercent` | 34 | Strength of the erosion octave that breaks up cloud edges. |
| `WindSpeed` | 11 | Drift speed in world units per second. |
| `WindAngle` | 25 | Drift direction in degrees. |
| `AbsorptionPercent` | 125 | Self-shadowing strength along the light march. |
| `SunIntensityPercent` | 130 | Direct sun/moon contribution. |
| `AmbientPercent` | 62 | Sky ambient contribution, tinted by the zone fog colour. |
| `MoonStrengthPercent` | 30 | Direct contribution at night relative to daylight. |
| `ForwardScatterPercent` | 55 | Henyey-Greenstein `g`: the silver lining when looking towards the sun. |
| `HorizonFadePermille` | 85 | How quickly the deck fades out towards the horizon. |
| `MaxDistance` | 9000 | March limit; the deck fades out before it. |
| `DebugMode` | 0 | 1 = raw low-resolution buffer, 2 = coverage, 3 = direct lighting only. |

Rain and snow add up to `0.26 * WeatherIntensity` to coverage while the weather layer is actually live
(`volume::cloudWeatherCoverageBoost`). The boost is applied to a copy of the settings, so the player's slider is never
rewritten.

## Cost

Two passes: one quarter-resolution march (at 1920x1080 that is 480x270 rays, 20 steps with a 4-tap light march) and
one full-resolution composite. Both are timed as `GpuPerfStage::VolumetricClouds` and appear in the performance
report.

`tools/ValidateShaders.ps1` compiles both shaders with `fxc` in CI and enforces an instruction budget. Measured on
the CI runner: **march 314 slots**, composite 36, against the 512-slot `ps_3_0` ceiling (for scale, the atmosphere's
integrate pass is 462). The light march is a real `[loop]`, not `[unroll]` - four inlined copies of the two-octave
`Shape()` keep more intermediates alive than the 32 temp registers `ps_3_0` has, and fxc rejects it with X4505.

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
