---
area: renderer
type: fixed
audience: players
title: Waves no longer race across the water after a teleport
---
The sea re-anchors its heading and wave size only about a camera it can see this frame. After a teleport it used the previous zone's camera, sweeping every crest across the surface until the heading settled. It now holds the lattice for the frames without a camera.
