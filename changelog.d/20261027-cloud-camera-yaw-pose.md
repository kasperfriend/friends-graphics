---
area: renderer
type: fixed
audience: players
title: Use the active camera pose to anchor overhead clouds
---
When full-frame world draws do not provide a camera, supported 3.3.5 clients can supply a stable active-camera position and matching world-view orientation together. The renderer validates the pose and keeps its existing in-world, viewport, and continuity guards; this does not replace normal world-space cloud rendering with the sky-only recovery.
