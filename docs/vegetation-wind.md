# Wind-driven vegetation sway

**Grass** leans and shimmers in the shared wind. The wind itself is not new: the
sway is a consumer of the one world-space `WindState` that already moves the sea,
the cloud deck and the precipitation continuity layer, and it creates no wind,
clock, noise texture, render target or shader of its own.

The client draws its ground grass as rigid, unskinned meshes, so the grass half
of this feature is a **shader injection** - the client's own grass vertex shader,
reassembled with a motion block in it (section 3 below) - and the model half, the
bone-matrix lean that tilts trees, bushes and plants, ships **off**:
`[Vegetation] SwayModels=0`, with its own F7 control. A player reported the model
half as "parts of the buildings sway too": this client draws canopies, bushes and
a good deal of scenery through the same path with foliage textures attached, and
a card that is 20 yards tall bends visibly while a blade does not. The two halves
are otherwise independent; the link, the wind snapshot and the HUD rows are
shared.

- **F7 → WIND → `VEGETATION SWAY`** turns the effect off and on; the link itself
  is **F7 → WIND → `DRIVE PLANTS & TREES`**, next to `DRIVE WAVES` and
  `DRIVE CLOUDS`.
- The model half has a **second lock** for a player who turns `TREES & BUSHES
  SWAY` on: **`GRASS HEIGHT LIMIT (YD)`** (`[Vegetation] GrassLimitYards`, 2 by
  default) refuses any model taller than a tuft of grass, and the HUD counts what
  it refused as `tall`. A real session's log measures the grass-shaped draws at
  0.16 to 0.38 yd leaning the full 20 degrees, while the draws a player reports as
  trees measure 3.40 to 11.03 yd and lean 0.2 to 1.6 degrees; the limit sits
  between them.
- The blade path writes one line to `ModernWoWRenderer.log` every ten seconds of
  play while it is doing anything, `blade wind: …`, carrying the same counters the
  HUD row shows (`drew`, `bent`, `looked at`, `mat/never/decl/card/read/vs/asm`,
  the tip amplitude) - so "the field is green and nothing moves" can be answered
  from a log alone: `drew 0` narrows it to the gate, and a nonzero `drew` with a
  still field narrows it to the bend.
- Settings live in the `[Vegetation]` section of `GraphicsEffects.ini` and obey
  the same per-location overrides as every other section (global, zone, subarea).
- The optional diagnostic **F7 → WIND → `VEGETATION DEBUG VIEW`** adds two rows
  under the wind HUD, readable with F7 closed, plus a third that names the build
  which drew them - the loaded `d3d9.dll`'s own file time, as in
  `ModernWoWRenderer 2026-10-15 18:32` - so a screenshot says which build it is
  about. The first row says how many models are
  leaning out of how many foliage draws arrived, how many of them are still
  **waiting** to repeat themselves in a second frame before anything is written
  (a number that is not a refusal and not the effect working either - it is what
  turns an otherwise unanswerable "0 leaning of 40 drew, 0 solid"), and how many
  solid model draws were passed over; the next row names **the gate that refused
  something**, and
  only the gates that refused something, so "nothing sways" is answered on the
  spot instead of guessed at. The gates are named in the order they run - `decl`
  (a declaration that is not the client's skinned model layout), `world` (the
  WMO/terrain projection: scenery), `vs` (a shader this build could not prove is
  a model), `geo` (the draw's vertices could not be read), `depth` (a pass that
  writes no colour), `cam` (no frame camera, or another view's viewport), `pal`
  (a palette that failed validation, including a c31 that is not a bone block at
  all), `shape`/`size` (not a plant by its skeleton or its proportions), `ground`
  (a model that hangs from its placement point: a cape, a shoulder pad, a sword,
  a saddle), `move` (the client is animating it), `far`
  (beyond the range), `calm` (inside the range, but the wind and its gust field
  leave a zero angle at that spot), `write` (the device refused the leaned
  registers), `new` (shader variants
  proven this frame, with their hashes in `ModernWoWRenderer.log`). A frame that
  refuses at enough gates to fill the box continues the same list, in the same
  order, on the next row (`Plants gate (more): ...`) instead of being clipped.
  These rows are also
  drawn while the wind HUD itself is open, so the vegetation counts are visible
  from the wind tab.

## How it works

The client's skinned M2 vertex shader cannot be replaced - it carries the
client's own lighting, fog and texture stages - so the sway is applied where the
client already keeps a model's animation: **in the bone matrices it uploads for
the draw**.

1. A draw is examined only while it is inside the shared wind's snapshot and the
   frame has an accepted camera pose, and only if it is a **cut-out**: either the
   device has alpha testing or alpha blending on for it, or its own stage 0
   texture has transparent texels in it (see *What it refuses to touch* - the
   second witness is what makes this work on a client whose shader system
   compiles its alpha test into the pixel shader). A solid draw is not foliage
   and cannot reveal that a model is a plant; it is only leaned once its own
   vertex buffer proves it is a batch of a model that has already been accepted,
   which is what keeps a tree's opaque trunk welded to the canopy above it.
2. The draw has to **prove it is a model**: its vertex declaration has to carry
   `POSITION` and `BLENDINDICES` in stream 0, and it has to come from a vertex
   buffer. Every M2 model the client draws is declared with that layout, one bone
   or seventy; terrain, WMOs, particles, ribbons, effects, shadow blobs and the
   interface declare no bone indices at all, so this is the structural proof the
   whole feature rests on. A fixed-function draw, an up-front (particle, ribbon
   or interface) batch and a declaration this reader cannot address are never
   considered - and a declaration it cannot read is written to
   `ModernWoWRenderer.log` in D3D's own terms, once, so a model that refuses to
   sway can be identified from a player's log alone.
3. Its vertex shader has to be a model shader, and that is judged by what the
   draw **is**, not by a hash list: this project's captured lists
   (`src/Lighting/TorchSignatures.h`, the same lists the torch effect uses) are
   accepted on sight, and so is any other variant over a declaration that already
   proved the client's skinned model path - a list captured from one build of the
   client cannot gate another build, which is exactly how a working feature reads
   as "0 swaying" on a machine whose client ships different shader binaries.
   Only two families are refused: the world's own projection (WMO and terrain,
   `c4..c7`), and the passes this project has already identified as
   never-a-model (the native shadow casters, UI, precipitation and water). A
   variant this build had not seen is reported once in `ModernWoWRenderer.log`
   **by hash**, accepted or refused, and counted as `new` on the HUD row so the
   captured list can be extended from a real session.

   Which shader is judged, and which declaration it is judged through, are read
   **from the device** - `GetVertexShader` plus the shader's own bytecode,
   `GetVertexDeclaration` - and never from the renderer's state mirror. The
   client binds its own passes in cached state blocks, which never reach the
   `SetVertexShader`/`SetVertexDeclaration` hooks, so a mirror can describe an
   earlier draw, or no shader at all, while a model is being drawn; every draw
   in the frame then fails the shader gate at once, which is the one refusal a
   player cannot argue with and is exactly what the first two in-client reports
   of "2 leaning, nothing moves" looked like. A shader whose bytecode the
   runtime will not hand back has no identity at all: such a draw is judged by
   the constants it binds, on that draw, and is never cached - a verdict filed
   under hash 0 would let the first unidentifiable draw in the session decide
   for every later one.
