---
area: renderer
type: fixed
audience: players
title: Crossing a zone border no longer causes a multi-second hitch
---
Crossing a zone border used to stall the game for several seconds while every effect re-read the zone settings from disk. The settings are now loaded once and kept in memory, so changing zones is smooth again while per-zone water, atmosphere and F7 presets behave exactly as before.
