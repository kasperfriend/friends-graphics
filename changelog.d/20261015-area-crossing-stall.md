---
area: renderer
type: fixed
audience: players
title: Crossing into a new area no longer stalls the game for a second
---
Walking between areas used to re-read every graphics setting and the whole F7 menu whenever the client's idea of your place flipped across a border, which can happen several times a second while you walk along it. That tuning now applies once the new place has settled: one quick reload per crossing.
