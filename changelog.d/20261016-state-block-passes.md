---
area: renderer
type: changed
audience: players
title: Per-pass device state saved and restored with one state block instead of ~170 calls
---
Every compositor pass (atmosphere, ambient occlusion, global illumination, clouds, water) used to save and restore around 170 individual device state calls per entry and exit. Each pass now stores one state block and restores it in one call: the same end state at a fraction of the driver traffic.