---
area: renderer
type: fixed
audience: players
title: Underwater - no white login, dirty water fogs its plane, underside keeps water's colour
---

Launching could stall on a white login screen while the underwater shader compiled in the first frame; preparation now happens off the render thread. Dirty water also fogs the far shore out through its surface, and a lake's underside keeps its own colour instead of the sky's.
