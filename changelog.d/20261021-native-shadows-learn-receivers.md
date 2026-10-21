---
area: renderer
type: fixed
audience: players
title: Shadow softness and strength work on every shadow receiver again
---
Both shadow sliders used to reach only the small set of shaders the mod had been taught by hand, so a client update could leave them doing nothing at all. The mod now recognises a shadow receiver from the shader itself, which also stops it from adjusting shaders that have nothing to do with shadows.
