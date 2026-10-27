---
area: renderer
type: changed
audience: players
title: Use the active camera pose to anchor overhead clouds
---
When world draws lack a full-frame camera, supported 3.3.5 clients now provide a validated active-camera position and matching orientation together. Existing in-world, viewport and continuity checks remain, and normal world-space clouds stay the primary renderer.
