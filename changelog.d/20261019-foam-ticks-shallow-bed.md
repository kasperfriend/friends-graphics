---
area: renderer
type: fixed
audience: players
title: Foam ticks on shallow sand tracked to depth precision and removed
---

Rows of dark ticks could appear in the foam over shallow sand on some graphics cards, where the depth the water reads back is stored less precisely. The water now judges the seabed's slope over a few yards instead of a single pixel, so the foam lies smooth on every card.
