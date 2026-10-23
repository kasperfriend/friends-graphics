---
area: renderer
type: fixed
audience: players
title: Reduce broad sun glare and diagnose cloud height
---
Radial rays open the tracked sun at matched depth. Sky-only cloud tracking reuses perspective only when viewport/FOV match and rejects expired eyes. Logs expose eye/projection evidence; the debug marker compares glare with the vanilla sun and colors the sun-depth match.
