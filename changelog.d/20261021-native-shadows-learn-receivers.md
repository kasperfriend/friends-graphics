---
area: renderer
type: fixed
audience: players
title: Shadow softness and strength work on every shadow receiver again
---
Both shadow sliders used to reach only the small set of shaders the mod had been taught by hand, so a client update could leave them doing nothing at all; the mod now reads the shader itself to find the shadow receivers, and its log names the one step it could not take if a slider still does nothing.
