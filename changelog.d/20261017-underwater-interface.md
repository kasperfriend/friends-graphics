---
area: renderer
type: changed
audience: players
title: Underwater views now treat the water surface as a real optical boundary
---
Below the surface, the shader now adds the water-to-air critical-angle reflection, absorbs light along the camera-to-surface water path, and keeps output coverage in step with its reflection so the opposite face cannot double-blend through. Above water the new path is an exact identity.
