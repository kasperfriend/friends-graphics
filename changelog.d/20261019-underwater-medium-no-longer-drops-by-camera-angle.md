---
area: renderer
type: fixed
audience: players
title: The underwater look no longer depends on where the camera points
---
The underwater fog, caustics, beams and grade no longer vanish at some camera angles. A frame whose draws carried no fresh camera pose (looking up at the surface, at a wall, in a menu) now holds the last one for up to a second instead of switching the medium off.
