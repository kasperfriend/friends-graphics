---
area: renderer
type: fixed
audience: players
title: Plants and trees now sway on every client build
---
The sway no longer depends on recognising one particular set of shader binaries, so grass, bushes and tree canopies lean on any build of the client. The shadow map and other depth-only passes stay untouched, and the diagnostic row now names every check that refused a draw, with its count.
