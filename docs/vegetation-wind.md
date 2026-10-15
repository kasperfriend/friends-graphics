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
- The optional diagnostic line is **F7 → WIND → `VEGETATION DEBUG VIEW`**; it
  appears under the wind HUD and can be read with F7 closed.

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
2. The draw has to use the client's skinned model layout (a vertex declaration
   with `POSITION` float3, `BLENDINDICES` and `BLENDWEIGHT`) **and** one of the
   rigid skinned M2 vertex shaders this project has verified live - the captured
   list in `src/Lighting/TorchSignatures.h` that the torch effect also requires.
   A fixed-function draw, an FVF draw, an unskinned model variant and any
   unlisted shader (shadow-depth, portrait, UI, post-process) are all left
   alone, because they are not paths where a bone palette in the frame camera's
   own space has ever been proven.
3. The bone palette the client uploaded at **c31** is validated block by block:
   every block must be finite, orthogonal up to scale and hold a plausible
   translation. The first block that is not ends the palette; nothing past it is
   ever written.
4. The validated blocks are reconstructed into a world pivot (where the model
   meets the ground), a height and a footprint. The pivot, the height and the
   model's root orientation together identify one *instance*, which is what makes
   the sway follow a placed model rather than a draw call: the opaque trunk of a
   tree leans with the alpha-tested canopy that revealed it, because both belong
   to the same instance.
5. The instance's own phase - taken from its world identity, never from a frame
   counter - sets its flutter so two tufts a yard apart do not move in lockstep.
6. Every validated bone gets **one** rigid rotation about that pivot, taken from
   the shared wind: the same transform for the whole model, so no two batches can
   be pulled apart, and no distance from the pivot changes, so a stem bends
   instead of stretching.
7. The leaned blocks are written into the same c31 registers for the draw, and
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
- **Models the client is already animating.** A static doodad repeats its pose
  exactly, frame to frame, from any camera; a character, creature, mount or
  animated prop never does. A model whose motion is ambiguous **never** sways:
  it is dropped the moment two readings disagree.
- **Full skeletons.** More than 24 bones in the validated palette is a
  character, creature, mount or weapon, not a plant.
- **Anything too small or too large** (below 0.15 yd or above 90 yd tall), and
  anything clearly longer than it is tall (more than 2.5x, with 3 yd of slack) -
  a fence and a wall are not plants.
- **Anything beyond `SWAY DISTANCE`.** The sway reaches full strength at 75% of
  that range and fades to nothing at the range itself, so nothing pops.
- **Solid materials.** A draw with an opaque material never reveals an instance,
  and neither does any draw outside the shared wind's snapshot, so the whole
  discovery path is limited to the client's own transparent model materials.
- **Every shader that is not a verified rigid skinned M2 variant.** The
  client's unskinned M2 path keeps a single matrix in c31..c33 instead of a
  palette, and the shadow-depth, portrait, UI and post-process shaders have
  never been shown to hold a palette there - or to share the frame camera's
  space, which is what makes a lean about the world ground contact correct.
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
| `DebugView` | VEGETATION DEBUG VIEW | 0 | on/off | Extra diagnostic HUD line. |

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
- the shader whitelist: every captured rigid M2 variant is accepted, every
  unskinned variant and every unknown hash is not, and the gate sits above the
  first device call in the draw scope;
- INI/F7/default parity for every `[Vegetation]` key and for
  `[Wind] AffectVegetation`, no duplicated key in any section, and the production
  wiring itself (four draw hooks, one update next to the wind snapshot, the reset
  and reload paths, and that the lean is written over c31 and then restored).

In-client acceptance checks (Windows and the Ascension client):

1. Stand in a forest at day and at night, with the F7 WIND tab open on
   `VEGETATION DEBUG VIEW`. Grass, bushes and tree canopies lean downwind and
   shimmer; the counts rise with the number of visible plants and the "skipped"
   counters explain what was passed over.
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
- The client's own shadow pass is not leaned: it is not one of the verified
  palette shaders, so a leaning tree's native shadow stays with the client's
  pose. This is the conservative choice - a lean built in the wrong camera's
  space would be a visibly broken shadow, not a missing one. Whether that pass
  should lean too is a separate decision in `TODO.md`.
- Only the shader variants captured in `src/Lighting/TorchSignatures.h` sway. If
  a foliage variant in the client is not in that list it stays still; extending
  the list needs the variant's hash, which the diagnostic line's counters and
  the renderer log identify.
- A model the client animates is skipped even if it is a plant, unless
  `SWAY ANIMATED MODELS` is enabled. A zone whose foliage is animated therefore
  needs that switch - and accepts that a motionless creature can lean.
- The wind is read as a single world-space vector; there is no terrain channeling
  or shelter, exactly like the waves and clouds. A sheltered valley sways as much
  as an exposed ridge.
