---
area: renderer
type: fixed
audience: players
title: The picture no longer flickers faintly across the whole screen
---
Two causes are gone: fog, clouds and sky light were timed by a coarse clock that advanced in steps instead of gliding, and the fog's frame smoothing was thrown away for a frame whenever the lamps around you changed, which happened constantly near lamp posts and at low sun. Both now keep a steady, smooth image.
