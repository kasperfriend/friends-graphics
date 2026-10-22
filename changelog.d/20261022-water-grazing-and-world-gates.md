---
area: renderer
type: fixed
audience: players
title: Water stays stable at grazing angles, and fog stays out of character select
---
Water now uses safe fallbacks instead of clamped edge pixels when a side-on view sends a depth or refraction sample outside the screen. Underwater and atmospheric fog, plus world clouds, require playable-world evidence; they no longer carry stale scene state into login or character-select screens. When the replacement water shader is turned off, native water is coverage-marked so the full-screen atmosphere composite no longer fogs those water pixels away.
