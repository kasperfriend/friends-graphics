---
area: renderer
type: fixed
audience: players
title: Cloud startup no longer rereads the noise cache every frame
---
When cloud noise is being generated in the background, the renderer now waits without repeatedly opening and scanning the cache file on every frame.
