---
area: renderer
type: changed
audience: players
title: Per-draw and per-frame hot path cleanup
---
Several paths the renderer runs on every draw and every frame no longer pay for work that cannot change their outcome: re-read state, redundant full-screen passes and per-call allocations are gone. The look is unchanged; frames are steadier in busy zones.
