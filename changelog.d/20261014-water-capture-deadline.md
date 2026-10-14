---
area: renderer
type: fixed
audience: players
title: Water no longer captures itself when camera setup arrives late
---
If camera data is missing when world water first draws, reflections and refraction wait for the next frame rather than copying a scene that already contains water.
