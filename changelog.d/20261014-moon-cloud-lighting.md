---
area: renderer
type: fixed
audience: players
title: Cloud lighting no longer follows the camera at night
---
Looking at the moon made the clouds' light swing around as the camera turned, because the moon's direction was read against the billboard that draws it instead of the world. Moonlight now stays fixed in the sky, so panning the camera leaves cloud lighting where it belongs.
