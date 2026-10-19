---
area: renderer
type: fixed
audience: players
title: Dynamic lights no longer flicker when you turn
---
Dynamic lights no longer flicker or drop out as you turn the camera. Which lamps are drawn no longer depends on where you are looking, and a frame without a fresh camera pose holds the last one for up to a second instead of skipping the pass.
