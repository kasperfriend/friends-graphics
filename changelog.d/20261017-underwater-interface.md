---
area: renderer
type: improved
audience: players
title: Underwater views now treat the water surface as a real optical boundary
---
From below the surface, the shader now adds the water-to-air critical-angle reflection and attenuates light along the camera-to-surface water path. Above water the new path is an exact identity, preserving the existing surface look.
