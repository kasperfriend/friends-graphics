---
area: renderer
type: fixed
audience: players
title: Grass no longer shakes as you walk, and area changes no longer hitch
---
Grass sway is built from the position the client's own shader computed, so walking no longer makes the field jitter, and crossing an area border stops re-reading every setting and F7 row from disk: a crossing is now one quick reload.
