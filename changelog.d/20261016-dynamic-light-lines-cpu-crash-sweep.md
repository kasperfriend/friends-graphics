---
area: renderer
type: fixed
audience: players
title: Dynamic-light "lines" at quality 1 fixed, plus a CPU and crash-safety sweep
---
At DynamicLighting Quality=1 the light pool was drawn into a 0.75x
texture and blended back to full resolution. The blend's depth-aware
weights had no floor, so wherever the 1.33 px tap window straddled a
depth edge (an object base, a step, the water edge inside the pool)
the far side of the edge was weighted to zero and the pool snapped in
1-2 px steps along the light-map grid; a secondary fallback picked the
"nearest" tap, which switches on hard grid lines of its own. At
Quality=2 the light map is 1:1 with the scene so neither can trigger,
which is why the lines only appeared at quality 1. The depth mask now
keeps a bilinear floor and the result is always a bounded, continuous
weighted blend (identical to before wherever it was already smooth),
and the water-edge exclusion of the low-res pass is linear-filtered so
its boundary follows the mask instead of a texel grid.

The CPU sweep removed per-frame and per-draw work from the render
thread: the dynamic-light selection no longer allocates its candidate
list on every frame (one persistent buffer, sorted in place, swapped
into the result) and no longer samples the system clock once per
light; the torch-emitter observer no longer allocates its index list
and emitter clusters on every matched draw; and the frame diagnostics
no longer take a mutex on every classified draw call - the counters
accumulate lock-free on the render thread and commit once per frame,
with identical totals. The light smoothing cache, which gained a new
entry every time the game recycled a light slot, is now bounded.

Three background threads (the water shader compile, the cloud noise
generation and its worker pool) could each have terminated the whole
game if an ordinary failure - an allocation, a log file, a cache
write - threw: an exception escaping a std::thread kills the process.
They now publish their normal failure state instead, so a failed
compile or generation only costs that feature, and the two light-slot
hook observers that could throw out of a hooked D3D9 call are guarded
like the rest.
