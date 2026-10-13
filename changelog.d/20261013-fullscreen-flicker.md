---
area: renderer
type: fixed
audience: players
title: The picture no longer flickers faintly across the whole screen
---
Fog, clouds and sky light were timed in coarse steps, so the haze pulsed instead of gliding, and frame smoothing was dropped for a frame whenever nearby lamps changed. Both are fixed: the picture now stays steady.
