---
area: renderer
type: fixed
audience: players
title: Local fog no longer floats above you in cities
---
The near fog layer takes its floor from the ground under the player. In a city, tilting the camera up used to move that floor onto a wall or a roof, lifting the whole bank overhead. It now ignores anything above eye level, holds its last value when nothing is underfoot, and never rises above you.
