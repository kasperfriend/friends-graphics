---
area: renderer
type: fixed
audience: players
title: Nearby dynamic lights stop flickering as you walk
---
Only a handful of lights fit in the light table at once, and the boundary used to be redrawn every frame, so lamps of similar strength traded places as you moved. A light now keeps its place until a challenger is clearly stronger, and every entry fades in or out instead of switching on and off.
