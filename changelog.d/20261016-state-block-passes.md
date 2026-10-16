---
area: renderer
type: performance
audience: players
title: Per-pass device state saved and restored with one state block instead of ~170 calls
---
Every compositor pass (atmosphere, ambient occlusion, global illumination,
dynamic lights, the cloud deck, the weather scene copy, the post-process
composite), every per-terrain-draw ground capture and every water tile
wrapped its work in a full manual state save: each pass crossed the D3D9
driver roughly 170 times just to remember and later restore the render
states, textures, samplers, shaders and vertex format the pass was about
to borrow - about a thousand driver calls per frame before a single
effect pixel was drawn.

Those saves are now one `Capture()` on a shared D3D9 state block
(`D3DSBT_ALL`) per pass and one `Apply()` on the way out - the block
holds every render state, all eight texture stages, all sampler states,
both shaders, the vertex format and the stream bindings at once. Only
the state a block cannot hold - the render target, the depth surface,
the viewport and the pixel-constant span - is still saved and restored
manually, in the same order the old code used. On a device that reports
no state block support the pass falls back to the exact legacy save, so
behavior is identical everywhere and the fast path is pure
optimization.

Two correctness details the block path needed: the shared block is
created once per device and a device change or an orphaned block
(after a device reset) recreates it before capture; and the block's
owning device is pinned with a reference, because a raw pointer cannot
tell "the same device" from "a new device allocated at the address of
a destroyed one" - a stale block captured through a reused address
would have applied its state to the wrong device. No visual, log or
diagnostic behavior changes; the passes set and restore exactly the
state they always did, in fewer driver crossings.
