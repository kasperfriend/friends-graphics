---
area: renderer
type: fixed
audience: players
title: Keep shore foam anchored when the camera moves
---
Foam now uses a global world anchor across translated water tiles, world-space seabed depth and slope, and exact per-pixel broad noise instead of view-depth gradients and triangle-interpolated foam. This prevents camera-driven movement and visible mesh seams; water optics remain unchanged.
