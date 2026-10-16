---
area: renderer
type: fixed
audience: players
title: Crossing into a new area no longer costs the game a second
---
A crossing used to re-read every graphics setting and the whole F7 menu each time the client's idea of your place flipped - about a fifth of a second of stall. Those reads now come from a cached section, dropped by any edit, so a saved toggle is still what the next read answers.
