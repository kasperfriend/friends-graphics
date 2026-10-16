---
area: renderer
type: fixed
audience: players
title: Dynamic light pools no longer sit under a layer of grain
---

Lamp glow on ground used to break into pixel-sized speckle at a distance, worst at LIGHT QUALITY 0, and no setting removed it. The light now reads the ground's true slope instead of a leftover of its own window, so pools stay smooth. LIGHT SMOOTHING trades detail for softness.
