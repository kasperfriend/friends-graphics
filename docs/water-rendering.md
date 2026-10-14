# Water rendering audit

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
shader validation measures **494 slots** and enforces a **496-slot budget**
with room below the minimum hardware limit. Device-creation failures log the HRESULT and device instruction
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
  exceeds 496 static instruction slots or its instruction count cannot be read.
- `tools/WaterRegression.cpp`: portable material, independent-capture toggle
  combinations, and target-size policy tests (included in Linux CI).
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
