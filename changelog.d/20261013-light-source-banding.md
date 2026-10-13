---
area: renderer
type: fixed
audience: players
title: Light sources no longer show rings, spokes or stepped rays around them
---
The sun, moon and lamps no longer draw visible rings and spokes into their own glow. Smooth light falloff was being rounded to hard steps, the sun-ray blur sampled in lockstep, and cloud rays were sampled at fixed distances; all three now use a fixed per-pixel offset and a fine static grain, so halos fade smoothly instead of banding.
