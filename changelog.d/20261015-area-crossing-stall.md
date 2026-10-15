---
area: renderer
type: fixed
audience: players
title: Crossing into a new area no longer costs the game a second
---
Walking along a border used to re-read every graphics setting and the whole F7 menu each time the client's idea of your place flipped, several times a second there - about a fifth of a second of stall each time. Those reads now come from memory: one read per file, until the file itself changes.
