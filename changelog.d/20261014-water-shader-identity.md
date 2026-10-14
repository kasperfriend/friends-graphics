---
area: renderer
type: fixed
audience: players
title: Water detection no longer inherits the identity of released shaders
---
Shader identification now keeps cached identities alive with a bounded cache and distinguishes recycled addresses, preventing stale water or UI classifications.