4. The material has to be a cut-out, and both witnesses for that are read
   **from the device**, not from the renderer's state mirror: the client's own
   passes can arrive through cached state blocks, which never reach the
   `SetRenderState` hook, and a foliage draw the mirror believes is solid is
   never revealed as an instance at all. A pass that writes no colour is refused
   for the same reason, and that test runs *before* the shader is judged - the
   client's native shadow map is the biggest source of model-path draws in a
   frame, it is a depth-only pass whose palettes live in the light's space, and
   naming it `depth` is truer than offering it as a model this build could not
   prove.
5. The bone palette the client uploaded at **c31** is bounded by the draw's
   **own vertices**: the model's bones are the bone indices *its* vertices
   reference, and the palette is read, validated and written no further than
   that. This matters more than it sounds - the registers above a model's own
   bones still hold whatever the previous draw left there, so a reader that walks
   c31 upwards while the blocks look like matrices counts another model's
   skeleton (a six-bone bush read as a seventy-five-bone one). Every block inside
   the bound must then be finite, orthogonal up to scale and hold a plausible
   translation; the first one that is not ends the palette.
6. The validated blocks are reconstructed into the model's world **placement
   point** (block 0, the root bone every batch of a model uploads first), its
   root orientation, its size, and the **spread** of its bones - how far apart
   the bone positions are, which is what distinguishes one body from a batch that
   carries a whole patch of plants (see *What it refuses to touch*). The size is measured from the draw's **own
   vertices** through their bone blocks, with the bone span kept as a lower
   bound: bone pivots sit inside a model, so a single-bone grass tuft spans no
   height at all and would be refused forever by a size gate that trusted them.
   The placement point, the root orientation and the model's vertex buffer
   together identify one *instance*, which is what makes the sway follow a placed
   model rather than a draw call: the opaque trunk of a tree leans with the
   alpha-tested canopy that revealed it, even when the client draws the trunk
   first, and the batches of one model all rotate about the same point.
7. The instance's own phase - taken from its world identity, never from a frame
   counter - sets its flutter so two tufts a yard apart do not move in lockstep.
8. Every validated bone receives **one** rigid rotation about **its own
   position**, taken from the shared wind. For a single-plant model that is
   exactly the rotation about the model's placement point it has always been - a
   tuft's one bone sits where it meets the ground - and for everything else it is
   what makes the transform mean the right thing: a tree's trunk turns about its
   base while its branches turn about the points they grow from, and a batch the
   client merged a whole field's grass into bends tuft by tuft instead of
   swinging the patch like a plank. No distance from a bone's own position
   changes, so a stem bends instead of stretching, and two batches of one model
   receive the same rotation, so they cannot be pulled apart.
9. The leaned blocks are written into the same c31 registers for the draw, and
   the client's own matrices are **restored immediately after it**, in the same
   call, so the next draw and any state the game applies afterwards see exactly
   what it uploaded.
10. The values that are put back are the ones **this draw** read, a few
    microseconds earlier, from the device - never a copy remembered from an
    earlier batch of the same instance. A client re-uploads a palette whenever it
    likes, in the middle of a frame as much as at the start of one, so a
    remembered palette is not what the device holds at the draw being leaned, and
    restoring it hands a model a skeleton the client never gave it. That is what
    a bent shoulder, a sword and a mount's backside look like from the outside,
    and it is why one `GetVertexShaderConstantF` per leaning draw is paid
    without hesitation. The write is also armed for roll-back *before* it is
    attempted, so a write the device refuses is put back over just the same - and
    the first few leans of a session have their roll-back **read back and
    compared**, bit for bit, with the mismatch reported in
    `ModernWoWRenderer.log` and counted as `restore` on the HUD row. On a healthy
    client that number is zero; when it is not, the renderer's own write - not
    the gates - is what deformed a model, and the report says so.

