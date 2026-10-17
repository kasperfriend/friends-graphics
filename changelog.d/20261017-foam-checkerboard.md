---
area: renderer
type: fixed
audience: players
title: Sea foam no longer shows a grid of squares that moves with the camera
---
The foam's fine detail was pushed through a threshold inside the noise's own range, so nearly half of it clipped flat into squares one ripple cell across, printing that grid over the sea. It is now a smooth blend of the same two scales, with the same average coverage and the same darkest gaps.
