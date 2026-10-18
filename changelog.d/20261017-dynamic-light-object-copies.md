---
area: renderer
type: fixed
audience: players
title: Edge filtering reduces nearby-object copies in lamp pools
---
Lamp pools near characters and objects can show shape-copies when their depth steers the reconstructed normal. The edge filter reduces that influence at ordinary smoothing, but wide LIGHT SMOOTHING windows can still make residual copies worse; increasing smoothing is not a fix.