Nothing else is touched: no render state, no shader, no constant above c31, no
render target.

## The grass that has no bones

A field of ground grass in this client is often not a model at all: the blades
arrive as **one rigid mesh**, drawn with a vertex declaration that carries no
bone indices (`POSITION + NORMAL + COLOUR + TEXCOORD`), textured with a leaf
card. There is no palette to bend and no animation to follow, which is why such
a field stays perfectly still however many plants the sway leans - and why the
`Blades:` row counts it: those draws are its `by height` number, the cards the
client hands over with no texture coordinate to bend by.

Those draws are bent in **the client's own vertex shader** instead:

1. The draw reaches the same early gates as everything else: master wind, the
   link, the effect switch, a frame camera pose, colour writes, no up-front
   (particle/ribbon/interface) batch. It is a draw the sway would otherwise
   count as `decl`.
2. Its vertex declaration has to carry a **`FLOAT2` texture coordinate** - that
   coordinate is the bend weight, and it is also what the injected code reads -
   **or no texture coordinate at all**. A whole family of this client's foliage
   arrives that way: `POSITION + NORMAL` and a cut-out atlas, 1.2 million such
   draws in one real session against the 49 that reached the material gate, which
   is why a report could say a shader was bent while the field stood still. Those
   cards are bent by the **vertex's own height** instead - the block reads that
   register's `z`, and the ramp is the card's own measured floor and height, the
   same zero-at-the-base, one-at-the-tip weight the coordinate gave. Nothing else
   about the admission changes: the material still has to measure as a cut-out,
   the draw's own vertices still have to measure as a blade of a blade's size, and
   the shader still has to be one the client's library says is not the interface,
   the precipitation or the liquid. And its stage 0 texture has to be a
   **cut-out**. Whether that cut-out is
   `GREEN` is deliberately not asked here, unlike the material gate the model
   path uses: a dry, autumn or snow-covered field is as much of a blade card as a
   green one, and asking about colour on this path refused the very field the
   feature exists for. Nothing that could be mistaken for gear can reach this
   path at all - a character, a weapon, a mount, a cape and every other attached
   model carries bone indices in its declaration and is judged by the model path
   instead - so a cut-out is the whole material proof a blade needs.
3. The shader it is drawn with is looked at **once per session**. Its bytecode is
   taken from the device, disassembled (`D3DDisassemble`), and a motion block is
   inserted after the line that last gives `r0` the vertex's own position before a
   clip-space output reads it. That is found by the data rather than by the
   instruction, because this client draws its clutter in two shapes and both have
   to be bent: the reference client's own shader lands the vertex with
   `add r0, r0, c3` (`r0` is then the *view* position), while the shapes this
   client's fields are drawn with place it through the instance's own rows
   (`dp4 r0.z, c33, v0`, so `r0` is already the *world* position and the clip-space
   output is spelled `o0`, not `oPos`). The block is built for the frame it will
   run in: the world-space variant reads `r0` directly, lands its displacement on
   the world's own axes, and the device half packs away the camera correction and
   the view-distance fade that only a view position has. The text is then
   reassembled (`D3DAssemble`) and created on the device, and from that draw on the
   client is handed the bent shader instead of its own whenever it binds it - and,
   for a bind that goes through a cached state block and never reaches this
   project's hook, again at the draw itself, where the draw is already known to be
   a blade card. Both compiler entry points come from `d3dcompiler_47.dll`; if
   that library is not there, the path switches itself off and says so.

   Two assembler rules the block obeys, each learned from a refusal in a real
   session: no instruction may read **two different constant registers**
   (`D3DAssemble` answers `X5752`, and one such line - the distance fade reading
   `c17.w` and `c14.z` in one `mad` - is a whole still field), and no instruction
   carries an immediate float.
4. The injected block moves each blade in the **world plane**: it reconstructs
   the blade's world position from the view-space one the client just computed and
   the view transform's OWN translation row - the `cN` of the `add r0, r0, cN`
   anchor line, subtracted off and rotated back with the shader's own rows -
   displaces it by a travelling wave plus a per-blade phase taken from that
   position (so blades do not move in lockstep and the motion does not shimmer as
   the camera moves), and rotates the result back into view space. A shader whose
   anchor names no such row falls back to the frame's camera correction, which is
   correct only when the captured camera is the matrix the shader was drawn with -
   a real session's grass shook while the player walked because it was not, and
   the world position a wave is built from now has no term from the frame in it at
   all. The bend weight is the blade's own texture coordinate - or, for a card
   that carries none, its own height above the floor its vertices measured: zero
   at the base either way, growing towards the tip, with the bottom third held
   rigid (`anchor`, 0.3) and faded with distance. At reference strength a blade's tip
   sways about a foot over a wave five yards long travelling downwind at two and
   a half yards per second - a crest every two seconds, which is what makes the
   field read as moving from a walking pace. The first shipped numbers were five
   inches over an 18 yard wave at 3 yards per second, and a real session's own
   `blade wind: ... yd at the tip` line is what said they were wrong: the shader
   was bent on every draw and the field still looked still, because a whole field
   drifting a hand's width every six seconds is smaller and slower than the eye
   reads as swaying. The constants ride in the block at
   **c14..c19**, or - for a shader that reads one of those registers as its own
   lighting, which a real session's 956 and 1048 byte shaders do - at the first
   run of six registers above them that the shader never mentions (`c34` for
   those two). The device writes each shader's rows at that shader's own base and
   re-arms them when the client writes over that range.
