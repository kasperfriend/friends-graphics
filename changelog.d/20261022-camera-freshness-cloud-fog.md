---
area: renderer
type: fixed
audience: players
title: Clouds and fog stay in sync as you turn
---
Camera capture now accepts valid infinite-far perspective views (P22=1), and fallback logs the projection when it rejects a view. Full-frame, rigid-basis, viewport and eye checks remain; held poses still cannot render world-anchored clouds. Menu/out-of-world suppression remains.
