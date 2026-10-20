---
area: renderer
type: fixed
audience: players
title: Turning no longer disturbs the dynamic lights
---
Which dynamic lights are drawn no longer depends on which way you face, so turning left and right can no longer drop, swap or flicker them. Distance and brightness decide; a frame without a fresh camera pose still holds the pose it has.