5. The draw's own vertices decide whether it is a card worth bending - and they
   are the *whole* admission: nothing is bent that did not measure as a blade. Its
   texture coordinate, its height and the base/tip pairs in it are measured from
   the vertices the draw consumes (a strict fit: an element that ends exactly on
   the stride's last byte is inside the vertex - the client's tight
   `POSITION@0 COLOUR@12 TEXCOORD@16` cards are 24 bytes and were refused by a
   `<=` that treated them as out of bounds), and a card that is measurably flat -
   a decal or a shadow blob in a cut-out material - is refused on that geometry
   and never looked at again. A card taller than a grass tuft (3 yards) is not a
   blade either: the log of a real session has this path bending a 22.91 yd card
   with ten base/tip pairs in it - a canopy, or a wall carrying a foliage texture -
   which at a blade's amplitude is invisible motion with a moving silhouette. A
   draw whose vertices cannot be read at all (a stream that will not lock, a batch
   that is not a triangle list) proves *nothing* either way and is not bent
   either; refusing it costs the shader its chance only when every draw of it is
   unreadable, and a real field measures in the first draw or two. Because one
   shader is bent for all of its draws, a verdict waits for a few draws
   (`kBladeCardTries`) before it is taken as final: the first draw of a chunk is
   not always a blade.
6. Every refusal is structural and one-shot logged: an empty disassembly, a
   shader that is not `vs_2_0`/`vs_3_0`, a declaration that gives the block
   neither a texture coordinate nor a position to read a height from, a shader that never lands its vertex in `r0` before a
   clip-space output reads it, a scratch register the shader still needs **across**
   the insertion point (a register the shader redefines *after* the block is not a
   conflict - three of the five foliage shaders of a real session keep their
   normals and their lighting in `r2`/`r3`), a declaration whose texture coordinate
   is not the register the shader reads - the coordinate on one ramp, the position
   on the other - a card that was read and lies flat, or an
   assembler that refuses the text. In that last case the shader's **whole
   disassembly** goes to `ModernWoWRenderer.log` once, so the next build can be
   written against the exact code the client ships.
7. What the shader *is* is judged the same way the model path judges it, with one
   deliberate difference. The passes this project has already identified as
   never-a-model - the native shadow casters, UI, precipitation and water - are
   refused here too, because an interface icon is a cut-out card with a texture
   coordinate, exactly the shape a blade has. The **world's own projection**
   (`c4..c7`) is *not* refused on this path: that verdict says a shader binds the
   WMO/terrain layout, which is a real suspicion on the model path where a model
   is supposed to carry its bones at `c31` - and an unskinned card has no bones
   at all, so an object -> world matrix is simply how the client draws its grass.
   Refusing it there turned away **376** foliage draws in one real session (the
   HUD read `world 376`) and left the field perfectly still. Such a shader is
   recorded instead - the HUD's `Blades world-path` row counts it - and bent on
   the geometry and material proofs above.

The switch is `[Vegetation] GrassBlades` (F7 → `GRASS BLADE WIND`, on by
default). Off, every plant with a skeleton still sways and no shader is ever
rewritten.

Its rows in the debug view (F7 → WIND, with `GRASS BLADE WIND` on) are:

- `Blades: N swaying, M blocks/s, K by height | X.XX yd at the tip` - how many
  draws in this frame were handed the bent shader, how often the constant block
  had to be rewritten, how many of those draws are from the uv-less family (see
  below: the cards bent by their own vertex height), and how far a blade's tip
  moves at the current wind strength.
- `Blades gate: N looked at, M bent | ...` - the blade path's own census, in the
  same style as the model row above: `mat` (a material that is not a cut-out:
  opaque scenery), `never` (the interface, precipitation, the liquid variants and
  the shadow casters), `decl` (a declaration with neither a texture coordinate
  nor a position, so there is no height to read a weight from), `card` (a card
  that was measured and lies flat), `read` (a card whose vertices could not be
  read at all), `vs` (a coordinate that is not the register the shader reads) and
  `asm` (a shader that could not be disassembled, reassembled or created).
- `Blades world-path: N bent` - shaders that were bent and bind the world's own
  projection. It is a note, not a refusal: see point 7 above.

## What it refuses to touch

This is the part that keeps the effect safe, and every rule is enforced before a
single byte is written:

- **Master wind off, `DRIVE PLANTS & TREES` off, `VEGETATION SWAY` off, or F11
  graphics off.** No palette is read at all. Disabling a link restores exactly
  the behaviour that existed before this feature for that consumer, and never
  resets the shared wind for the others.
- **A body's skeleton.** The bones a model's own *vertices* reference are the
  skeleton that model has, and a plant's is a tuft, a bush or a tree's branches -
  never a body's. The palette reader can only count the blocks it can hold
  (`kM2MaxBones`), so this count is taken from the vertices, where it is honest:
  a character, a mount or a creature references more than a plant's worth, and is
  refused for good the moment its model is measured. A merged patch and a tree
  are the two exceptions, and only because their skeletons are *spatial* (see
  below).
- **Geometry that hangs instead of standing.** A plant grows out of the point it
  is placed at: a WoW doodad is placed with its base on the ground, so its
  vertices are above its own origin. Everything the client hangs from a joint
  reaches *below* it - a cape, a shoulder pad, a sword, a saddle, a banner - and
  that is a witness about the model's shape, so it refuses them even though a
  standing character's shoulder is not moving
  (`kVegetationBelowPivot`).
