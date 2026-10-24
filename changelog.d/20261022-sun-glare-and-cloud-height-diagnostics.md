---
area: renderer
type: fixed
audience: players
title: Reduce broad sun glare and diagnose cloud height
---
Radial rays open the tracked sun at matched depth. Sky-only cloud tracking reuses a recent validated perspective only for the same viewport/depth range and rejects expired eyes. Logs expose eye/projection evidence; debug markers compare glare and vanilla sun/depth.
