---
area: renderer
type: fixed
audience: players
title: Dynamic lights no longer paint copies of nearby characters and objects into the pool
---
Lamp pools near characters and objects used to show dark shape-copies of them; LIGHT SMOOTHING only softened the copies. The light now ignores a nearby object's edge instead of being steered by it. Terrain shading and the slider work as before.
