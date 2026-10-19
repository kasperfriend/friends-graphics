---
area: renderer
type: fixed
audience: players
title: Water opacity now really changes what you can see through
---
Water is now composited with a normal alpha blend, so its transparency actually reaches the screen: raising WATER OPACITY hides the seabed instead of only replacing the old water texture. A missing or implausible depth reading can no longer switch the slider off.
