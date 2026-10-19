---
area: renderer
type: fixed
audience: players
title: Underwater visuals never switched on
---
The underwater medium checked each water draw against state a shader-driven client leaves stale, so it never believed the camera was wet and the underwater toggle did nothing. It now reads the surface from the draw itself, so submersion fog, caustics and dirt engage when you dive.
