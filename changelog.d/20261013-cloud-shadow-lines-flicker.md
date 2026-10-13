---
area: renderer
type: fixed
audience: players
title: Cloud shadows no longer streak, slide or blink with the camera
---

The ground cloud-shadow map was only 256² and marched the sun path with 8 coarse samples, so low sun angles smeared it into lines near the light and a small camera-centred coverage disc let the shadow blink in and out as you moved. It now renders at 512² with a 16-step march, a wider soft edge, and roughly double the coverage radius, so the shadow stays put in the world and the sun no longer appears to drift.
