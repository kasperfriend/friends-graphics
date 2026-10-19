---
area: renderer
type: fixed
audience: players
title: Display changes no longer retain stale graphics resources
---
Fullscreen, resolution and device changes now release cached render-state resources before reset. Cloud and volumetric effects also rebuild cleanly on replacement devices without recompiling shaders for ordinary resizes.
