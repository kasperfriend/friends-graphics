---
area: renderer
type: fixed
audience: players
title: Grass no longer shakes as you walk, and area changes no longer hitch
---
Grass sway is built from the position the client's own shader computed, so walking no longer makes the field jitter; crossing a border stops re-reading every setting and F7 row from disk, and settling on a place that is already in force costs nothing at all - the log line says 0 ms.
