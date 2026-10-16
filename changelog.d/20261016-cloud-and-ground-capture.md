---
area: renderer
type: changed
audience: players
title: First-launch cloud generation moves off the render thread
---
The one-time cloud generation at a fresh launch no longer runs on the render thread, so a first frame with clouds cannot stutter, and the ground-fog capture no longer saves and restores device state it never touches. Nothing about the look changes.
