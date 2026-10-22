---
area: renderer
type: fixed
audience: players
title: Clouds and fog stay in sync as you turn
---
Sky fallback retains the moon's direction and observes VIEW/PROJECTION changes before later passes replace them, logging candidate rejections. Full-frame, rigid-basis, viewport and eye checks remain; menu/out-of-world suppression is unchanged. Clouds, fog and weather still require a fresh pose.
