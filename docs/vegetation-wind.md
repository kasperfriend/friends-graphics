# Wind-driven vegetation sway

Grass, bushes, plants and trees lean and shimmer in the shared wind. The wind
itself is not new: the sway is a consumer of the one world-space `WindState`
that already moves the sea, the cloud deck and the precipitation continuity
layer, and it creates no wind, clock, noise texture, render target or shader of
its own.

- **F7 → WIND → `VEGETATION SWAY`** turns the effect off and on; the link itself
  is **F7 → WIND → `DRIVE PLANTS & TREES`**, next to `DRIVE WAVES` and
  `DRIVE CLOUDS`.
- Settings live in the `[Vegetation]` section of `GraphicsEffects.ini` and obey
  the same per-location overrides as every other section (global, zone, subarea).
- The optional diagnostic **F7 → WIND → `VEGETATION DEBUG VIEW`** adds two rows
  under the wind HUD, readable with F7 closed. The first says how many models are
  leaning out of how many foliage draws arrived (and how many solid model draws
  were passed over); the second names **the gate that refused something**, and
  only the gates that refused something, so "nothing sways" is answered on the
  spot instead of guessed at. The gates are named in the order they run - `decl`
  (a declaration that is not the client's skinned model layout), `world` (the
  WMO/terrain projection: scenery), `vs` (a shader this build could not prove is
  a model), `geo` (the draw's vertices could not be read), `depth` (a pass that
  writes no colour), `cam` (no frame camera, or another view's viewport), `pal`
  (a palette that failed validation), `shape`/`size` (not a plant), `move` (the
  client is animating it), `far` (beyond the range), `new` (shader variants
  proven this frame, with their hashes in `ModernWoWRenderer.log`). They are also
  drawn while the wind HUD itself is open, so the vegetation counts are visible
  from the wind tab.

## How it works

The client's skinned M2 vertex shader cannot be replaced - it carries the
client's own lighting, fog and texture stages - so the sway is applied where the
client already keeps a model's animation: **in the bone matrices it uploads for
the draw**.

1. A draw is examined only while it is inside the shared wind's snapshot and the
   frame has an accepted camera pose, and only if its material is transparent -
   alpha tested or alpha blended, which is how the client draws leaves, grass
   and cloth. A solid draw is not foliage and cannot reveal that a model is a
   plant; it is only leaned once its model is known, which is what keeps a
   tree's opaque trunk welded to the canopy above it.
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
4. The material has to be transparent (alpha tested or alpha blended), and that
   is read **from the device**, not from the renderer's state mirror: the
   client's own passes can arrive through cached state blocks, which never reach
   the `SetRenderState` hook, and a foliage draw the mirror believes is solid is
   never revealed as an instance at all. A pass that writes no colour is refused
   for the same reason - the client's native shadow map is a depth-only pass
   whose palettes live in the light's space, so leaning it would rotate shadows
   by the frame camera's wind.
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
   root orientation, and its size. The size is measured from the draw's **own
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
8. Every validated bone gets **one** rigid rotation about that pivot, taken from
   the shared wind: the same transform for the whole model, so no two batches can
   be pulled apart, and no distance from the pivot changes, so a stem bends
   instead of stretching.
9. The leaned blocks are written into the same c31 registers for the draw, and
   the client's own matrices are **restored immediately after it**, in the same
   call, so the next draw and any state the game applies afterwards see exactly
   what it uploaded.

Nothing else is touched: no render state, no shader, no constant above c31, no
render target.

## What it refuses to touch

This is the part that keeps the effect safe, and every rule is enforced before a
single byte is written:

- **Master wind off, `DRIVE PLANTS & TREES` off, `VEGETATION SWAY` off, or F11
  graphics off.** No palette is read at all. Disabling a link restores exactly
  the behaviour that existed before this feature for that consumer, and never
  resets the shared wind for the others.
- **Models the client is already animating.** Two readings have to agree: the
  skeleton's world pose, *and* the world positions of the model's own vertices
  (which is what a particle or effect batch, re-uploaded every frame with new
  data, fails even when every matrix it binds is still). A static doodad repeats
  both exactly from any camera; a character, creature, mount or animated prop
  never does. A model whose motion is ambiguous **never** sways: it is dropped
  the moment the two readings disagree.
- **Full skeletons.** More than 24 bones in the validated palette is a
  character, creature, mount or weapon, not a plant.
- **Anything too small or too large** (below 0.15 yd or above 90 yd tall), and
  anything clearly longer than it is tall (more than 2.5x, with 3 yd of slack) -
  a fence and a wall are not plants.
- **Anything beyond `SWAY DISTANCE`.** The sway reaches full strength at 75% of
  that range and fades to nothing at the range itself, so nothing pops.
- **Solid materials.** A draw with an opaque material never reveals an instance,
  and neither does any draw outside the shared wind's snapshot, so the whole
  discovery path is limited to the client's own transparent model materials. The
  material verdict is read from the device, so a pass that arrives through a
  cached state block is judged by what the device is actually doing.
- **Depth-only passes.** A draw that writes no colour - the client's native
  shadow map above all, but also its occlusion fills and depth fills - is
  refused: nothing visible is being drawn, and the shadow cascades skin in the
  light's space, so the frame camera's wind on them would be a visibly wrong
  shadow.
- **Anything that is not the client's skinned model path.** Terrain, WMOs,
  particles, ribbons, effects, the interface and every fixed-function or
  up-front batch declare no bone indices (or no vertex buffer at all), so they
  are left alone before a palette is even read. So is the world's own
  projection: a draw whose constants bind `c4..c7` the way terrain and WMOs do
  is scenery, whatever its declaration says.
- **The passes this project has already identified as never-a-model** - the
  native shadow casters, UI, precipitation and water - are refused by hash
  before any constant is read.
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
- strength, size classes, gains, range fade, flutter, and all three switches
  (master wind, the link, the effect);
- admission: every captured model variant is accepted on sight, the
  never-a-model list (shadow casters, UI, precipitation, water) is refused and
  does not overlap the captured lists, the client's model projection is
  recognized while a terrain/WMO-style or transposed one is not, the world's own
  `c4..c7` projection is refused, and a declaration without bone indices is not
  a model draw whatever its shader is;
- the two proofs the gates read from the device rather than from the state
  mirror: the material (alpha test or alpha blend) and the colour-write mask -
  with the mirror left to the free early counters, the palette read kept above
  every device call it can be, and the declaration gate ordered before the
  shader gate because the declaration is what proves the draw is a model;
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
  in, and both rows fit the box they are drawn in;
- every counter the HUD can report is incremented by the device half, so a gate
  that refuses silently is a test failure rather than a bug report;
- the guard sits above the first palette read in the draw scope, and the four
  draw entry points each describe their own batch exactly once;
- INI/F7/default parity for every `[Vegetation]` key and for
  `[Wind] AffectVegetation`, no duplicated key in any section, and the production
  wiring itself (four draw hooks, one update next to the wind snapshot, the reset
  and reload paths, and that the lean is written over c31 and then restored).

In-client acceptance checks (Windows and the Ascension client):

1. Stand in a forest at day and at night, with the F7 WIND tab open on
   `VEGETATION DEBUG VIEW`. Grass, bushes and tree canopies lean downwind and
   shimmer; `leaning` rises with the number of visible plants, and if it does not,
   the second row names the gate that refused them (`decl` for a declaration that
   is not the client's skinned model layout, `world` for the scenery projection,
   `vs` for a shader this build could not prove, `geo` for vertices that could not
   be read, `depth` for a pass that writes no colour, `cam` for the frame
   camera's own viewport, `pal` for a palette that failed validation,
   `shape`/`size` for a model that did not look like a plant, `move` for a model
   the client is animating, `far` for range). `new` above zero means a shader
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

- Only the client's skinned M2 path sways. Terrain-bound grass that the client
  does not draw as a model, and anything the client itself deforms in a vertex
  shader outside the bone palette, is untouched.
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
