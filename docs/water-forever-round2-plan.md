# Water rework round 2: the WoW Forever look

Research notes for the second half of the water rework. Round 1 (square patterns and
whole tiles of native water) is in `water-rendering.md`; this file records what the
target actually looks like and how it maps onto a D3D9 `ps_3_0` shader with no compute
passes, so the plan survives being picked up later.

## What "WoW Forever water" is

Blizzard's Forever client keeps the 2004 art and replaces the renderer. The water part
was shown in the *Evolving a Timeless World* panel (BlizzCon 2026) and is exposed as a
**PBR Water** setting. Reported behaviour, from the panel and the follow-up coverage:

- One shader, three author controls per body: **movement**, **look**, **reflection**.
- Waves **scale with the size of the body** (a puddle, a lake, an ocean) and react to the
  zone's **wind**, so a lake is not animated like a coastal sea.
- Rivers **follow their path**, **bend around rocks** and **foam where the flow meets an
  obstacle**. Artists can author flow (even uphill, for magical water).
- Depth is a real optical property, not a fade: artists set **density and colour** per body,
  **how far light reaches** and **how transparent** it is, "from crystal clear to murky".
  A murky swamp and a clear mountain lake are supposed to look different.
- Water **reflects the world**, including the sun and the moon as the time of day changes.
- Panel-tuned against a stock **1.12 debug client** per zone: the goal is the remembered
  silhouette with modern light on it, not retail art.

