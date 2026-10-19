---
area: renderer
type: fixed
audience: players
title: Effects work from the first frame, not after a teleport
---
Depth and camera capture are now wired on every device, and the camera no longer waits for a captured depth buffer. Clouds, shafts, torches, ambient occlusion, reflections and water start on the frame you enter the world. A failed water shader also retries after two seconds instead of staying off.
