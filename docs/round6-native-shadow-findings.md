# Round 6: native shadow architecture findings

Runtime capture: Ascension test client, 2560x1440, 25 September 2026.

## Confirmed architecture

The client uses native cascaded shadow maps. It is not a projected blob-only
system and it does not apply the world shadows in a later fullscreen pass.

Four cascades are rendered every world frame into separate 2048x2048 `D24X8`
depth textures. A 2048x2048 `A8R8G8B8` color target is bound alongside them,
but caster draws use `COLORWRITEENABLE=0`; authoritative shadow information is
therefore the native depth texture. The same four depth resources are bound as
textures for the normal world/material receiver draws later in the frame.

One additional 2048x2048 depth resource is present. It does not participate in
the same four-cascade producer sequence and is treated as a main/view depth
resource until a later diagnostic proves a different role.

## Native caster pass

Common state:

- render target: 2048x2048 `A8R8G8B8` (dummy colour target)
- depth target: 2048x2048 `D24X8` texture, one per cascade
- viewport: 2048x2048
- colour write mask: 0
- alpha blend: off
- depth test/write: on/on
- stencil: off
- primitive type: triangle list

Observed caster shader families:

| Stage | Hashes |
|---|---|
| VS | `202b452e580a7742`, `8535271f53b044b4`, `ea99ea307a197a84` |
| PS | `4f20b99325ed9c47`, `78641e03a615f46a` |

The VS variants cover different caster/material layouts. The two PS variants
track opaque/alpha-tested caster handling. No generic draw replay is needed or
permitted: the game already renders all authoritative caster geometry.

## Native receiver path

The four depth textures are sampled by the normal opaque/alpha-tested world
draws. There is a receiver shader family rather than one receiver pass. The
capture observed the cascade textures on the high texture slots (normally
`s5`-`s8`; some state transitions expose the four-resource set one slot lower).

Repeated receiver PS hashes include:

- `e6931331d14bdbba`
- `2e4e1d13da88efee`
- `827c05c3635b689f`
- `777239a63edea3a1`
- `ac008ac06cd451a9`
- `42c3a115d145164a`
- `f71ca77414ce780e`
- `b52eba6fbc7f34f2`

These pair with several material-specific vertex shaders (WMO, terrain, M2 and
alpha-tested variants). Enhancing a single fullscreen draw would miss native
receivers and is explicitly rejected.

## Cascade evidence

Within the same frame the four depth resources finish at progressively later
draw ordinals, for example approximately 73, 182, 461 and 1955 in one stable
scene. They are subsequently rebound together to receiver shader texture slots.
This producer-to-consumer relationship repeats every frame and remained stable
while the camera was rotated.

## Constants

The diagnostic captured VS/PS `c0`-`c15` for every unique candidate. Caster
projection scale changes per cascade and receiver constants change with the
material. Register semantics are not declared validated yet: the next stage
must disassemble the exact receiver variants and compare values over camera and
sun-time changes before naming projection/light registers.

## Safe next step

1. Capture sampler state for all four native cascade slots.
2. Dump/disassemble the confirmed receiver shaders to locate comparison/filter
   operations and the exact constant registers they consume.
3. Add a developer-only cascade preview/receiver highlight.
4. Improve filtering only through the native shadow representation (sampler or
   receiver shader variant), preserving WoW's matrices, caster geometry and
   receiver placement.

The retired custom `ShadowDraw` replay and long screen-space directional march
remain compile-time unreachable.

## Round 7: the sliders reach a receiver nobody disassembled (2026-10-21)

Reported after the water rounds: **"Native shadows don't work at all."** The
session's `NativeShadowDiagnostics.log` answered it in two lines:

```
[SOFTNESS] ACTIVE: scaled registers c3..10 by 1.45x for psHash=0x449b66c5fb94fede cascadeStage=-1
[SOFTNESS] cascades bound at stage 4 but psHash=0xbeec8ff97f375ed1 is not in the receiver whitelist
```

Read together, those two lines are the whole fault. The enhancement knew about
seventeen pixel-shader **hashes** that somebody had disassembled by hand. The
shader the client actually drew with the four cascades bound was not one of
them, so every slider was inert; and the shader that *was* being scaled had no
cascades bound at its draws at all (`cascadeStage=-1`), so what it scaled was
not a Poisson kernel. A client update that recompiles its material shaders
therefore turns both sliders off, permanently and silently - which is exactly
what "don't work at all" means from the player's seat.

### What replaced the hash list

The shader can state both facts about itself, and the enhancement now asks it.
`src/Effects/ShadowReceiverBytecode.h` walks the pixel shader's own SM2/SM3
token stream (portable, exercised by `tools/ShadowRegression.cpp`):

* **Is this a receiver?** Does it sample the native cascade window? The
  register type in a D3D9 parameter token is five bits and not adjacent -
  bits 30:28 plus bits 12:11 (`D3DSP_REGTYPE_MASK` / `D3DSP_REGTYPE_MASK2`) -
  which is the only thing that separates a constant (type 2) from a sampler
  (type 10) when their low three bits agree. Four sampled stages in s4..s8 (or
  the runtime cascade probe finding the four cascade targets bound at that
  draw) is a receiver. `dcl_2d sN` is read too: it is the only place a pixel
  shader names a stage in the destination slot.
* **Which constants may softness scale?** Only those read with a swizzle that
  never touches .z or .w - the offsets added to a texture coordinate's x/y.
  Each draw still has to hold a value that looks like a tiny 2-component
  offset, so a register used differently in another branch of a material is
  left alone.
* **Which literal may strength patch?** The `def` whose floats are a
  complementary pair summing to one - the client's own
  `visibility * darkening + floor` curve, at either of the two component
  positions the captured families use. That is a shape, not a register guess,
  so a receiver nobody has disassembled can still have its shadows darkened.

The hand-verified families stay as a fast path (a dozen in-memory comparisons
before the driver is touched), a learned receiver is recorded once per session
with the constants it declared (`[SOFTNESS] LEARNED receiver ...`), and the one
whitelist entry the live log proved was *not* a receiver
(`0x449b66c5fb94fede`, `cascadeStage=-1`) is gone. The cascade probe itself no
longer demands one exact depth format: a square depth target of cascade size is
what the question is about, and the client exposes its cascades through a
driver FourCC on some drivers.

Nothing about the client's caster pass, receiver placement or shadow matrices
changes - this is still an enhancement of the client's own cascaded shadow
maps, never a second shadow system.