- **Models the client is already animating.** Four readings have to agree, and
  any one of them refusing is enough to keep a model out of the wind for good:
  1. *The skeleton's world pose* has to be identical between frames;
  2. *the world positions of the model's own vertices* have to be where they
     were (which is what a particle or effect batch, re-uploaded every frame with
     new data, fails even when every matrix it binds is still);
  3. *the placement point* has to be where it was - a placed doodad does not
     travel, while a mount under a player, a walking creature and an object held
     in someone's hands move constantly;
  4. *the pose has to repeat for a run of consecutive frames*
     (`kVegetationStableFrames`) **and to hold for a stretch of time**
     (`kVegetationStanceFrames`): the model has to still be in the pose its run
     started with. A creature breathing, a tail swishing or a walk cycle only
     looks still for a frame or two, and - because the comparison is against the
     stance and not only against the frame before - a slow idle animation is
     caught as motion rather than passing as stillness.
  A model that fails any of them is refused, stops leaning immediately, and is
  refused for a while afterwards (`kVegetationMoveCooldownFrames`), so a walk
  cycle cannot flicker back into the wind between two frames that happen to
  match. This is what keeps mounts, characters and creatures out with
  `SWAY ANIMATED MODELS` off, and the placement-point and pose tests are
  re-run for **every** batch of a model - including the solid batches a
  creature's saddle, armour or held object arrives as.
- **Full skeletons - unless they are a patch.** More than 24 bones in a
  validated palette is a character, creature, mount or weapon rather than one
  plant. But a client may merge a whole field's grass into a single draw with one
  bone per tuft, and such a draw carries more bones than any creature: what tells
  them apart is that a creature's bones all sit inside one body (a couple of
  yards across), while a patch's are spread over the patch (or a tree's span its
  own height). A skeleton that is *spread out* (`kVegetationMergedSpread`) or
  *tall* (`kVegetationTallSkeleton`) is not one body and is admitted, and every
  one of its bones turns about its own position (see below). The two are not the
  same thing to the player, though: a **merged patch** is low and wide (bones
  spread over the field, none of them above the grass) and is counted on the HUD
  as the number of plants it carries, while a **tree** is tall - one plant,
  however many branches its skeleton has, and counted as one.
- **Anything too small or too large** (below 0.15 yd or above 90 yd tall), and
  anything clearly longer than it is tall (more than 2.5x, with 3 yd of slack) -
  a fence and a wall are not plants. A *merged patch* gets the generous limit
  instead, because it is not one plant but a whole one-distance-across field of
  them; even that limit is a limit.
- **Anything beyond `SWAY DISTANCE`.** The sway reaches full strength at 75% of
  that range and fades to nothing at the range itself, so nothing pops.
- **Solid materials - with two witnesses, because one of them is not always
  there.** A draw with an opaque material never reveals an instance, and neither
  does any draw outside the shared wind's snapshot, so the whole discovery path
  is limited to the client's own cut-out model materials. The first witness is
  the render state, read from the device (so a pass that arrives through a
  cached state block is judged by what the device is actually doing): alpha
  testing or alpha blending on means a cut-out. The second is the draw's own
  stage 0 texture, read once per texture and remembered: a plant is a card cut
  out of a transparent image, so a texture with transparent texels in it is a
  cut-out whatever the render state says. The second witness is what the first
  one cannot replace on a client whose shader system compiles its alpha test
  into the pixel shader - a `discard` on the sampled alpha - where both states
  are off while grass is being drawn and a state-only test reads a whole field
  of grass as solid props. A draw is refused here only when the state says solid
  **and** its texture was read **and** that texture has no transparency in it: a
  crate, a boulder, a piece of armour. The proof is recorded on the *instance*,
  so a model's other batches (the trunk under the canopy) cost nothing, and it is
  dropped the moment the model stops leaning - a spot reused by a solid prop is
  not leaned on the strength of what stood there before. The first refusal of a
  session names its shader in `ModernWoWRenderer.log`, which is what makes a
  `solid` count answerable - and a refusal that could not read the texture at all
  (nothing bound at stage 0, a resource that is not a texture, a level 0 that
  will not lock, a format this reader cannot decode) says so instead, because
  that is a different bug from a texture that was read and had no holes.
- **A palette that is not where this reader looks for it.** `pal` counts a c31
  that is not a bone block, a palette that fails validation, a root block whose
  translation is not a placement point, and a pivot that does not survive the
  view transform; a device that refuses the leaned write has its own name
  (`write`). The first such refusal of a session writes the twelve numbers it
  read at c31 to `ModernWoWRenderer.log`: zeros mean the client keeps its bone
  matrices in a different register, a projection means the draw was not a model
  at all, and a different stride shows up in the numbers. That is the difference
  between "the plants are not being measured" and "this client keeps its
  skeleton somewhere else", and no counter can tell them apart.
- **Depth-only passes.** A draw that writes no colour - the client's native
  shadow map above all, but also its occlusion fills and depth fills - is
  refused: nothing visible is being drawn, and the shadow cascades skin in the
  light's space, so the frame camera's wind on them would be a visibly wrong
  shadow. The colour-write test runs *before* the shader is judged, precisely
  because the shadow map is the biggest source of model-path draws in a frame:
  counting it `depth` is true and readable, where counting it as an unproven
  shader put a number on the gate row that no player could act on.
- **Anything that is not the client's skinned model path.** Terrain, WMOs,
  particles, ribbons, effects, the interface and every fixed-function or
  up-front batch declare no bone indices (or no vertex buffer at all), so they
  are left alone before a palette is even read. Terrain-bound grass is terrain:
  it is not a model, it is part of the ground mesh, and no amount of wind in this
  module will move it. A fixed-function pass is counted
  `flat` and a draw with no readable declaration `decl`.
