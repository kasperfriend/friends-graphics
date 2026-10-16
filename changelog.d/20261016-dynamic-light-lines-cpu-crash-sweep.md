---
area: renderer
type: fixed
audience: players
title: Dynamic-light "lines" at quality 1 fixed, plus a CPU and crash-safety sweep
---
At DynamicLighting Quality=1 the light pool snapped in 1-2 pixel steps wherever its blend straddled a depth edge (an object base, a step, the water edge); the blend now keeps a continuous floor, so the pool stays smooth at every quality. Effect failures are caught, so the game no longer freezes.