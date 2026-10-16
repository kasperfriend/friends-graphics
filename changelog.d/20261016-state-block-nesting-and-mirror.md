---
area: renderer
type: fixed
audience: players
title: State-block passes no longer leak their shaders and blend flags into world classification
---
A D3D9 state block restores the device without running the proxy's
Set-shader and Set-render-state hooks, so every block-based pass was
leaving the renderer's internal state mirror showing the pass's own
shaders, blend and depth flags. The next world draw then classified
itself against those leaked values: UI detection, rain-particle
recognition, camera capture gating and the liquid-signature shader
hashes could all run on the wrong identities right after any effect
pass. The mirror is now saved with the block capture and restored with
the block apply, for every pass, in the same transaction the UI
drawing already used.

The same audit found that the shared block was a single slot, so two
passes running at once (the weather composite contains the ambient
occlusion, cloud and local-light passes) would have let the inner pass
overwrite the outer one's snapshot. The shared block is now a small
stack - one block per nesting level, created once per device - so
nested passes each keep their own snapshot and stay on the fast path,
and the two remaining legacy volume passes (the weather composite and
the late local-light pass) converted to state blocks as well: another
~340 driver calls per composited frame gone, with the exact legacy
save still the fallback on devices without state blocks.