- **The passes this project has already identified as never-a-model** - the
  native shadow casters, UI, precipitation and water - are refused by hash
  before any constant is read, and counted `never`. This is *not* the same
  counter as `vs`, which is a model draw this build could not prove at all:
  the row keeps them apart because a player can act on one of them and not on
  the other.
- **The world's own projection is a suspicion, not a verdict.** A draw whose
  constants bind `c4..c7` the way terrain and WMOs do may be scenery - but the
  constants are a guess about which shader variant a client happened to compile,
  and a client that draws its grass through that path would stop swaying for
  good. So such a draw is carried to the palette gate and refused there only if
  `c31` does *not* hold an object -> view bone block: a world-shaped binding with
  a real bone palette is a model, and a model leans. It is counted `world`, which
  therefore means "scenery, confirmed by the absence of a palette".
- **Draws that are not the frame camera's.** A model is only leaned when the
  device is drawing into the viewport the accepted camera pose was captured in,
  which is what keeps the client's portrait, target, minimap and reflection views
  (cameras of their own, in viewports of their own) out of the wind.
- **Up-front batches and fixed-function draws.** A particle, ribbon or interface
  batch has no vertex buffer and therefore no model identity, no palette bound
  and no placement point that outlives the call.
- **Overlays and the F7 menu** - those draws are scoped out before any work
  happens.
- A palette, a vertex layout or a camera pose that does not validate, and any
  non-finite number anywhere in the chain: the draw is left exactly as the client
  intended it.

`SWAY ANIMATED MODELS` (default **off**) is the escape hatch: it also leans the
instances the client animates, which is what a zone with animated foliage needs
and also what lets a creature that stands still lean in the wind. It is a
deliberate opt-in, not the default.

## Wind semantics

- The sway reads **one** published `WindState` per frame: unit horizontal
  downwind `direction`, `strength` (0..3, 1 = the reference breeze, 12 yd/s of
  travel), weather and a bounded frame delta. Same state, same frame, as the
  waves and the clouds.
- The gust pattern is a world-anchored, wrapping noise field (26 yd cells, 256
  cells per axis) that is **advected by that same velocity**, so gust lines
  travel downwind at the same time as the clouds drift. A small constant drift
  keeps the air of a calm zone alive without inventing a second wind.
- A gust tilts the wind a little; it never replaces the wind direction. The mean
  travel stays the vector the other consumers use.
- The lean is the tip travel a plant of that size would have under that wind:
  `angle = atan(tip / height)`, so grass leans visibly and a tall tree leans by a
  couple of degrees, capped hard at `MAX LEAN (DEGREES)`.
- Flutter is a small fast shimmer on top, strongest on grass and bushes,
  weakest on trunks, with a per-instance phase.
- All of it is driven by the wind snapshot's own **bounded delta** (0.1 s
  maximum): F7 edits, F12, location changes, a device reset and an Alt+Tab gap
  cannot teleport or catch up anything. Device reset drops the cached draw
  identities and pose history but never re-anchors a phase.

## Preview

`tools/VegetationPreview.cpp` is an offline previewer: no SDK, no GPU and no
client. It drives the same `WindSystem` the renderer runs, the same
`VegetationWind` policy, and builds each plant's bone matrix the way the client
uploads it, leans it with `BuildVegetationLean`/`ApplyVegetationLean` and renders
the result on the CPU. A frame from it is what the client is asked to draw for
that wind, plant size and camera.

```
g++ -O2 -std=c++20 tools/VegetationPreview.cpp -o build/vegetationpreview
build/vegetationpreview --out Screenshots/vegetation-wind.png --grid 2 --stats
```

`--grid 2` draws the four-panel sheet above: calm, breeze, strong wind and a
storm (the same 200% setting with rain raising the target strength), one frame
each, under a bar that prints the shared strength, the downwind speed in yd/s and
the mean, tree and tuft lean of that panel. `--stats` prints the same numbers and
a per-plant line with `--debug`. Single frames use `--width/--height`, and
`--strength`, `--direction`, `--gust`, `--flutter`, `--max-angle`, `--distance`,
`--plant-gain` and `--tree-gain` take the settings the F7 tab takes, so a value
can be tried here before the client. `--yaw` and `--pitch` override the fitted
camera; `--no-labels` drops the bars.

It is a diagnostic aid, not a beauty pass: the scene is a schematic row of
tufts, bushes and trees whose only job is to make a lean measurable - the trunk
and canopy of a tree stay welded because the whole model gets one rotation, and
the tip travel of a stem can be read off the sheet. The sheet above was rendered
at the shipped defaults with a steady west wind.

## Settings

| INI key (`[Vegetation]`) | F7 | Default | Range | Meaning |
| --- | --- | --- | --- | --- |
| `Enabled` | VEGETATION SWAY | 1 | on/off | Turns the whole effect off. No palette is read while off. |
| `SwayPercent` | SWAY STRENGTH | 100 | 0..300 | Overall lean. 0 leaves every model untouched. |
| `GustPercent` | SWAY GUSTS | 45 | 0..100 | How deeply the travelling gust field modulates the lean and its heading. |
| `FlutterPercent` | GRASS FLUTTER | 100 | 0..200 | The small fast shimmer, mostly grass and bushes. |
| `MaxAngleDegrees` | MAX LEAN (DEGREES) | 20 | 2..30 | Hard ceiling, so nothing can fold over. |
| `PlantPercent` | GRASS & BUSH GAIN | 100 | 0..200 | Gain for models below ~1.5 yd. 0 stops undergrowth and leaves trees alone. |
| `TreePercent` | TREE GAIN | 100 | 0..200 | Gain for models above ~6 yd, blended in between. 0 stops trees only. |
| `DistanceYards` | SWAY DISTANCE (YARDS) | 120 | 20..300 | Sway fades to nothing by this range. |
| `GrassBlades` | GRASS BLADE WIND | 1 | on/off | Bends the client's own vertex shader so the grass drawn as rigid, bone-less blade meshes moves too. Off, only models with skeletons sway. |
| `SwayAnimatedModels` | SWAY ANIMATED MODELS (MAY MOVE CREATURES) | 0 | on/off | Also leans models the client animates. Off by default; see above. |
| `DebugView` | VEGETATION DEBUG VIEW | 0 | on/off | Two extra diagnostic HUD rows: what leans, and which gate refused the rest, by name and count. |

