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
