---
area: renderer
type: fixed
audience: players
title: Local-light smoothing now supports a one-texel minimum
---
F7 LIGHT SMOOTHING now reaches 0%, removing only the optional world-space spread floor while retaining the required depth-texel footprint. Periodic local-light diagnostics also identify selected sources by type, ID, position and score, so overlapping attached flame emitters can be investigated from a runtime log.
