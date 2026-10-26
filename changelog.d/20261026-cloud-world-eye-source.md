---
area: renderer
type: fixed
audience: players
title: Keep the normal cloud march active when looking straight up
---
On verified 3.3.5 clients, the current active-camera eye is read from the game when overhead views cull full-frame world draws. A continuity-leashed fresh eye keeps the normal world-space cloud march, local weather and shadows active; sky fallback is no longer the only path.
