---
area: renderer
type: changed
audience: players
title: Dynamic light pools are smoother and steadier, with a small driver-call reduction
---
The surface normal a dynamic light uses on the ground is now a central difference with a depth-edge guard: pools stop dappling and shimmering with the ground texture, while cliffs and object bases still light from the correct side. F7 LIGHT DEBUG mode 1 shows the raw source contribution again.