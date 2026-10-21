---
area: renderer
type: changed
audience: players
title: Clouds default to a sharper deck
---
The cloud buffer now defaults to 75% of the screen instead of 25%, and the raymarch defaults to ULTRA (128 steps) instead of HIGH (96). The deck's edges and the noise in it stop crawling, and the extra march steps pay for the larger buffer. Both remain independent on the VOLUMETRIC CLOUDS tab.
