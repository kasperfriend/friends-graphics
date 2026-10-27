---
area: renderer
type: fixed
audience: players
title: Clouds stay visible when the camera eye ages out
---
A verified sky-only pass keeps clouds visible after eye timeout without touching geometry, water, or world shadows; its tint follows the zone instead of bleaching white. Bright-blue night light uses a confirmed moon direction, eased in on first tracking.