`[Wind] AffectVegetation` (F7 → WIND → `DRIVE PLANTS & TREES`, default on) is the
link, exactly like `AffectWaves` and `AffectClouds`.

## Validation

The previewer above is what makes a tuning change visible, but it is not the
guard: `tools/VegetationWindRegression.cpp` is the portable suite, and it is what
decides whether the behaviour is still correct. It builds and runs with no SDK on
Linux and is wired into CI, and it pins:

- the lean transform: pivot-exact, purely rotational (unit, orthogonal basis),
  keeps every distance from the pivot, and tips a stem **toward the world wind
  direction** for any camera yaw/pitch and any of the eight headings;
- the palette guard: a sheared block, a NaN, an absurd translation and an
  all-zero palette each end the palette instead of being used, while mirrored and
  scaled bones are accepted;
- static-vs-animated detection: a static model reads as static from a camera
  hundreds of yards away and through a different pose reconstruction, a bone that
  moved 25 cm does not, and a model that turned in place does not;
- the gust field: bounded, continuous across the world including its wrap period,
  continuous in time, travelling downwind, and costless in calm air;
- the grass that has no bones: the shader model tokens, the injection point (after
  the world-to-view anchor, before the position output), the texture-coordinate
  register read from the declaration, every structural refusal (no texture
  coordinate, occupied scratch registers, no anchor, no output, unknown model,
  empty text), the register discipline of the injected block, and the packing of
  the constants it reads, including the calm frame;
- strength, size classes, gains, range fade, flutter, and all three switches
  (master wind, the link, the effect);
- admission: every captured model variant is accepted on sight, the
  never-a-model list (shadow casters, UI, precipitation, water) is refused and
  does not overlap the captured lists, the client's model projection is
  recognized while a terrain/WMO-style or transposed one is not, the world's own
  `c4..c7` projection is refused, and a declaration without bone indices is not
  a model draw whatever its shader is;
- the proofs the gates read from the device rather than from the state mirror:
  the declaration a draw is judged through (`GetVertexDeclaration`), the
  colour-write mask, the shader it is judged by (`GetVertexShader`, then the
  shader's own bytecode), the stage 0 texture, and the material (alpha test or
  alpha blend) - with the mirror left to the free early counters, the palette
  read kept above every device call it can be, and the gate order pinned as
  declaration -> colour writes -> shader -> palette -> material -> camera: the
  declaration proves the draw is a model, the depth pass is named before a hash
  or a texture is touched, the palette proves it is a model at all, and the
  material read is paid for the draws that can still become a plant;
- the cut-out decoder, over every format a client uploads: DXT1 in both of its
  modes (the 1-bit alpha one and the opaque one), DXT2/DXT3 explicit alpha,
  DXT4/DXT5 interpolated alpha in both of its modes, and the unpacked
  A8R8G8B8 / X8R8G8B8 / A4R4G4B4 / A1R5G5B5 / A8 / L8 - each one checked on both
  sides of the halfway mark, with padding outside the image never counted as a
  texel (an odd-sized opaque texture is not foliage), a partial lock read exactly
  as far as it goes, and an undecodable format, a texture that is too small to
  take a share over or no texture at all classified as *unknown* rather than
  guessed - because a false cut-out now puts a solid prop in the wind;
- the identity fallback: a shader the runtime will not hash is judged by the
  constants it binds and never cached under hash 0, and a shader-gate refusal of
  a model draw is counted whatever the mirror believes about its material;
- the one-shot data dumps a refusal cannot carry as a number: the shader hash of
  the first solid model draw of a session, and the twelve values read at c31 when
  it did not hold a bone block;
- the material proof of a model outliving the batch that paid for it, and the
  moment it is dropped: a model that stops leaning loses it, so a solid prop
  placed on a spot a plant used to occupy is never leaned for it;
- the model's own vertices: the palette bound comes from the bone indices the
  draw's vertices reference (never from the constant file), the size gate
  measures the vertices themselves - a one-bone tuft with a 1.6 yd mesh measures
  1.6 yd where its bone pivots measure nothing - a vertex bound to an unrevealed
  bone is clamped instead of reading past the bound, and the lean pivots on the
  model's placement point;
- instance identity: the placement point, the root orientation and the vertex
  buffer decide it, a moving camera does not change it, and **batches of one model
  that reveal different numbers of bones stay one instance** - the failure that
  made every plant a new, never-stable instance and left the HUD reading zero;
- the HUD itself: every gate the device half counts has a name on the gate row,
  the row lists only the gates that refused something and in the order they run
  in, a frame that refuses at every gate **continues the census on the next row
  rather than clipping it**, every row fits the box it is drawn in, the waiting
  count is printed next to what leans and never offered as a refusal, the head of
  the gate row carries the number of draws that arrived as the client's skinned
  model path at all (the one number that tells "nothing was refused" apart from
  "this client does not draw this field as models"), a fixed-function pass and a
  pass this project has already refused are named as `flat` and `never` rather
  than offered as unproven plants, and the build stamp row is drawn
  from the DLL's file time - published by the Windows half, absent when nothing
  published one - so a screenshot names the build it came from;
