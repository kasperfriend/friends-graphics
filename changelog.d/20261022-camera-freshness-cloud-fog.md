---
area: renderer
type: fixed
audience: players
title: Clouds and fog stay in sync as you turn
---
Sky fallback retains the moon's confirmed world direction and no longer lets a conflicting disc veto a continuous view. Clouds, fog and precipitation fallback require a fresh camera, preventing stale-pose fog-only frames while preserving menu/out-of-world suppression.
