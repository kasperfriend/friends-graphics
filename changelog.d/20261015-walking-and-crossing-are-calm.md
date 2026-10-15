---
area: renderer
type: fixed
audience: players
title: Grass no longer shakes as you walk, and area changes no longer hitch
---
Grass sway is built from the position the client's own shader computed, so walking no longer jitters the field; a border crossing stops re-reading every setting and F7 row from disk, and a place whose settings resolve to the ones already in force costs nothing - the log says 0 us.
