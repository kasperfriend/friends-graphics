---
area: renderer
type: fixed
audience: players
title: Sea foam no longer shows a grid of squares that moves with the camera
---
The quality 7 mid-wave value was still imprinting its lattice in the foam after the first mix fix. The lace now uses the fine fleck and broad vertex-interpolated warp instead; mid waves still shape the surface, but no longer stamp their per-pixel grid into foam coverage.
