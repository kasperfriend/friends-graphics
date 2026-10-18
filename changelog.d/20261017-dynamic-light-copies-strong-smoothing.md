---
area: renderer
type: fixed
audience: players
title: Tilted cameras can still reveal nearby-object copies in lamp pools
---
Edge filtering reduced nearby-object copies but not at every camera angle. A pitched camera can expose residual copies, and wide LIGHT SMOOTHING can make them worse. Measuring height against world vertical reduces the pitched-camera error; increasing smoothing is not a cure.