- every counter the HUD can report is incremented by the device half, so a gate
  that refuses silently is a test failure rather than a bug report;
- the guard sits above the first palette read in the draw scope, and the four
  draw entry points each describe their own batch exactly once;
- INI/F7/default parity for every `[Vegetation]` key and for
  `[Wind] AffectVegetation`, no duplicated key in any section, and the production
  wiring itself (four draw hooks, one update next to the wind snapshot, the reset
  and reload paths, and that the lean is written over c31 and then restored).

In-client acceptance checks (Windows and the Ascension client):

0. The last row names the build (`ModernWoWRenderer 2026-10-15 18:32`). Check
   it against the `d3d9.dll` in the client folder - Explorer, right click,
   Properties, Modified - **before** reading anything else: a client that never
   restarted, or a DLL that was not replaced, shows the previous build's
   counters, and they read exactly like the fixed build's.
1. Stand in a grass field with trees beside you, day and night, with the F7
   WIND tab open on
   `VEGETATION DEBUG VIEW`. Grass, bushes and tree canopies lean downwind and
   shimmer; `leaning` rises with the number of visible plants, and if it does not,
   the second row names the gate that refused them, in the order they run:
   `flat` for fixed-function T&L (no vertex shader at all), `decl` for a
   declaration that is not the client's skinned model layout, `depth` for a pass
   that writes no colour (the client's own shadow map above all), `never` for a
   pass this project identified as never a model (UI, rain, water), `vs` for a
   model draw this build could not prove at all, `cam` for the frame camera's own
   viewport, `world` for world-shaped constants with no bone block at `c31`,
   `pal` for a palette that failed validation - including a c31 that is not a
   bone block at all, which is what a client that keeps its bones somewhere else
   looks like - `geo` for vertices that could not be read,
   `shape` for a model whose own vertices reference a body's skeleton or whose
   shape is not a plant's, `size` for one outside the supported size range,
   `ground` for one whose geometry hangs below the point it is placed at, `move`
   for a model the client is animating, `far` for range, `calm` for a plant the wind's own
   gust field leaves with nothing to apply, `write` for a refused device write;
   the list continues on the next row if it does not fit, and a `waiting` count
   on the row above means those models are being measured, not refused). The
   head of that row reads `h 4.2 yd, 214 models`: the size the gate last
   measured, and how many draws arrived as the client's skinned model path at
   all - a small number there, with the field full of grass, means the client is
   not drawing that grass as models, and no gate tuning can change that.
   `new` above zero means a shader
   variant was proven this frame: `/log ModernWoWRenderer.log` names its hash, and
   adding that hash to the captured list is what makes the next session accept it
   without reading constants.
2. Set `SWAY STRENGTH` to 0 and back, then `VEGETATION SWAY` off and on. Nothing
   jumps; off is indistinguishable from the renderer without the feature.
3. Set `DRIVE PLANTS & TREES` off. Plants stand still, waves and clouds keep
   moving, and the wind keeps its heading. Turn it back on: nothing re-anchors.
4. Turn `MAX LEAN` to 30 degrees with a storm running: plants lean hard but never
   fold or stretch, and two batches of one tree stay welded.
5. Walk through a populated area: characters, creatures, mounts, weapons and
   animated props must not sway. If one does, note the model and leave
   `SWAY ANIMATED MODELS` off.
6. Rapidly turn the camera, Alt+Tab for a minute, F12, and change zone. No plant
   jumps, no lean snaps back, no graphics-state leak: close the menu and confirm
   the world still renders exactly as before.
7. With a location override active, change a `[Vegetation]` value for one subarea
   and confirm only that area changes.

## Known limitations

- The instance and geometry caches hold 2048 placed models and 2048 batches
  (`kMaxInstances`, `kMaxGeometryProfiles`). A frame that places more than that
  drops the least recently seen entries, and a dropped instance has to repeat
  itself twice again before it leans - which shows up as a `waiting` count on the
  state row rather than as silence. The per-frame sweep is what keeps the maps
  small in practice; the caps are only the floor under a flood.
- The client's skinned M2 path sways through its bone palette, and the rigid
  bone-less blade meshes sways through their own vertex shader (see *The grass
  that has no bones*). Anything else the client deforms itself - a shader that
  reads no texture coordinate, a draw that is not a green cut-out - is untouched,
  and a blade draw is never bent if its shader cannot be disassembled and
  reassembled on the machine.
- The client's own shadow pass is not leaned: it is one of the never-a-model
  passes, it writes no colour, and it renders into its own 2048x2048 viewport, so
  a leaning tree's native shadow stays with the client's pose. This is the
  conservative choice - a lean built in the wrong camera's space would be a
  visibly broken shadow, not a missing one. Whether that pass should lean too is
  a separate decision in `TODO.md`.
- A foliage variant outside the captured list sways as long as its draw carries
  the client's skinned model declaration - the renderer reports the hash once in
  `ModernWoWRenderer.log` and counts it as `new` on the HUD row, so the captured
  list can be extended and the next session accepts it without any proof at all.
  A draw that is *not* the model path stays still, by design.
- A model the client animates is skipped even if it is a plant, unless
  `SWAY ANIMATED MODELS` is enabled. A zone whose foliage is animated therefore
  needs that switch - and accepts that a motionless creature can lean.
- The wind is read as a single world-space vector; there is no terrain channeling
  or shelter, exactly like the waves and clouds. A sheltered valley sways as much
  as an exposed ridge.
