---
area: renderer
type: fixed
audience: players
title: Reduce broad sun glare and diagnose cloud height
---
Radial rays open the tracked sun at matched depth. Sky-only tracking reuses a recent validated camera projection only on the same full-screen viewport, pairing it with its captured depth mapping. Logs expose cache, eye, and celestial-draw evidence; debug markers compare glare and vanilla sun/depth.
