---
area: renderer
type: fixed
audience: players
title: The last copies of nearby objects around lamp pools are gone
---
A soft shape-copy of a nearby character or object remained in the light's pool around it, growing with LIGHT SMOOTHING. The light now discards a measurement that hits something standing off the ground, or a ray that skims the horizon tens of yards away. The copies are gone at every smoothing level.
