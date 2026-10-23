---
area: renderer
type: fixed
audience: players
title: Reduce broad sun glare and diagnose cloud height
---
The radial glare mask now treats the depth-matched tracked sun disc as its light source instead of an occluder, while nearer geometry still blocks rays. Compact halo falloff and cloud transmission remain. Cloud diagnostics report the camera's position relative to the fixed layer.
