---
area: renderer
type: fixed
audience: players
title: Smooth cloud patterns in upward sky views
---
The screen-only cloud fallback now uses a broad angular noise scale instead of the 42 km planet-radius projection, reducing tiny overhead patches and gaps. Logs also report moon-texture candidate transforms to diagnose missing celestial tracks without loosening acceptance checks.
