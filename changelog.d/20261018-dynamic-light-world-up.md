---
area: renderer
type: fixed
audience: players
title: Local lights render reliably and debug more clearly
---
Direct lamp pools now render without another volume effect; volumetric scattering still needs the atmosphere's fog medium. A pitched-camera fix reduces nearby-object copies. Wide LIGHT SMOOTHING can still worsen them, so increasing smoothing is not a fix.
