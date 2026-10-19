---
area: renderer
type: fixed
audience: players
title: First dive of a session could freeze rendering
---
Going underwater for the first time could stop the picture while the game kept running: a cold shader compile and per-chunk geometry reads were all waiting on that frame. Both are now spread over ordinary frames, so a first dive costs nothing.
