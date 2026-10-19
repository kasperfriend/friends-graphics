---
area: renderer
type: changed
audience: players
title: Water checks cost far less CPU per frame
---

Every water tile on screen was measured triangle by triangle each frame to tell whether the camera is under it, even tiles hundreds of yards away. Distant tiles are now settled from their outline alone, which frees CPU on lake and ocean views with no visual change.
