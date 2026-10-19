---
area: renderer
type: fixed
audience: players
title: Underwater - the 2x2 grid is gone and dirty water no longer shows a clear sky
---

The dirt lattice kept a vertex pinned under the camera - a cross in every downward view - and the caustic net had seams where its noise wrapped. The patch is now offset mid-cell, the net is truly periodic, and exiting rays pay the turbidity, so dirty water shows the sky's glow, not its picture.
