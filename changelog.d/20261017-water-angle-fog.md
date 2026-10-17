---
area: renderer
type: fixed
audience: players
title: Water no longer fades to a flat color at certain camera angles
---
Some client water shader variants do not keep fog parameters in the expected constant. The replacement now checks that input and uses an unfogged fallback when it is invalid, instead of fogging the entire water surface to a flat color.
