---
area: renderer
type: fixed
audience: players
title: A zone-change hiccup can no longer freeze the image with the audio still running
---
When a zone change made the game re-create or reset its render device, a
rare error in the proxy's own bookkeeping (a log file that refused a
write, a settings read that failed, an allocation that threw) could
unwind out of a hooked D3D9 call. An exception escaping through the
driver's call dispatch is exactly how one bad frame becomes a
permanently frozen image while the game's audio and menu sounds keep
playing: the device is left mid-reset and the game's recovery path is
never finished.

Every hooked entry point is now exception-safe. Logging can no longer
throw at all, the reset path always runs the game's own reset even if
the proxy's cleanup fails, each draw hook re-runs the game's draw if
the effect pass threw before it ran (without double-drawing if it
already did), and the internal "we are drawing" flags that a
half-finished pass could have left stuck - which would have silenced
every effect for the rest of the session - are cleared on the way out.
A device that is re-created wholesale (driver recovery, full render
re-init) is now re-hooked instead of being ignored, which previously
left every effect dead while the game itself kept running.

The per-draw light-space shadow pass is now built on the same state
block as the other passes (about half the driver calls per shadow
draw) and restores the framebuffer no matter what, so a failure there
can no longer leave the shadow map bound as the render target and
swallow the rest of the frame.
