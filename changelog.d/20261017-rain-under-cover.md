---
area: renderer
type: fixed
audience: players
title: Rain no longer covers the whole screen when you are standing under a roof
---
The factor meant to fade rain out indoors was never set, so it stayed at zero and rain drew at full strength over caves, inns and arcades. The game's own weather particles now drive it, and the low mist no longer floods the screen when depth is unavailable.
