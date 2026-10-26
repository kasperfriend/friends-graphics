---
area: renderer
type: fixed
audience: players
title: Keep night clouds visible and more dimensional
---
Cloud ambient lighting no longer applies the day/night level twice, preventing unnecessary near-black shading. The safe sky-only fallback samples the cloud profile through a short direction-space volume for more natural structure without using a camera eye or world shadow.
