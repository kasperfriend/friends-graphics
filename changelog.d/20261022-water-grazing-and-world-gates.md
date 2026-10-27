---
area: renderer
type: fixed
audience: players
title: Water stays stable at grazing angles, and fog stays out of character select
---
Side-on water views now use safe fallbacks for off-screen depth and refraction samples. Fog, underwater effects, and clouds are gated to playable-world scenes, preventing menu leaks. Turning off replacement water now preserves native water against the atmosphere composite.
