# Local fog field (PR #5)

The near fog now samples a 256 x 256 world-space patch covering 256 world
units. RGBA holds four density slices at 0, 0.5, 1 and 1.5 times HeightUnits
above the layer base. The raymarch interpolates those slices using the local
terrain height, so the same field follows slopes instead of a camera plane.

The field is evaluated analytically every frame from world coordinates and
elapsed time. Multiscale noise, time-varying domain deformation, height-dependent
sampling and upper-layer erosion give autonomous flow. It does not accumulate
or advect a history texture. Snapping the patch to whole world texels preserves
overlapping samples when the camera moves. The existing GPU actor state only
adds a bounded clearing and trailing disturbance to this already-moving field.

Terrain coverage now weights the height estimate against the existing tracked
ground fallback, including a soft atlas-border transition. It no longer gates
fog density or lamp-medium density. Actual terrain still clips the medium below
the surface. LocalFog.BaseOffset, FogBaseOffset and GroundMistOffset are applied
relative to that height; positive LocalFog.BaseOffset also raises the bottom of
the layer. Previously these offsets were passed but unused.

No INI key, range, default or hotkey was changed. Old files need no migration.
Activating previously ignored offsets intentionally changes their visual result.
The water boundary/depth shaders, distant atmosphere, temporal reprojection,
upsample, composite, weather and material code remain unchanged. Stage 5 is now
included in ScopedRenderState, and the field's default-pool resources and shader
are released/recreated with the atmosphere. The pass detaches depth through the
existing state guard and restores it on exit.

## Cost and limits

- One additional 256-square draw while local fog is enabled and density is nonzero.
- 512 KiB with RGBA16F; 256 KiB if the existing RGBA8 fallback is used.
- One packed field lookup per raymarch sample; wake and deformation run once per
  field texel instead of per raymarch step.
- FXC /O3 ps_3_0: field 342 slots, integration 431 slots, both below 512.
- Four vertical samples trade fine vertical detail for predictable D3D9 cost.
  This is a procedural pseudo-volume, not a fluid simulation.
- Temporal history is unchanged. The GPU regression verifies that the default
  88% history still permits stationary near-fog animation.

## Validation (2026-09-28)

- Release Win32 MSBuild succeeds without compiler warnings/errors.
- `tools/ValidateShaders.ps1` compiles the active water, local-light and atmosphere
  shaders and enforces the 512-slot budget for the field and integration shaders.
  The pre-existing water shader has 1552 slots and was not changed here.
- `tools/Test-Atmosphere.cmd` passes on the real D3D9 HAL device. Added tests cover
  deterministic autonomous evolution, non-rigid deformation, world anchoring,
  bounded wake, wake-strength zero, positive/negative BaseOffset, mist/height
  offsets, coverage 0/0.1/0.5, sampler 5 and viewport restoration, stationary
  temporal output, master disable and resource recreation.
- The slope integral differs from an independent 8192-step reference by at most
  1 channel level out of 255. Existing shoreline, lamp, torch, water reflection,
  depth-range, terrain capture and atmosphere checks pass.
- Existing local `tools/Build-ProxyTest.cmd` and `tools/Test-FogProxy.ps1` checks
  pass with volume on/off, including pre-UI composition, state restoration,
  Present and actual Device Reset. The proxy script/fixture predate this change
  and are not all tracked in the repository.

Logs are in build/local-fog-build.log, build/local-fog-shaders.log,
build/local-fog-regression.log and build/local-fog-proxy.log (ignored build output).

Actual gameplay screenshots and a GPU frame-time profile have not been captured.
Walking/running appearance, shoreline composition and live overlay interaction
still need in-client visual acceptance; the automated fixtures establish the
technical behavior, not final artistic quality. No client DLL was installed and
no commit was pushed by this change.

## The floor the layer sits on (2026-10-19)

The banks are integrated against a floor that must be a property of the world,
not of where the camera is pointed. Two rules now enforce that
(`src/Effects/GroundReference.h`, mirrored by `kGroundHeightSource` and the
integration shader):

- A depth tap is only a candidate floor if it actually hit a surface and that
  surface is **not above the eye** (`kAboveEyeTolerance`, 0.75 yd). Pitching up
  in a city used to sweep the three fixed probe rows straight onto a wall, a
  balcony or a roof, and the layer rode up with the view.
- The base is the **lowest** usable tap, held when a frame offers none, and
  never above the eye. A crate, a stair or a kerb beside the player cannot lift
  it either. Travel is bounded (3 yd/s up, 8 yd/s down), so a probe flicking
  between two surfaces cannot drag the layer.

Where no terrain was captured - most of Orgrimmar from street level - that
floor is the only anchor the fog has, which is why the old screen-row median
looked camera-attached only in cities. `LocalFog` in the integration shader
also clamps its own base to `cameraPos.z`, so even a terrain-height atlas entry
above the viewer cannot put the bank overhead.

`tools/CameraAnchoringRegression.cpp` pins the rule (roof rejected, crate
ignored, hold on no evidence, never above the eye, bounded travel) and checks
the shipped shader still applies it.