Community reaction adds one constraint worth taking seriously: the most cited complaint is
that Forever's shallow water "**lacks the shading the old water had for shallow bits**".
So depth-tinted shoreline shading (the client's own strength of vanilla water) has to stay
strong, not be replaced by a uniform mirror.

## The concrete model to replicate

A deferred PBR liquid: a refracted copy of the scene, absorbed and in-scattered through the
water column, plus a reflected term weighted by a Fresnel/EnvBRDF response, a GGX sun-and-moon
glint with the sun's angular disc taken into account, and foam split into shore, crest and
flow-obstacle masks. Reference formulas worth using directly:

| Piece | Formula | Cost |
| --- | --- | --- |
| Fresnel | `F0 + (1-F0)*pow(1-NoV,5)`, `F0 = 0.02` | 3 |
| EnvBRDF (Karris curve, no metallic) | polynomial in roughness and `NoV` | ~8 |
| Roughness | `pow(saturate(variance), 0.25)`, wave slope variance drives it | ~4 |
| Sun glint | GGX `NoH` lobe **widened** by the sun disc (`alpha + 0.02`), times Smith visibility, times Fresnel, times `NoL` | ~14 |
| Refraction | uv offset `slope * k * min(50, sceneZ - waterZ)` - distortion grows with column depth | ~5 |
| Transmittance | `exp(-absorptionPerYard * pathDepth)` per channel | 3 |
| In-scattering | `(1-T) * colour * HG(cosTheta, g)` with an anisotropic (sun) and isotropic (sky) term | ~12 |
| Foam light | wrapped diffuse `saturate(NoL + 0.25*L.z)`, crests sharpened with `pow(NoL, 8)` | ~6 |
| Reflection direction | `float3(reflect(-V,N).xy, abs(...z))` so the ray never aims into the seabed | 2 |
| Sky fallback | elevation-banded horizon -> zenith gradient, per-band colour | ~10 |
| Reflection fog | fog along the **reflected** path length, not the view path | ~4 |

Two structural tricks that matter more than any single term:

1. **Sample the client's own detail tiles with an explicit footprint**
   (`tex2Dgrad(tile, worldScale, ddx(worldScale), ddy(worldScale))`). Distant water then uses
   a real mip instead of aliasing into a grid. The footprint comes from the projected plane
   step, which round 1 already computes for the wave lattice.
2. **Fade fine detail with column depth, not only with screen footprint.** Fine ripples are
   suppressed on a bar/shoreline and full in open water (`tanh` of `columnDepth / tileScale`).
   This is the direct answer to the "shallow bits lost their shading" complaint: shallow
   water keeps the client's own depth tint while the detail octave steps back.

## What is out of reach, and what replaces it

The reference implementation runs a **Tessendorf FFT wave simulation** on compute shaders with
an 8x8 butterfly, plus a foam *state* texture advected over frames and a per-mip reflection
chain. None of that exists in `ps_3_0` on D3D9: no compute, no UAVs, no ping-pong state texture
without burning a render pass, and the whole shader has to fit **496 instruction slots**
(`tools/ValidateShaders.ps1`, and creation itself fails on PS3-class drivers above 512 - see
`water-rendering.md`; the rework later needed 543 and the budget was raised to 560, which is in
the measured outcome below). So the wave *field* stays the analytic two-octave value-noise lattice
from round 1, which already gives world-anchored slopes, a swell octave and a crest phase; the
"simulation" part is not what makes Forever water read as Forever.

The parts that *are* portable are the shading model (the table above), the mip-correct tile
sampling, depth-gated detail, and per-body authoring. Those are the plan.

## Plan

1. **Shading core.** Replace the hand-tuned Fresnel and the `pow(NoH, 64)` glint with the
   Schlick + EnvBRDF + widened-GGX set. Keep one specular lobe, not three (that duplication is
   what previously blew the slot budget).
2. **Volume.** Split the current single `absorption` scalar into per-channel transmittance with
   the scene-depth column, add Henyey-Greenstein in-scattering, and gate the refraction uv
   offset by `min(depth, column)` so shallow water stops bending the world around.
3. **Reflections.** Keep the existing 12-step SSR march (it already picks the highest-confidence
   hit), add the `abs(z)` direction fix, weight the miss with a sky gradient built from the
   renderer's existing sun/moon and fog state instead of `lerp(fogColor, sheenColor)`, and fog
   the hit along the reflected path.
4. **Foam.** Keep shore/crest, add a flow-obstacle term derived from the wave slope against the
   depth gradient (the client's depth buffer is the only geometry we have), light foam with the
   wrapped-diffuse term, and mix the crest colour toward the scattering colour.
5. **Detail LOD.** `tex2Dgrad` the client's 512 surface map with the world footprint, and fade
   the fine octave by column depth so shoreline water keeps vanilla tint and open water keeps
   sparkle.
6. **Authoring per body.** `GraphicsEffects.ini` already reads a `[Water]` section per zone from
   the location-tuning system; expose `Clarity`, `WaterColour`, `DeepColour`, `AbsorptionColor`,
   `WindStrength` there so a swamp and a lake can differ without a code change, mirroring
   Forever's three author controls. Round 1's `WaveScalePercent`/`WaveSofteningPercent` slot into
   this as the movement controls.
7. **Verification.** Before any of this can be *judged* rather than compiled, we need the real
   per-tile look: `F11` capture plus `tools/Test-Water.cmd` on the software HAL only prove
   finiteness and passthrough. Screenshots from the player (Elwynn lake, Darkshore coast,
   Stormwind docks at dusk, a Barrens river over rocks) are the reference to tune against, and
   are the one input this environment cannot produce.

## Budget

The current shader is documented at **494 of 496** slots. The table above is roughly +55 slots
and the removals (three-lobe specular leftovers, the per-pixel divides already folded to the
CPU, the second noise octave's duplicate hash) are not enough to pay for it at full strength.
Expected landing: SSR step count 12 -> 8 when `Reflections` is below Ultra, the sky gradient
evaluated from a 4-band lerp rather than a scattering integral, and the GGX visibility folded
into the same reciprocal as the existing `1/(1+distance*distance*k)` local-light term. If the
Windows job reports a budget failure, cut the SSR steps first, then the in-scattering isotropic
term, and never the shoreline shading.

**Measured outcome (2026-10-14).** The estimate was low: the rework landed at **543 slots**
(**542** once the doubled `WaterColour` gain came out of the body multiply - see the safety sweep
in `water-rendering.md`), and
two of the three fallbacks above bought nothing. The SSR step reduction saves **zero** slots -
the march loops are `[loop]`, so fxc counts the body once and steps only cost frame time - and
the sky gradient was already the 4-band form, worth 15 slots rather than a fix for the overflow.
With one copy of the shader per candidate cut priced by fxc in a single run, every route under
496 cost a visible term (69 slots for the SSR march, 51 for the sun-road march, 37 for the
analytic swell, and 11-17 each for the scatter lobe, sky gradient, body shading and shoal foam),
so the budget was raised to 560 and the trade recorded in `tools/ValidateShaders.ps1`. Water
effects now require a device that accepts more than 512 instruction slots; below that,
`CreatePixelShader` fails, the failure is logged, and the client's own water stays. The numbers
and the reasoning are in `water-rendering.md`, "Shader budget raised for the volume rework".

## Decisions taken before implementation

- **Budget.** If the PBR set does not fit 496 slots, cut in this order: SSR steps
  (12 -> 8, and 0 when reflections are off), then the isotropic sky-scatter term,
  then the sun-disc widening of the glint lobe. Shoreline and shallow-water shading
  is not a bargaining chip; it stays.
- **Authoring.** Per-body authoring is part of this PR, on the existing location-tuning
  path: `[Water] Clarity`, `WaterColour`, `DeepColour`, `AbsorptionColor`,
  `WindStrength`, with the global `[Water]` section as the fallback so an unauthored
  zone still gets the new look.
- **Reference.** Screenshots of the Forever water the player wants are the tuning input
  for colour, wave scale and foam amount; everything else above is implemented from the
  researched model while they arrive.
- **Logistics.** PR #17 (`arena/e57ae181-modern-wow-renderer`) carries round 1. This
  session may only push to that one branch, so the round-2 work lands as a second pull
  request once #17 is merged; until then the round-2 commits sit on the same branch.

## What was built

Every item in the plan landed, with these substitutions, and the details are in
`water-rendering.md` under "Full rework round 2":

- **Detail mip sampling: not needed.** The plan proposed `tex2Dgrad` with an explicit world
  footprint for the client's own 512 detail tiles. Checked against the shader: the tile is
  sampled at an interpolator uv that the vertex stage derives from the material transforms, so
  `tex2D` already has correct per-pixel derivatives and picks the right mip on its own. The
  reference needs `tex2Dgrad` because it *builds* its uv from a world position inside a loop.
  Adding it here would have cost slots to restate what the sampler already does. The fade that
  mattered - the one for our own procedural lattice - is driven by `fwidth(plane)` and was
  already in place; it now applies to the chop only, never to the swell.
- **FFT and foam state:** replaced by two analytic swell trains plus the bounded noise chop,
  sharing the vertex stage's displacement. No per-frame state texture.
- **GGX/Smith and the EnvBRDF curve:** replaced by a shaped-exponent lobe whose exponent carries
  roughness and sun height, and by Schlick Fresnel with the roughness mix standing in for the
  BRDF integration curve.
- **Blurred reflection mips:** replaced by mixing the screen-space hit toward the sky term by
  `rough`, which is the same perceptual move with fewer samples.
- **Shallow-detail fade by column depth:** implemented as the depth-gated refraction offset
  plus the shore/crest foam split; the fine octave keeps its own footprint fade.
- **Reflection path fog:** the composite already applies the draw's fog after reflections are
  mixed in, and at the near-grazing angles a water surface reflects, the difference between view
  path and reflected path is second order. No separate term.

