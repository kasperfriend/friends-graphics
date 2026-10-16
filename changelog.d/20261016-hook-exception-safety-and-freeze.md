---
area: renderer
type: fixed
audience: players
title: A zone-change hiccup can no longer freeze the image with the audio still running
---
When a zone change made the game reset its render device, a thrown exception could unwind through the hooked driver calls and leave the image frozen while the audio kept running. Every hooked entry now catches and logs its own failures, so the worst case is a skipped effect pass, not a frozen frame.