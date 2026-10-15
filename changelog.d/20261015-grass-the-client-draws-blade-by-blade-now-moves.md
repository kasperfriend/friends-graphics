---
area: renderer
type: added
audience: players
title: Grass the client draws blade by blade now moves with the wind
---
Ground grass is drawn as one rigid mesh with no bones to bend, so it never moved. It is now bent inside the client's own vertex shader, reassembled from the client's compiled code: whole fields ripple and sway as the wind passes, with a switch and a debug row of its own.
