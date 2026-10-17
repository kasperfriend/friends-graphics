---
area: renderer
type: added
audience: players
title: Anti-aliasing with its own switch, so smooth edges no longer cost effects
---
Edges are smoothed by the renderer instead of by the game's multisampling, which has to stay at 1x: above that, no effect can read the depth buffer, and water, occlusion, lighting, rain and fog all lose it. Turn it on under IMAGE & COLOR; Sharpness puts detail back.
