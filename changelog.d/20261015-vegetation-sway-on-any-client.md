---
area: renderer
type: fixed
audience: players
title: Plants and trees now sway on every client build
---
The sway no longer depends on recognising one particular set of shader binaries, and it no longer trusts a cached copy of which shader the client bound: grass, bushes and tree canopies lean on any build. Depth-only passes stay untouched, and the debug rows name the build that drew them.
