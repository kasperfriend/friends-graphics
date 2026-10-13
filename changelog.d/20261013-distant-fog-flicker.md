---
area: renderer
type: fixed
audience: players
title: Distant fog no longer flickers while you move
---
The fog keeps its smoothing between frames now. Two checks threw it away almost every frame while you moved: one compared depth at two different points, which on far ground seen at a shallow angle never matched; the other discarded it when nearby lights were listed in a different order.
